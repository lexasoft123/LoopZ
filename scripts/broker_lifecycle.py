"""Generation-checked lifecycle for one audited broker job; never a PID signal.

An update/recovery owner must persist intent before calling these operations and
supply admission that verifies sessions, graph, defaults and continuity. This
module does not certify graph activation, finalize a file journal or silently
retry a possibly completed launchctl mutation after an observation failure.
"""

import subprocess
import time

from process_code import same_generation


class BrokerLifecycle:
    def __init__(self, inspector, admit, *, clock=time.monotonic, sleep=time.sleep, timeout=10):
        if not callable(admit) or not 0 < timeout <= 30:
            raise ValueError("Broker lifecycle requires admission and a bounded wait")
        self.inspector, self.admit = inspector, admit
        self.clock, self.sleep, self.timeout = clock, sleep, timeout

    def ended(self, process):
        try:
            current = self.inspector.generation(process["pid"])
        except ProcessLookupError:
            return True
        return not same_generation(current, process)

    def dispatch(self, *arguments):
        # A timeout/nonzero exit is not evidence that launchd made no change.
        # Observe the same fixed job; never repeat a mutation to obtain output.
        try:
            result = self.inspector.run("/bin/launchctl", *arguments, check=False, timeout=10)
            if result.returncode:
                return f"launchctl returned {result.returncode}"
        except (OSError, subprocess.SubprocessError) as error:
            return str(error)
        return None

    @staticmethod
    def require_generation(audited, expected):
        if (
            not same_generation(audited["process"], expected["process"])
            or audited["job"] != expected["job"]
            or audited["definition"] != expected["definition"]
            or audited["image"] != expected["image"]
        ):
            raise RuntimeError("The broker generation changed before its requested transition")

    def stop(self, expected, image=None):
        self.admit()
        current = self.inspector.job()
        dispatch_error = None
        already_absent = current is None
        if not already_absent:
            audited = self.inspector.inspect(image, expected["image"])
            self.require_generation(audited, expected)
            # Keep the final generation/job check adjacent to the fixed-label
            # command. launchctl has no atomic compare-and-bootout primitive;
            # another root process must honor the shared installation lock.
            if (
                self.inspector.generation(expected["process"]["pid"], self.inspector.policy.owner)
                != audited["process"]
                or self.inspector.job() != audited["job"]
            ):
                raise RuntimeError("Broker process changed immediately before bootout")
            dispatch_error = self.dispatch("bootout", self.inspector.policy.target)
        deadline = self.clock() + self.timeout
        while True:
            job = self.inspector.job()
            if job is not None and any(
                job.get(key) != expected["job"].get(key)
                for key in ("target", "pid", "execs", "asid")
            ):
                raise RuntimeError(
                    "An unverified broker generation appeared during stop; no retry sent"
                )
            if job is None and self.ended(expected["process"]):
                self.admit()
                return {
                    "stopped": True,
                    "alreadyAbsent": already_absent,
                    "dispatchError": dispatch_error,
                }
            if self.clock() >= deadline:
                raise RuntimeError(
                    "The audited broker did not unload; recovery state must be retained"
                )
            self.sleep(0.1)

    def wait_started(self, expected, *, reused, dispatch_error=None):
        deadline = self.clock() + self.timeout
        while True:
            job = self.inspector.job()
            if job is not None and job["ready"]:
                audited = self.inspector.inspect(expected=expected)
                self.admit()
                return {"audit": audited, "reused": reused, "dispatchError": dispatch_error}
            if self.clock() >= deadline:
                raise RuntimeError(
                    "The expected broker did not start; recovery state must be retained"
                )
            self.sleep(0.1)

    def ensure_started(self, expected):
        self.admit()
        definition = self.inspector.definition()
        self.inspector.image(self.inspector.policy.program, expected)
        if self.inspector.job() is not None:
            # Resume after an ambiguous/bootstrap-completed result. A running
            # old/foreign image fails inspection; it is never stopped here.
            return self.wait_started(expected, reused=True)
        self.admit()
        if self.inspector.definition() != definition:
            raise RuntimeError("Broker launch policy changed before bootstrap")
        self.inspector.image(self.inspector.policy.program, expected)
        if self.inspector.job() is not None:
            return self.wait_started(expected, reused=True)
        dispatch_error = self.dispatch(
            "bootstrap", self.inspector.policy.domain, self.inspector.policy.definition
        )
        return self.wait_started(expected, reused=False, dispatch_error=dispatch_error)

    def restart(self, expected, image, replacement):
        stopped = self.stop(expected, image)
        started = self.ensure_started(replacement)
        if same_generation(started["audit"]["process"], expected["process"]):
            raise RuntimeError("The broker retaining the old image did not exit")
        return {"stop": stopped, "start": started}
