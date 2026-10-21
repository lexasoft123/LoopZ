"""Durable driver-image history and generation-scoped signal intents.

The full update owner holds the shared installer lock. Stored process paths are
identities only and are never executed. A new recovery actor may retry an intent
only after the previous actor's exact kernel generation has ended and the target
host has been freshly audited again.
"""

import copy
import hashlib
import json
import re

from driver_runtime import DriverPolicy, IMAGE_NAMES
from protected_record import ProtectedRecord
from process_record import generation_key, validate_process
from suite_artifacts import validate_artifacts
from suite_update import NAMES
from application_ownership import APPLICATIONS
from uninstall_files import RemovalFiles

NAME = ".suite-driver-state.json"
MAX_HOSTS = 128
MAX_ATTEMPTS = 8


def fingerprint(tree):
    return {
        "device": tree["device"],
        "inode": tree["inode"],
        "digest": hashlib.sha256(
            json.dumps(tree, sort_keys=True, separators=(",", ":")).encode()
        ).hexdigest(),
    }


class DriverRecord(ProtectedRecord):
    def __init__(self, folder, policy=None, *, owner=None):
        self.policy = policy or DriverPolicy()
        super().__init__(folder, NAME, owner=owner)
        if self.owner != self.policy.file_owner:
            self.close()
            raise ValueError("Driver recovery state and component ownership must match")

    def validate(self, value):
        if (
            not isinstance(value, dict)
            or set(value)
            != (
                {"schema", "transaction", "processes"}
                if value.get("schema") == 1
                else {"schema", "transaction", "processes", "decision"}
            )
            or type(value["schema"]) is not int
            or value["schema"] not in (1, 2)
            or not isinstance(value["transaction"], str)
            or not re.fullmatch(r"[a-f0-9]{32}", value["transaction"])
        ):
            raise ValueError("Invalid driver recovery record")
        decision = value.get("decision")
        if decision is not None:
            if (
                not isinstance(decision, dict)
                or set(decision)
                not in (
                    {"state", "manifest", "targets"},
                    {"state", "manifest", "targets", "artifacts"},
                )
                or decision["state"] not in ("committed", "restored")
                or not isinstance(decision["manifest"], str)
                or not re.fullmatch(r"[a-f0-9]{64}", decision["manifest"])
                or not isinstance(decision["targets"], dict)
                or set(decision["targets"]) != set(NAMES)
            ):
                raise ValueError("Invalid terminal update decision")
            if decision.get("artifacts") is not None:
                validate_artifacts(decision["artifacts"], self.owner)
            for node in decision["targets"].values():
                if (
                    not isinstance(node, dict)
                    or set(node) != {"device", "inode", "digest"}
                    or any(
                        type(node[key]) is not int or node[key] < 0 for key in ("device", "inode")
                    )
                    or not node["inode"]
                    or not isinstance(node["digest"], str)
                    or not re.fullmatch(r"[a-f0-9]{64}", node["digest"])
                ):
                    raise ValueError("Invalid terminal component fingerprint")
        processes = value["processes"]
        if not isinstance(processes, dict) or len(processes) > MAX_HOSTS:
            raise ValueError("Driver recovery process history exceeds its bounds")
        for key, entry in processes.items():
            if not isinstance(entry, dict) or set(entry) != {"process", "seen", "attempts"}:
                raise ValueError("Invalid driver history entry")
            validate_process(
                entry["process"], owner=self.policy.host_owner, executable=self.policy.host
            )
            if key != generation_key(entry["process"]):
                raise ValueError("Driver history key differs from its process generation")
            seen, attempts = entry["seen"], entry["attempts"]
            if (
                not isinstance(seen, list)
                or not seen
                or any(not isinstance(label, str) for label in seen)
                or seen != sorted(set(seen))
                or not set(seen) <= IMAGE_NAMES
            ):
                raise ValueError("Driver history contains unknown image generations")
            if not isinstance(attempts, list) or len(attempts) > MAX_ATTEMPTS:
                raise ValueError("Driver recovery signal history exceeds its bounds")
            actors = set()
            for attempt in attempts:
                if not isinstance(attempt, dict) or set(attempt) != {
                    "actor",
                    "target",
                    "result",
                    "error",
                }:
                    raise ValueError("Invalid driver signal intent")
                validate_process(attempt["actor"], owner=self.owner)
                if (
                    not isinstance(attempt["target"], str)
                    or attempt["target"] not in IMAGE_NAMES
                    or not isinstance(attempt["result"], str)
                    or attempt["result"] not in {"intent", "requested", "error"}
                    or not isinstance(attempt["error"], str)
                    or len(attempt["error"]) > 1024
                ):
                    raise ValueError("Invalid driver signal outcome")
                actor_key = generation_key(attempt["actor"])
                if actor_key in actors:
                    raise ValueError(
                        "One recovery actor cannot signal the same host generation twice"
                    )
                actors.add(actor_key)

    def begin(self, token, hosts):
        if self.pending():
            raise RuntimeError("An unfinished driver recovery record already exists")
        self.save(
            {"schema": 2, "transaction": token, "processes": {}, "decision": None}, initial=True
        )
        self.observe(hosts)

    def bind(self, token):
        if self.value is None:
            self.load()
        if self.value["transaction"] != token:
            raise RuntimeError("Driver recovery record belongs to another file transaction")

    def observe(self, hosts):
        value = copy.deepcopy(self.value)
        for host in hosts:
            process = host["process"]
            key = generation_key(process)
            entry = value["processes"].setdefault(
                key, {"process": process, "seen": [], "attempts": []}
            )
            if entry["process"] != process:
                raise RuntimeError("Driver kernel identity changed within one recorded generation")
            entry["seen"] = sorted(set(entry["seen"]) | set(host["images"]))
        if value != self.value:
            self.save(value)

    def entry(self, host):
        return self.value["processes"][generation_key(host["process"])]

    def intent(self, host, actor, target):
        if self.value.get("decision") is not None:
            raise RuntimeError("Terminal recovery permits cleanup only")
        value = copy.deepcopy(self.value)
        key = generation_key(host["process"])
        value["processes"][key]["attempts"].append(
            {"actor": actor, "target": target, "result": "intent", "error": ""}
        )
        self.save(value)
        return key, len(value["processes"][key]["attempts"]) - 1

    def outcome(self, reference, error=None):
        value = copy.deepcopy(self.value)
        key, index = reference
        attempt = value["processes"][key]["attempts"][index]
        if attempt["result"] != "intent":
            raise RuntimeError("Driver signal outcome is already recorded")
        attempt["result"] = "requested" if error is None else "error"
        attempt["error"] = "" if error is None else str(error)[:1024]
        self.save(value)

    def complete(self, token):
        """Only the full owner may call after runtime and file disposition verify."""
        self.bind(token)
        self.remove_record()

    def decide(self, state, trees, manifest, artifacts=None):
        if set(trees) != set(NAMES) or any(tree is None for tree in trees.values()):
            raise ValueError("A terminal decision requires all four component inventories")
        decision = {
            "state": state,
            "manifest": manifest,
            "targets": {name: fingerprint(tree) for name, tree in trees.items()},
        }
        if artifacts is not None:
            decision["artifacts"] = artifacts
        if self.value.get("decision") is not None and self.value["decision"] != decision:
            raise RuntimeError("A terminal update decision cannot change")
        value = copy.deepcopy(self.value)
        value.update(schema=2, decision=decision)
        self.save(value)

    def finish_decision(self, targets):
        """Read-only component verification, then remove only this orphan record."""
        decision = self.value.get("decision")
        if decision is None or set(targets) != set(NAMES):
            raise RuntimeError("An unfinished update needs full runtime recovery")
        with RemovalFiles(
            targets, owner=self.owner, user_components=APPLICATIONS & set(targets)
        ) as inspect:
            actual = inspect.inventory()
        if (
            any(tree is None for tree in actual.values())
            or {name: fingerprint(tree) for name, tree in actual.items()} != decision["targets"]
        ):
            raise RuntimeError("The terminal installation changed; recovery record retained")
        self.remove_record()
