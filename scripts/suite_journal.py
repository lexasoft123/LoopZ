"""Strict durable four-component file-update schema; no service commands."""

from pathlib import Path
import re
import stat

from application_ownership import APPLICATIONS, snapshot_owner
from protected_record import LIMIT, ProtectedRecord
from suite_update import NAMES

NAME = ".suite-update-state.json"
PHASES = {
    "preparing",
    "prepared",
    "activating",
    "restoring",
    "filesRestored",
    "restored",
    "committed",
}


def node_identity(node):
    return node["device"], node["inode"]


class SuiteJournal(ProtectedRecord):
    def __init__(self, folder, targets, *, owner=None):
        self.targets = {name: Path(path) for name, path in targets.items()}
        if set(self.targets) != set(NAMES):
            raise ValueError("A coordinated update requires exactly four fixed components")
        super().__init__(folder, NAME, owner=owner)

    def validate_node(self, node, *, depth=0, count=None, expected_owner=None):
        expected_owner = self.owner if expected_owner is None else expected_owner
        count = [0] if count is None else count
        count[0] += 1
        if depth > 64 or count[0] > 4096 or not isinstance(node, dict):
            raise ValueError("Invalid bounded update inventory")
        basic = {"device", "inode", "mode", "owner", "group", "modifiedNs"}
        if any(type(node.get(key)) is not int or node[key] < 0 for key in basic):
            raise ValueError("Invalid update file identity")
        mode = node["mode"]
        if node["owner"] != expected_owner or mode & 0o022 or not node["inode"]:
            raise ValueError("Update files must remain protected and owned")
        if stat.S_ISDIR(mode):
            if set(node) != basic | {"entries"} or not isinstance(node["entries"], dict):
                raise ValueError("Invalid update directory inventory")
            for name, child in node["entries"].items():
                if (
                    not isinstance(name, str)
                    or not name
                    or name in (".", "..")
                    or "/" in name
                    or "\0" in name
                ):
                    raise ValueError("Invalid update child name")
                self.validate_node(
                    child, depth=depth + 1, count=count, expected_owner=expected_owner
                )
        elif (
            not stat.S_ISREG(mode)
            or set(node) != basic | {"size", "sha256"}
            or type(node.get("size")) is not int
            or not 0 <= node["size"] <= 64 * 1024 * 1024
            or not isinstance(node.get("sha256"), str)
            or not re.fullmatch(r"[a-f0-9]{64}", node["sha256"])
        ):
            raise ValueError("Invalid update regular-file inventory")

    def validate(self, value):
        if (
            not isinstance(value, dict)
            or set(value)
            != {"schema", "phase", "token", "parents", "originals", "candidates", "workspace"}
            or type(value["schema"]) is not int
            or value["schema"] != 1
            or not isinstance(value["phase"], str)
            or value["phase"] not in PHASES
            or not isinstance(value["token"], str)
            or not re.fullmatch(r"[a-f0-9]{32}", value["token"])
        ):
            raise ValueError("Invalid coordinated update state")
        for key in ("parents", "originals", "candidates"):
            if not isinstance(value[key], dict) or set(value[key]) != set(NAMES):
                raise ValueError("Incomplete coordinated update inventory")
        for name in NAMES:
            pair = value["parents"][name]
            if (
                not isinstance(pair, list)
                or len(pair) != 2
                or any(type(n) is not int or n < 0 for n in pair)
                or not pair[1]
            ):
                raise ValueError("Invalid target parent identity")
            self.validate_node(
                value["originals"][name],
                expected_owner=snapshot_owner(
                    name, value["originals"][name], self.owner, APPLICATIONS
                ),
            )
            node = value["candidates"][name]
            if node is not None:
                self.validate_node(
                    node, expected_owner=snapshot_owner(name, node, self.owner, APPLICATIONS)
                )
                if node_identity(node) == node_identity(value["originals"][name]):
                    raise ValueError("Original and replacement must be distinct files")
            elif value["phase"] not in ("preparing", "restoring", "filesRestored", "restored"):
                raise ValueError("Ready update is missing a replacement identity")
        workspace = value["workspace"]
        if workspace is not None and (
            not isinstance(workspace, list)
            or len(workspace) != 2
            or any(type(n) is not int or n < 0 for n in workspace)
            or not workspace[1]
        ):
            raise ValueError("Invalid private update workspace identity")
        if workspace is None and any(node is not None for node in value["candidates"].values()):
            raise ValueError("Replacement preparation lacks a recorded workspace")
