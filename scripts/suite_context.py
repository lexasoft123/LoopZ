"""Data-only runtime context inside the file journal's recorded private workspace."""

import copy
from pathlib import Path
import re

from broker_host import BrokerPolicy
from process_record import validate_process
from suite_artifacts import validate_artifacts
from protected_record import ProtectedRecord
from suite_update import BROKER
from uninstall_sessions import require_sessions_off

STEPS = {
    "prepared",
    "promoting",
    "stoppingBroker",
    "startingBroker",
    "loadingDriver",
    "validating",
    "restoring",
    "verifyingOriginal",
    "complete",
}


def validate_before(before):
    if not isinstance(before, dict) or set(before) != {
        "consoleUID",
        "coreAudio",
        "defaults",
        "sessions",
        "preserved",
    }:
        raise ValueError("Incomplete original runtime context")
    if (
        type(before["consoleUID"]) is not int
        or before["consoleUID"] <= 0
        or not isinstance(before["coreAudio"], str)
        or not 1 <= len(before["coreAudio"]) <= 1024
    ):
        raise ValueError("Invalid original console/Core Audio identity")
    defaults = before["defaults"]
    if (
        not isinstance(defaults, dict)
        or set(defaults) != {"defaultInput", "defaultOutput", "defaultSystemOutput"}
        or any(
            not isinstance(value, str) or not value or len(value) > 4096
            for value in defaults.values()
        )
    ):
        raise ValueError("Invalid original audio defaults")
    require_sessions_off(before["sessions"])
    if (
        before["consoleUID"] not in {account["uid"] for account in before["sessions"]["accounts"]}
        or not isinstance(before["preserved"], dict)
        or not before["preserved"]
    ):
        raise ValueError("Original protected-state or console-session coverage is missing")


class SuiteContext(ProtectedRecord):
    def __init__(self, transaction, policy=None):
        self.transaction = transaction
        self.policy = policy or BrokerPolicy()
        folder = transaction.folder / transaction.workspace_name()
        super().__init__(folder, ".runtime-context.json", owner=transaction.owner)
        if list(self.folder_identity) != transaction.value["workspace"]:
            self.close()
            raise RuntimeError("Runtime context workspace differs from the file journal")

    def validate_audit(self, audit):
        if not isinstance(audit, dict) or set(audit) != {"job", "process", "image", "definition"}:
            raise ValueError("Incomplete broker recovery audit")
        validate_process(audit["process"], owner=self.policy.owner)
        allowed = {
            str(self.policy.program),
            *[
                str(self.policy.program.parent / self.transaction.sibling(BROKER, kind))
                for kind in ("old", "new")
            ],
        }
        if audit["process"]["executable"] not in allowed:
            raise ValueError("Stored broker process is outside its fixed image slots")
        job = audit["job"]
        if (
            not isinstance(job, dict)
            or set(job) != {"target", "pid", "execs", "asid", "ready"}
            or job["target"] != self.policy.target
            or type(job["pid"]) is not int
            or job["pid"] != audit["process"]["pid"]
            or type(job["execs"]) is not int
            or job["execs"] <= 0
            or job["ready"] is not True
            or (
                job["asid"] is not None
                if self.policy.owner == 0
                else type(job["asid"]) is not int or job["asid"] <= 0
            )
        ):
            raise ValueError("Stored broker job differs from its fixed policy")
        expected = [self.transaction.value[key][BROKER] for key in ("originals", "candidates")]
        if audit["image"] not in expected:
            raise ValueError("Stored broker image differs from the file transaction")
        self.transaction.validate_node(audit["definition"])
        if "sha256" not in audit["definition"]:
            raise ValueError("Stored broker definition is not a regular file")

    def validate_artifacts(self, artifacts):
        validate_artifacts(artifacts, self.owner)

    def validate(self, value):
        if (
            not isinstance(value, dict)
            or set(value)
            != {"schema", "transaction", "manifest", "before", "brokers", "step", "artifacts"}
            or type(value["schema"]) is not int
            or value["schema"] != 1
            or value["transaction"] != self.transaction.value["token"]
            or not isinstance(value["manifest"], str)
            or not re.fullmatch(r"[a-f0-9]{64}", value["manifest"])
            or not isinstance(value["step"], str)
            or value["step"] not in STEPS
        ):
            raise ValueError("Invalid coordinated runtime context")
        validate_before(value["before"])
        self.validate_artifacts(value["artifacts"])
        brokers = value["brokers"]
        if not isinstance(brokers, list) or not 1 <= len(brokers) <= 32:
            raise ValueError("Invalid bounded broker process history")
        for audit in brokers:
            self.validate_audit(audit)

    def begin(self, manifest, before, broker):
        if self.pending():
            raise RuntimeError("Runtime context is already recorded")
        self.save(
            {
                "schema": 1,
                "transaction": self.transaction.value["token"],
                "manifest": manifest,
                "before": before,
                "brokers": [broker],
                "step": "prepared",
                "artifacts": {"work": None, "guards": [], "probe": None},
            },
            initial=True,
        )

    def step(self, step):
        value = copy.deepcopy(self.value)
        value["step"] = step
        self.save(value)

    def observe_broker(self, audit):
        if audit not in self.value["brokers"]:
            value = copy.deepcopy(self.value)
            value["brokers"].append(audit)
            self.save(value)

    def artifacts(self, artifacts):
        value = copy.deepcopy(self.value)
        value["artifacts"] = artifacts
        self.save(value)
