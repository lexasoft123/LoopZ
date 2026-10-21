"""Bounded physical continuity guard with durable startup and no PID signals."""

import copy
import json
import os
from pathlib import Path
import shutil
import stat
import subprocess
import time
import uuid

from install_desktop import CHECKER
from install_routing import verify_code
from process_code import process_generation, require_process_code
from process_record import validate_process
from suite_update import unique_object
from suite_workspace import CHECKER_ID, CHECKER_NAME, CHECKER_REQUIREMENT, stop_request


class SuiteGuard:
    def __init__(
        self,
        manager,
        context,
        work,
        uid,
        *,
        checker=CHECKER,
        static_code=verify_code,
        generation=process_generation,
        dynamic_code=require_process_code,
        checkpoint=lambda event: None,
    ):
        self.manager, self.context, self.uid = manager, context, uid
        self.process = None
        self.stderr = None
        self.closed = False
        if (
            not isinstance(uid, str)
            or not uid
            or len(uid) > 4096
            or "\0" in uid
            or uid.startswith("com.lexasoft.loopz.")
        ):
            raise ValueError("A bounded non-LoopZ physical continuity UID is required")
        manager.attach(context, Path(work))
        artifacts = copy.deepcopy(context.value["artifacts"])
        if len(artifacts["guards"]) >= 8:
            raise RuntimeError("Too many retained guard attempts; recover their workspace first")
        name = "guard-" + uuid.uuid4().hex
        self.folder = Path(work) / name
        artifacts["guards"].append({"folder": name, "process": None})
        context.artifacts(artifacts)
        checkpoint("guard:recorded")
        workspace = manager.open(context.value["artifacts"]["work"])
        try:
            os.mkdir(name, 0o700, dir_fd=workspace)
            os.fsync(workspace)
        finally:
            os.close(workspace)
        binary = self.folder / CHECKER_NAME
        static_code(checker, CHECKER_ID, CHECKER_REQUIREMENT, False)
        shutil.copy2(checker, binary, follow_symlinks=False)
        static_code(binary, CHECKER_ID, CHECKER_REQUIREMENT, False)
        descriptor = os.open(
            self.folder / "guard.stderr",
            os.O_WRONLY | os.O_CREAT | os.O_EXCL | os.O_NOFOLLOW,
            0o600,
        )
        self.stderr = os.fdopen(descriptor, "wb")
        try:
            self.process = subprocess.Popen(
                [
                    str(binary),
                    "--guard",
                    uid,
                    "--report",
                    str(self.folder / "guard.json"),
                    "--stop-file",
                    str(self.folder / "guard.stop"),
                    "--seconds",
                    "120",
                ],
                stdin=subprocess.DEVNULL,
                stdout=subprocess.DEVNULL,
                stderr=self.stderr,
                start_new_session=True,
            )
            checkpoint("guard:spawned")
            observed = generation(self.process.pid, manager.owner)
            validate_process(observed, owner=manager.owner, executable=binary)
            dynamic_code(self.process.pid, CHECKER_REQUIREMENT)
            if generation(self.process.pid, manager.owner) != observed:
                raise RuntimeError("The launched continuity guard changed during its audit")
            artifacts = copy.deepcopy(context.value["artifacts"])
            artifacts["guards"][-1]["process"] = observed
            context.artifacts(artifacts)
            checkpoint("guard:started")
        except BaseException:
            self.close()
            raise

    def check(self):
        if self.process is None or self.process.poll() is not None:
            raise RuntimeError("Physical continuity guard ended unexpectedly")

    def report(self):
        descriptor = os.open(self.folder / "guard.json", os.O_RDONLY | os.O_NOFOLLOW)
        try:
            info = os.fstat(descriptor)
            if (
                not stat.S_ISREG(info.st_mode)
                or info.st_uid != self.manager.owner
                or info.st_mode & 0o022
                or info.st_nlink != 1
                or info.st_size > 65536
            ):
                raise RuntimeError("Continuity report is not a protected bounded regular file")
            encoded = os.read(descriptor, 65537)
        finally:
            os.close(descriptor)
        value = json.loads(encoded, object_pairs_hook=unique_object)
        if not isinstance(value, dict) or value.get("uid") != self.uid:
            raise RuntimeError("The continuity report describes a different physical device")
        return value

    def ready(self):
        deadline = time.monotonic() + 5
        while time.monotonic() < deadline:
            self.check()
            try:
                if self.report().get("state") == "running":
                    return
            except FileNotFoundError:
                pass
            time.sleep(0.05)
        raise RuntimeError("Physical continuity guard did not become ready")

    def stop(self):
        self.check()
        stop_request(self.folder, self.manager.owner)
        self.process.wait(timeout=5)
        value = self.report()
        if (
            self.process.returncode
            or value.get("passed") is not True
            or value.get("stoppedByRequest") is not True
            or value.get("state") != "stopped"
            or type(value.get("frames")) is not int
            or value["frames"] <= 0
            or type(value.get("timestampErrors")) is not int
            or value["timestampErrors"] != 0
        ):
            raise RuntimeError("Physical continuity did not pass")
        return value

    def close(self):
        if self.closed:
            return
        try:
            if self.process is not None:
                stop_request(self.folder, self.manager.owner)
                self.process.wait(timeout=5)
        finally:
            if self.stderr is not None:
                self.stderr.close()
            self.closed = self.process is None or self.process.poll() is not None
