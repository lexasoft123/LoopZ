"""Fixed-publisher coordinated update package policy; no service or file installation."""

import hashlib
import json
import os
from pathlib import Path
import re
import shutil
import subprocess

from desktop_install import bounded_files, validate_runtime_capabilities
from driver_update import UPDATE_NAMES, UPDATE_TARGETS, assemble, verify_update
from install_transaction import files
from prepare_routing_install import stable_requirement
from routing_install import SUPPORT, TEAM

BROKER = "loopz-routing-broker"
BROKER_IDENTIFIER = "com.lexasoft.loopz.routing.broker"
BROKER_REQUIREMENT = stable_requirement(BROKER_IDENTIFIER, TEAM)
NAMES = (*UPDATE_NAMES, BROKER)
TARGETS = {**UPDATE_TARGETS, BROKER: SUPPORT / BROKER}
RATES = [44100, 48000, 88200, 96000, 176400, 192000]
CAPABILITIES = {"rateNegotiation": 1, "outputPairRemoval": 1, "sampleRatesHz": RATES}


def unique_object(pairs):
    result = {}
    for key, value in pairs:
        if key in result:
            raise ValueError("Duplicate coordinated-update manifest field")
        result[key] = value
    return result


def rates(value):
    return isinstance(value, list) and all(type(rate) is int for rate in value) and value == RATES


def verify_suite(package, verify_code, expected_manifest=None):
    package = Path(package)
    actual = bounded_files(package)
    encoded = (package / "manifest.json").read_bytes()
    digest = hashlib.sha256(encoded).hexdigest()
    if len(encoded) > 1024 * 1024 or expected_manifest and digest != expected_manifest:
        raise ValueError("Coordinated update manifest changed or exceeds its size limit")
    manifest = json.loads(encoded, object_pairs_hook=unique_object)
    if not isinstance(manifest, dict) or set(manifest) != {
        "schema",
        "kind",
        "installed",
        "updateManifestSHA256",
        "capabilities",
        "files",
    }:
        raise ValueError("Unexpected coordinated update manifest fields")
    if (
        type(manifest["schema"]) is not int
        or manifest["schema"] != 1
        or manifest["kind"] != "loopz-suite-update"
        or manifest["installed"] is not False
    ):
        raise ValueError("Unsupported coordinated update manifest")
    nested = manifest["updateManifestSHA256"]
    if not isinstance(nested, str) or not re.fullmatch(r"[a-f0-9]{64}", nested):
        raise ValueError("A reviewed nested app/driver digest is required")
    caps = manifest["capabilities"]
    if (
        not isinstance(caps, dict)
        or set(caps) != set(CAPABILITIES)
        or any(
            type(caps.get(key)) is not int or caps[key] != 1
            for key in ("rateNegotiation", "outputPairRemoval")
        )
        or not rates(caps.get("sampleRatesHz"))
    ):
        raise ValueError("Unsupported coordinated update capabilities")
    actual.pop("manifest.json", None)
    if (
        not actual
        or manifest["files"] != actual
        or any(path.split("/", 1)[0] not in ("update", "broker") for path in actual)
    ):
        raise ValueError("Coordinated update differs from its complete manifest")
    verify_update(package / "update", verify_code, nested)
    if set(bounded_files(package / "broker")) != {BROKER}:
        raise ValueError("Only the fixed broker executable may accompany the update")
    binary = package / "broker" / BROKER
    if not binary.stat().st_mode & 0o111:
        raise ValueError("The staged broker is not executable")
    verify_code(binary, BROKER_IDENTIFIER, BROKER_REQUIREMENT, False)
    return manifest, digest


def runtime_capabilities(binary):
    if os.geteuid() == 0:
        raise RuntimeError("Inspect staged component capabilities as the console user, not root")
    environment = os.environ.copy()
    for name in (
        "LOOPZ_MAC_P12_PASSWORD",
        "SINGZ_MAC_P12_PASSWORD",
        "LOOPZ_ENGINE_SIGNING_IDENTITY",
        "LOOPZ_ENGINE_SIGNING_KEYCHAIN",
    ):
        environment.pop(name, None)
    result = subprocess.run(
        [str(binary), "--runtime-capabilities"],
        capture_output=True,
        text=True,
        timeout=10,
        stdin=subprocess.DEVNULL,
        env=environment,
    )
    if result.returncode or len(result.stdout) > 64 * 1024:
        raise ValueError("A staged component did not provide bounded capabilities")
    return json.loads(result.stdout, object_pairs_hook=unique_object)


def inspect_suite(package, run=runtime_capabilities):
    """Call only after signature/content verification; never start a routing session."""
    package = Path(package)
    engine = run(package / "update/desktop/LoopZ Engine.app/Contents/MacOS/LoopZ Engine")
    broker = run(package / "broker" / BROKER)
    validate_runtime_capabilities(engine)
    for value in (engine, broker):
        if (
            not isinstance(value, dict)
            or type(value.get("version")) is not int
            or value["version"] != 1
            or type(value.get("rateNegotiation")) is not int
            or value["rateNegotiation"] != 1
            or not rates(value.get("sampleRatesHz"))
        ):
            raise ValueError("Engine and broker must both implement the negotiated rate policy")
    if (
        type(engine.get("outputPairRemoval")) is not int
        or engine["outputPairRemoval"] != 1
        or type(broker.get("audioMappingProtocol")) is not int
        or broker["audioMappingProtocol"] != 2
    ):
        raise ValueError("Staged component features or audio protocol differ")
    return {"engine": engine, "broker": broker}


def assemble_suite(package, work):
    """Prepare only four fixed targets for a future guarded installation owner."""
    destination = assemble(Path(package) / "update", work)
    source = Path(package) / "broker" / BROKER
    shutil.copy2(source, destination / BROKER, follow_symlinks=False)
    if set(path.name for path in destination.iterdir()) != set(NAMES):
        raise RuntimeError("Coordinated assembly contains unexpected targets")
    if files(destination / BROKER) != files(source):
        raise RuntimeError("Assembled broker differs from the verified snapshot")
    bounded_files(destination)
    return destination
