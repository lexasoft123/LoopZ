"""Explicit real signed-process/launchd rehearsal in one private GUI job.

No root privileges, real broker binary, public bridge, HAL or audio API is used.
The three non-broker components are file fixtures for the durable transaction.
"""

import hashlib
import json
import os
from pathlib import Path
import plistlib
import re
import shutil
import subprocess
import sys
import tempfile
import time
import uuid

ROOT = Path(__file__).resolve().parents[2]
sys.path.insert(0, str(ROOT / "scripts"))
from broker_host import BrokerInspector, BrokerPolicy
from broker_lifecycle import BrokerLifecycle
from install_routing import command
from install_transaction import files
from process_code import require_process_code, same_generation
from suite_files import SuiteFiles
from suite_update import BROKER, NAMES


def verify_fixture_code(path, identifier, requirement, engine):
    if engine:
        raise RuntimeError("The private process fixture must have no capture entitlement")
    command("/usr/bin/codesign", "--verify", "--strict", "-R", "=" + requirement, path)
    details = command("/usr/bin/codesign", "-d", "--verbose=4", path).stderr
    flags = re.search(r"^CodeDirectory .*flags=\S*\(([^)]+)\)", details, re.M)
    if (
        f"Identifier={identifier}\n" not in details
        or flags is None
        or set(flags[1].split(",")) != {"adhoc", "runtime"}
    ):
        raise RuntimeError("The private fixture must retain its explicit ad-hoc runtime signature")
    encoded = command("/usr/bin/codesign", "-d", "--entitlements", "-", "--xml", path).stdout
    if encoded.strip() and plistlib.loads(encoded.encode()) != {}:
        raise RuntimeError("The private fixture unexpectedly has entitlements")


def fixture_policy(root, label):
    identifier = "com.lexasoft.loopz.broker-fixture"
    return BrokerPolicy(
        program=root / "installed" / BROKER,
        definition=root / "broker.plist",
        domain=f"gui/{os.geteuid()}",
        owner=os.geteuid(),
        label=label,
        identifier=identifier,
        requirement=f'identifier "{identifier}"',
        mach_service="",
        arguments=(
            "--routing-document",
            str(root / "private-document"),
            "--editor-service",
            "true",
            "--block-frames",
            "64",
        ),
    )


def abrupt_worker(root):
    if (
        os.geteuid() == 0
        or root.parent != Path("/private/tmp")
        or not root.name.startswith("loopz-broker-process-")
    ):
        raise RuntimeError("Abrupt-exit worker requires the private user fixture namespace")
    info = root.lstat()
    if root.is_symlink() or info.st_uid != os.geteuid() or info.st_mode & 0o077:
        raise RuntimeError("Private worker workspace is not protected")
    label = plistlib.loads((root / "broker.plist").read_bytes())["Label"]
    if not re.fullmatch(r"com\.lexasoft\.loopz\.test\.broker-process\.[a-f0-9]{32}", label):
        raise RuntimeError("Worker cannot control a non-fixture job")
    policy = fixture_policy(root, label)
    inspector = BrokerInspector(policy, static_code=verify_fixture_code)

    def crash_after_bootstrap(*args, **kwargs):
        mutation = args[:2] in (("/bin/launchctl", "bootout"), ("/bin/launchctl", "bootstrap"))
        if mutation:
            with (root / "worker-operations.jsonl").open("a") as log:
                log.write(json.dumps([str(a) for a in args]) + "\n")
                log.flush()
                os.fsync(log.fileno())
        result = command(*args, **kwargs)
        if args[:2] == ("/bin/launchctl", "bootstrap") and result.returncode == 0:
            os._exit(77)
        return result

    inspector.run = crash_after_bootstrap

    def admit():
        if (root / "private-document").read_bytes() != b"fixture, never production routing":
            raise RuntimeError("Private worker document changed")

    lifecycle = BrokerLifecycle(inspector, admit)
    targets = {name: root / "installed" / name for name in NAMES}
    with SuiteFiles(root / "support", targets) as transaction:
        transaction.load()
        assert transaction.value["phase"] == "activating"
        backup = root / "installed" / transaction.sibling(BROKER, "old")
        original = inspector.inspect(backup, transaction.value["originals"][BROKER])
        lifecycle.stop(original, backup)
        lifecycle.ensure_started(transaction.value["candidates"][BROKER])
    raise RuntimeError("Abrupt worker did not reach its requested exit boundary")


def run(fixture, crash_after_bootstrap=False):
    if os.geteuid() == 0:
        raise RuntimeError("The private broker rehearsal must run as the logged-in user")
    # Read-only baseline. Process ownership inspection of the real root broker
    # needs administrator context and is deliberately not attempted here.
    root_job = BrokerInspector().job()
    root_definition = Path("/Library/LaunchDaemons/com.lexasoft.loopz.routing.plist")
    root_definition_hash = hashlib.sha256(root_definition.read_bytes()).hexdigest()
    root_broker_hash = hashlib.sha256(BrokerPolicy().program.read_bytes()).hexdigest()
    root = Path(tempfile.mkdtemp(prefix="loopz-broker-process-", dir="/private/tmp"))
    destination = root / "installed"
    destination.mkdir(mode=0o700)
    source = root / "source"
    source.mkdir(mode=0o700)
    support = root / "support"
    support.mkdir(mode=0o700)
    targets = {name: destination / name for name in NAMES}
    document = root / "private-document"
    document.write_bytes(b"fixture, never production routing")
    marker = document.read_bytes()
    identifier = "com.lexasoft.loopz.broker-fixture"
    for name in NAMES:
        if name == BROKER:
            shutil.copy2(fixture, targets[name])
            subprocess.run(
                [
                    "/usr/bin/codesign",
                    "--force",
                    "--sign",
                    "-",
                    "--identifier",
                    identifier,
                    "--options",
                    "runtime",
                    str(targets[name]),
                ],
                check=True,
                capture_output=True,
                timeout=15,
            )
            shutil.copy2(targets[name], source / name)
        else:
            for parent, prefix in ((destination, "old"), (source, "new")):
                (parent / name).mkdir()
                (parent / name / "payload").write_text(prefix + name)
    policy = fixture_policy(root, "com.lexasoft.loopz.test.broker-process." + uuid.uuid4().hex)
    policy.definition.write_bytes(plistlib.dumps(policy.plist()))
    inspector = BrokerInspector(policy, static_code=verify_fixture_code)
    admissions = []
    operations = []

    def tracked(*args, **kwargs):
        if args[:2] in (("/bin/launchctl", "bootout"), ("/bin/launchctl", "bootstrap")):
            operations.append([str(a) for a in args])
        return command(*args, **kwargs)

    inspector.run = tracked

    def admit():
        if document.read_bytes() != marker or os.geteuid() != policy.owner:
            raise RuntimeError("Private rehearsal admission changed")
        admissions.append(time.monotonic())

    lifecycle = BrokerLifecycle(inspector, admit)
    cleaned = False
    try:
        original_node = inspector.inventory(policy.program)
        original = lifecycle.ensure_started(original_node)["audit"]
        repeated = BrokerLifecycle(inspector, admit).ensure_started(original_node)
        assert repeated["reused"] and repeated["audit"] == original
        try:
            require_process_code(
                original["process"]["pid"], 'identifier "com.lexasoft.loopz.foreign-publisher"'
            )
        except RuntimeError:
            pass
        else:
            raise AssertionError("A foreign running-code identity was accepted")
        originals = {name: files(path) for name, path in targets.items()}
        original_inodes = {name: path.stat().st_ino for name, path in targets.items()}
        with SuiteFiles(support, targets) as transaction:
            transaction.prepare(source)
            transaction.apply()
        with SuiteFiles(support, targets) as transaction:
            transaction.load()
            old_image = destination / transaction.sibling(BROKER, "old")
            new_node = transaction.value["candidates"][BROKER]
            resident_old = inspector.inspect(old_image, original_node)
            assert same_generation(resident_old["process"], original["process"])
            try:
                lifecycle.ensure_started(new_node)
            except RuntimeError:
                pass
            else:
                raise AssertionError(
                    "The old resident image was accepted as the installed replacement"
                )
            assert inspector.job()["pid"] == original["process"]["pid"]
            if crash_after_bootstrap:
                worker = subprocess.run(
                    [sys.executable, "-E", "-s", "-S", __file__, "--abrupt-worker", str(root)],
                    capture_output=True,
                    text=True,
                    timeout=30,
                )
                assert worker.returncode == 77, worker.stdout + worker.stderr
                worker_operations = [
                    json.loads(line)
                    for line in (root / "worker-operations.jsonl").read_text().splitlines()
                ]
                assert [op[1] for op in worker_operations] == ["bootout", "bootstrap"]
                operations.extend(worker_operations)
                resumed = BrokerLifecycle(inspector, admit).ensure_started(new_node)
                assert resumed["reused"]
                replacement = resumed["audit"]
            else:
                transition = lifecycle.restart(resident_old, old_image, new_node)
                replacement = transition["start"]["audit"]
            assert not same_generation(original["process"], replacement["process"])
            assert replacement["image"] == new_node
            assert inspector.inspect(expected=new_node) == replacement
            lifecycle.stop(replacement)
            # Reconstruct the file owner just as a fresh recovery process would.
        with SuiteFiles(support, targets) as transaction:
            transaction.load()
            transaction.rollback()
            restored = BrokerLifecycle(inspector, admit).ensure_started(original_node)["audit"]
            assert restored["image"] == original["image"]
            assert not same_generation(restored["process"], replacement["process"])
            assert {name: files(path) for name, path in targets.items()} == originals
            assert {name: path.stat().st_ino for name, path in targets.items()} == original_inodes
            transaction.finish()
        lifecycle.stop(restored)
        assert inspector.job() is None
        assert not list(support.iterdir())
        assert document.read_bytes() == marker
        assert BrokerInspector().job() == root_job
        assert hashlib.sha256(root_definition.read_bytes()).hexdigest() == root_definition_hash
        assert hashlib.sha256(BrokerPolicy().program.read_bytes()).hexdigest() == root_broker_hash
        cleaned = True
        return {
            "status": "passed",
            "mode": "private-broker-runtime-file-recovery",
            "dynamicRunningSignatureChecked": True,
            "fixtureSignature": "ad-hoc hardened runtime, private identifier; not a production publisher test",
            "foreignRunningIdentityRejected": True,
            "residentOldImageAfterRenameVerified": True,
            "canonicalReplacementDoesNotImplyResidentReplacement": True,
            "updaterForcedExitAfterBootstrap": crash_after_bootstrap,
            "bootstrapWasNotRepeated": True,
            "original": original,
            "replacement": replacement,
            "restored": restored,
            "fourComponentOriginalInodeRollback": True,
            "operations": operations,
            "admissions": len(admissions),
            "productionBrokerJobUnchanged": root_job,
            "productionBrokerAndDefinitionBytesUnchanged": True,
            "rootBrokerProcessAuditPerformed": False,
            "driverRuntimeVerified": False,
            "audioDevicesOpened": False,
            "permissionsRequested": False,
        }
    finally:
        # Even a parser failure must not leave the fixture's infinite-loop job
        # behind. This unique label and protected definition were created above.
        result = command("/bin/launchctl", "print", policy.target, check=False)
        if result.returncode == 0:
            if (
                f"\tprogram = {policy.program}\n" not in result.stdout
                or f"\tpath = {policy.definition}\n" not in result.stdout
            ):
                raise RuntimeError("Unverified job retained for inspection: " + str(root))
            command("/bin/launchctl", "bootout", policy.target)
            end = time.monotonic() + 10
            while time.monotonic() < end:
                result = command("/bin/launchctl", "print", policy.target, check=False)
                if (
                    result.returncode in (3, 113)
                    and "Could not find service" in result.stdout + result.stderr
                ):
                    break
                time.sleep(0.1)
            else:
                raise RuntimeError(
                    "Private broker job still loaded; workspace retained: " + str(root)
                )
        elif (
            result.returncode not in (3, 113)
            or "Could not find service" not in result.stdout + result.stderr
        ):
            raise RuntimeError("Private broker cleanup could not be verified: " + str(root))
        if cleaned:
            shutil.rmtree(root)
        else:
            print("Private failure workspace retained: " + str(root), file=sys.stderr)


if __name__ == "__main__":
    if len(sys.argv) == 3 and sys.argv[1] == "--abrupt-worker":
        abrupt_worker(Path(sys.argv[2]))
    elif len(sys.argv) in (2, 3) and (
        len(sys.argv) == 2 or sys.argv[2] == "--crash-after-bootstrap"
    ):
        print(json.dumps(run(Path(sys.argv[1]).resolve(), len(sys.argv) == 3), indent=2))
    else:
        raise SystemExit(
            "usage: broker_process_tests.py <native-session-fixture> [--crash-after-bootstrap]"
        )
