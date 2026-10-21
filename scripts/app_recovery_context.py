"""Data-only app update context; executable paths always come from product policy."""

import hashlib
import json
import re

from app_update_journal import inventory
from desktop_install import TARGETS
from editor_update import ENGINE, LOGIN_FLAGS, login_state


def digest(encoded):
    return hashlib.sha256(encoded).hexdigest()


def engine_fingerprint(path, owner):
    node = inventory(path, owner)
    return {
        "device": node["device"],
        "inode": node["inode"],
        "contents": digest(json.dumps(node, sort_keys=True).encode()),
    }


def validate_context(value, names):
    if not isinstance(value, dict) or set(value) != {
        "version", "mode", "manifestSHA256", "interfaceWasOpen", "sessionDefinitionSHA256",
        "login", "unchangedEngine",
    }:
        raise ValueError("Unsupported app update recovery context")
    if (
        type(value["version"]) is not int
        or value["version"] != 1
        or type(value["interfaceWasOpen"]) is not bool
        or not isinstance(value["manifestSHA256"], str)
        or not re.fullmatch(r"[a-f0-9]{64}", value["manifestSHA256"])
        or not isinstance(value["login"], dict)
        or set(value["login"]) != set(LOGIN_FLAGS)
        or any(type(flag) is not bool for flag in value["login"].values())
        or value["login"]["available"] is not True
    ):
        raise ValueError("Invalid app recovery identity, login choice or version")
    login_state({"version": 1, "login": value["login"]})
    if value["mode"] == "routing" and list(names) == ["LoopZ.app", ENGINE]:
        if (
            not isinstance(value["sessionDefinitionSHA256"], str)
            or not re.fullmatch(r"[a-f0-9]{64}", value["sessionDefinitionSHA256"])
            or value["unchangedEngine"] is not None
        ):
            raise ValueError("Invalid paired app recovery context")
    elif value["mode"] == "interface" and list(names) == ["LoopZ.app"]:
        fingerprint = value["unchangedEngine"]
        if (
            value["sessionDefinitionSHA256"] is not None
            or not isinstance(fingerprint, dict)
            or set(fingerprint) != {"device", "inode", "contents"}
            or any(
                type(fingerprint[key]) is not int or fingerprint[key] <= 0
                for key in ("device", "inode")
            )
            or not isinstance(fingerprint["contents"], str)
            or not re.fullmatch(r"[a-f0-9]{64}", fingerprint["contents"])
        ):
            raise ValueError("Invalid interface-only recovery context")
    else:
        raise ValueError("Recovery mode does not match the fixed application roles")
    return value


def update_context(host, manifest_sha, *, interface_was_open=False, interface_only=False):
    value = {
        "version": 1,
        "mode": "interface" if interface_only else "routing",
        "manifestSHA256": manifest_sha,
        "interfaceWasOpen": bool(interface_was_open),
        "sessionDefinitionSHA256": None if interface_only else digest(host.session.definition),
        "login": host.expected_login,
        "unchangedEngine": (
            engine_fingerprint(TARGETS[ENGINE], host.uid) if interface_only else None
        ),
    }
    validate_context(value, ["LoopZ.app"] if interface_only else ["LoopZ.app", ENGINE])
    return value
