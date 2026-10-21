"""Fail-closed audit of the fixed LoopZ launchd broker and its resident image."""

from dataclasses import dataclass
import hashlib
from pathlib import Path
import plistlib
import re

from install_routing import command, verify_code
from native_maps import mapped_identity, mapping_records
from prepare_routing_install import SERVICE, stable_requirement
from process_code import process_generation, require_process_code
from routing_install import SUPPORT, TEAM
from suite_update import BROKER, BROKER_IDENTIFIER, BROKER_REQUIREMENT
from uninstall_files import RemovalFiles
from uninstall_sessions import block, field, lines_for, number


@dataclass(frozen=True)
class BrokerPolicy:
    """Production uses these fixed defaults; fixtures use private GUI job paths."""

    program: Path = SUPPORT / BROKER
    definition: Path = Path("/Library/LaunchDaemons/com.lexasoft.loopz.routing.plist")
    domain: str = "system"
    label: str = "com.lexasoft.loopz.routing"
    owner: int = 0
    identifier: str = BROKER_IDENTIFIER
    requirement: str = BROKER_REQUIREMENT
    arguments: tuple = (
        "--engine-requirement",
        stable_requirement("com.lexasoft.loopz.engine", TEAM),
    )
    mach_service: str = SERVICE

    def __post_init__(self):
        if (
            type(self.owner) is not int
            or not 0 <= self.owner <= 0xFFFFFFFF
            or self.domain != ("system" if self.owner == 0 else f"gui/{self.owner}")
            or not re.fullmatch(r"[A-Za-z0-9.-]{1,200}", self.label)
            or any(not p.is_absolute() or ".." in p.parts for p in (self.program, self.definition))
            or not isinstance(self.arguments, tuple)
            or any(not isinstance(a, str) or "\n" in a or "\0" in a for a in self.argv)
        ):
            raise ValueError("Invalid fixed broker job policy")

    @property
    def target(self):
        return self.domain + "/" + self.label

    @property
    def argv(self):
        return [str(self.program), *self.arguments]

    def plist(self):
        return {
            "Label": self.label,
            "ProgramArguments": self.argv,
            "MachServices": {self.mach_service: True} if self.mach_service else {},
            "KeepAlive": True,
            "RunAtLoad": True,
            "ProcessType": "Background",
            "ThrottleInterval": 3,
        }


def mapped_executable(encoded, pid, names, expected):
    records = mapping_records(encoded, pid)
    found = False
    for row in records:
        if row.get("f") not in ("txt", "mem", "DEL") or row.get("n") not in names:
            continue
        if mapped_identity(row) != (expected["device"], expected["inode"]):
            raise RuntimeError("The broker also maps another executable generation")
        found = True
    if not found:
        raise RuntimeError("The expected broker executable is not resident")


class BrokerInspector:
    def __init__(
        self,
        policy=None,
        *,
        run=command,
        generation=process_generation,
        dynamic_code=require_process_code,
        static_code=verify_code,
    ):
        self.policy = policy or BrokerPolicy()
        self.run, self.generation = run, generation
        self.dynamic_code, self.static_code = dynamic_code, static_code

    def inventory(self, path):
        with RemovalFiles({"broker": path}, owner=self.policy.owner) as inspect:
            node = inspect.inventory()["broker"]
        if node is None or "sha256" not in node:
            raise RuntimeError("A protected broker regular file is required")
        return node

    def definition(self):
        node = self.inventory(self.policy.definition)
        if node["size"] > 64 * 1024:
            raise RuntimeError("Broker job definition exceeds its size limit")
        encoded = self.policy.definition.read_bytes()
        if hashlib.sha256(encoded).hexdigest() != node["sha256"]:
            raise RuntimeError("Broker job definition changed while reading")
        value = plistlib.loads(encoded)
        # Canonical plist encoding distinguishes booleans from integer lookalikes.
        if plistlib.dumps(value, sort_keys=True) != plistlib.dumps(
            self.policy.plist(), sort_keys=True
        ):
            raise RuntimeError("The broker job differs from fixed publisher/launch policy")
        if self.inventory(self.policy.definition) != node:
            raise RuntimeError("Broker job definition changed during verification")
        return node

    def job(self):
        result = self.run("/bin/launchctl", "print", self.policy.target, check=False)
        if result.returncode:
            if result.returncode in (3, 113) and (
                "Could not find service" in result.stdout + result.stderr
            ):
                return None
            raise RuntimeError("The fixed broker job could not be inspected")
        if result.stderr.strip():
            raise RuntimeError("Broker job inspection emitted warnings")
        lines = lines_for(result, self.policy.target)
        domain = field(lines, "domain")
        asid = None
        if self.policy.owner:
            asid = number(field(lines, "asid"))
            if not asid or domain != f"{self.policy.domain} [{asid}]":
                raise RuntimeError("Broker GUI domain identity is inconsistent")
        elif domain != "system":
            raise RuntimeError("Broker is not registered in its fixed system domain")
        if (
            field(lines, "path") != str(self.policy.definition)
            or field(lines, "program") != str(self.policy.program)
            or field(lines, "type") != ("LaunchDaemon" if self.policy.owner == 0 else "LaunchAgent")
            or block(lines, "arguments") != ["\t\t" + a for a in self.policy.argv]
        ):
            raise RuntimeError("A foreign job occupies the broker launchd label")
        environment = block(lines, "environment")
        allowed = {"\t\tOSLogRateLimit => 64", "\t\tXPC_SERVICE_NAME => " + self.policy.label}
        if set(environment) != allowed or len(environment) != len(allowed):
            raise RuntimeError("Broker job has unexpected explicit environment overrides")
        if self.policy.mach_service:
            endpoints = block(lines, "endpoints")
            headers = [
                line
                for line in endpoints
                if line.startswith("\t\t") and not line.startswith("\t\t\t") and line != "\t\t}"
            ]
            managed = [line for line in endpoints if line.startswith("\t\t\tmanaged = ")]
            if headers != ['\t\t"' + self.policy.mach_service + '" = {'] or managed != [
                "\t\t\tmanaged = 1"
            ]:
                raise RuntimeError(
                    "The broker's loaded Mach endpoint differs from its fixed policy"
                )
        pids = [line[len("\tpid = ") :] for line in lines if line.startswith("\tpid = ")]
        if len(pids) > 1:
            raise RuntimeError("Broker job has ambiguous process identity")
        pid = number(pids[0]) if pids else None
        if pid is not None and pid <= 1:
            raise RuntimeError("Broker job has an invalid process identifier")
        state = field(lines, "state")
        values = [line[len("\texecs = ") :] for line in lines if line.startswith("\texecs = ")]
        if len(values) > 1 or not values and pid is not None and state == "running":
            raise RuntimeError("Broker job has missing or ambiguous execution identity")
        execs = number(values[0]) if values else 0
        return {
            "target": self.policy.target,
            "pid": pid,
            "execs": execs,
            "asid": asid,
            "ready": pid is not None and execs > 0 and state == "running",
        }

    def allowed_image(self, image):
        image = Path(image)
        if image != self.policy.program and (
            image.parent != self.policy.program.parent
            or not re.fullmatch(
                re.escape("." + self.policy.program.name) + r"\.loopz-(old|new)-[a-f0-9]{32}",
                image.name,
            )
        ):
            raise ValueError("Broker recovery image must occupy its fixed recorded slot")
        return image

    def image(self, path, expected=None):
        path = self.allowed_image(path)
        node = self.inventory(path)
        if not node["mode"] & 0o111 or expected is not None and node != expected:
            raise RuntimeError("Broker executable differs from its recorded image")
        self.static_code(path, self.policy.identifier, self.policy.requirement, False)
        if self.inventory(path) != node:
            raise RuntimeError("Broker executable changed during signature verification")
        return node

    def inspect(self, image=None, expected=None):
        path = self.allowed_image(image or self.policy.program)
        definition = self.definition()
        job = self.job()
        if job is None or not job["ready"]:
            raise RuntimeError("The verified broker job has no live process")
        node = self.image(path, expected)
        process = self.generation(job["pid"], self.policy.owner)
        names = {str(self.policy.program), str(path)}
        if process["executable"] not in names:
            raise RuntimeError("Broker kernel executable path differs from its recorded slot")
        self.dynamic_code(job["pid"], self.policy.requirement)
        mapped = self.run("/usr/sbin/lsof", "-nP", "-a", "-p", str(job["pid"]), "-F0pfDin")
        if mapped.stderr.strip():
            raise RuntimeError("Broker mapped-image inspection emitted warnings")
        mapped_executable(mapped.stdout.encode("utf-8"), job["pid"], names, node)
        if (
            self.generation(job["pid"], self.policy.owner) != process
            or self.job() != job
            or self.definition() != definition
            or self.inventory(path) != node
        ):
            raise RuntimeError("Broker job, process or image changed during inspection")
        return {"job": job, "process": process, "image": node, "definition": definition}
