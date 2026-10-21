"""Audit only isolated LoopZ driver hosts against known original/replacement images."""

from dataclasses import dataclass, field
from pathlib import Path
import pwd
import re

import driver_host
from driver_update import DRIVER_NAME, verify_driver
from install_routing import command, verify_code
from native_maps import MAX_OUTPUT, mapped_identity, mapping_records
from process_code import process_generation, require_process_code
from uninstall_files import RemovalFiles

APPLE_HOST = Path(
    "/System/Library/Frameworks/CoreAudio.framework/Versions/A/XPCServices/com.apple.audio.Core-Audio-Driver-Service.helper.xpc/Contents/MacOS/com.apple.audio.Core-Audio-Driver-Service.helper"
)
IMAGE_NAMES = {"original", "replacement"}


@dataclass(frozen=True)
class DriverPolicy:
    plugin_root: Path = Path("/Library/Audio/Plug-Ins/HAL")
    bundle_name: str = DRIVER_NAME
    executable: str = "LoopZAudio"
    file_owner: int = 0
    host: Path = APPLE_HOST
    host_owner: int = field(default_factory=lambda: pwd.getpwnam("_coreaudiod").pw_uid)
    host_requirement: str = driver_host.IDENTITY

    @property
    def bundle(self):
        return self.plugin_root / self.bundle_name


def binary_node(tree, executable="LoopZAudio"):
    try:
        node = tree["entries"]["Contents"]["entries"]["MacOS"]["entries"][executable]
    except (KeyError, TypeError) as error:
        raise ValueError("Driver inventory is missing its fixed executable") from error
    if "sha256" not in node or not node["mode"] & 0o111:
        raise ValueError("Driver binary must be a protected executable file")
    return node


class DriverInspector:
    def __init__(
        self,
        policy=None,
        *,
        run=command,
        generation=process_generation,
        dynamic_code=require_process_code,
        verify_bundle=None,
        candidates=None,
    ):
        self.policy = policy or DriverPolicy()
        self.run, self.generation, self.dynamic_code = run, generation, dynamic_code
        self.verify_bundle = verify_bundle or (lambda path: verify_driver(path, verify_code))
        self.candidates = candidates or self.discover_pids

    def allowed_bundle(self, path):
        path = Path(path)
        if path != self.policy.bundle and (
            path.parent != self.policy.plugin_root
            or not re.fullmatch(
                re.escape("." + self.policy.bundle_name) + r"\.loopz-(old|new)-[a-f0-9]{32}",
                path.name,
            )
        ):
            raise ValueError("Driver recovery image is outside its fixed recorded slot")
        return path

    def inventory(self, path):
        with RemovalFiles(
            {"driver": self.allowed_bundle(path)}, owner=self.policy.file_owner
        ) as inspect:
            tree = inspect.inventory()["driver"]
        if tree is None:
            raise RuntimeError("The recorded driver image is missing")
        binary_node(tree, self.policy.executable)
        return tree

    def images(self, transaction):
        """Resolve inodes from the file journal, never paths supplied by its JSON."""
        if transaction.targets[DRIVER_NAME] != self.policy.bundle:
            raise ValueError("Driver transaction target differs from fixed slot policy")
        result = {}
        locations = [
            self.policy.bundle,
            *[
                self.policy.plugin_root / transaction.sibling(DRIVER_NAME, kind)
                for kind in ("old", "new")
            ],
        ]
        for label, key in (("original", "originals"), ("replacement", "candidates")):
            expected = transaction.value[key][DRIVER_NAME]
            if expected is None:
                continue
            for path in locations:
                if not path.exists() and not path.is_symlink():
                    continue
                tree = self.inventory(path)
                if tree != expected:
                    continue
                if label in result:
                    raise RuntimeError("A driver image occupies more than one recorded location")
                result[label] = {"path": path, "tree": tree}
        if "original" not in result and transaction.value["phase"] != "committed":
            raise RuntimeError("Original driver image could not be resolved for recovery")
        return result

    def discover_pids(self):
        result = self.run("/bin/ps", "-axo", "pid=,args=")
        if result.stderr.strip() or len(result.stdout) > MAX_OUTPUT:
            raise RuntimeError("Driver host discovery is incomplete")
        pids = []
        for row in result.stdout.splitlines():
            fields = row.strip().split(maxsplit=1)
            if len(fields) == 2 and fields[1] == f"Core Audio Driver ({self.policy.bundle_name})":
                if not re.fullmatch(r"[0-9]{1,10}", fields[0]):
                    raise RuntimeError("Invalid candidate driver PID")
                pids.append(int(fields[0]))
        if len(pids) > 4 or len(pids) != len(set(pids)):
            raise RuntimeError("Too many or ambiguous LoopZ driver host candidates")
        return sorted(pids)

    def inspect(self, pid, images):
        if not images or not set(images) <= IMAGE_NAMES:
            raise ValueError("Driver host inspection requires known image generations")
        expected = {}
        for label, image in images.items():
            path = self.allowed_bundle(image["path"])
            if self.inventory(path) != image["tree"]:
                raise RuntimeError("Driver image differs from its recovery inventory")
            self.verify_bundle(path)
            node = binary_node(image["tree"], self.policy.executable)
            key = node["device"], node["inode"]
            if key in expected:
                raise RuntimeError("Driver image generations share one executable inode")
            expected[key] = label
        before = self.generation(pid, self.policy.host_owner)
        if (
            before["pid"] != pid
            or before["uid"] != self.policy.host_owner
            or before["executable"] != str(self.policy.host)
        ):
            raise RuntimeError("Candidate is not the fixed isolated driver executable")
        self.dynamic_code(pid, self.policy.host_requirement)
        result = self.run("/usr/sbin/lsof", "-nP", "-a", "-p", str(pid), "-F0pfDin")
        if result.stderr.strip():
            raise RuntimeError("Driver mapped-image inspection emitted warnings")
        allowed = {
            self.policy.bundle_name,
            *[Path(image["path"]).name for image in images.values()],
        }
        found, bundles = set(), set()
        for row in mapping_records(result.stdout.encode("utf-8"), pid):
            name = row.get("n", "")
            roots = (
                str(self.policy.plugin_root),
                "/Library/Audio/Plug-Ins/HAL",
                "/System/Library/Audio/Plug-Ins/HAL",
            )
            root = next((root for root in roots if name.startswith(root + "/")), None)
            if root is None:
                continue
            if root != str(self.policy.plugin_root) or name.endswith(" (deleted)"):
                raise RuntimeError("Driver host references an unowned or deleted HAL image")
            component = name[len(root) + 1 :].split("/", 1)[0]
            if component not in allowed:
                raise RuntimeError("Driver host also references another HAL component")
            bundles.add(component)
            if name.endswith("/Contents/MacOS/" + self.policy.executable) and row.get("f") in (
                "txt",
                "mem",
                "DEL",
            ):
                label = expected.get(mapped_identity(row))
                if label is None:
                    raise RuntimeError("The host maps an unknown driver executable generation")
                found.add(label)
        if not found:
            raise ProcessLookupError("No recorded driver executable is resident yet")
        if self.generation(pid, self.policy.host_owner) != before:
            raise RuntimeError("Driver host process generation changed during audit")
        for image in images.values():
            if self.inventory(image["path"]) != image["tree"]:
                raise RuntimeError("Driver image changed during host inspection")
        return {"process": before, "images": sorted(found), "HALBundles": sorted(bundles)}

    def observe(self, images):
        before = self.candidates()
        if len(before) > 4 or len(before) != len(set(before)):
            raise RuntimeError("Ambiguous driver host discovery")
        hosts = [self.inspect(pid, images) for pid in sorted(before)]
        if self.candidates() != before:
            raise ProcessLookupError("Driver hosts changed during inventory; observe again")
        return hosts
