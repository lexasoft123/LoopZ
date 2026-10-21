"""One owner for durable four-file, broker and isolated-driver transitions.

The fixed macOS host adapter supplies actual admission, validation and diagnostic
workspace recovery. Tests use private hosts. The caller holds the installation
lock; stored data never chooses commands, targets or code-signing requirements.
"""

from pathlib import Path
import time

from broker_lifecycle import BrokerLifecycle
from suite_context import SuiteContext
from suite_update import BROKER


class SuiteTransaction:
    def __init__(self, files, record, host, work, *, checkpoint=lambda event: None):
        self.files, self.record, self.host, self.work = files, record, host, Path(work)
        self.checkpoint = checkpoint
        self.context = None
        self.guard = None
        self.baseline = None

    def admit(self):
        self.host.admit(self.baseline)
        if self.guard:
            self.guard.check()

    def controllers(self):
        broker = BrokerLifecycle(self.host.broker, self.admit)
        driver = self.host.driver_controller(self.record, self.admit)
        return broker, driver

    def broker_images(self):
        result = {}
        for path in [
            self.files.targets[BROKER],
            *[
                self.files.targets[BROKER].parent / self.files.sibling(BROKER, kind)
                for kind in ("old", "new")
            ],
        ]:
            if not path.exists() and not path.is_symlink():
                continue
            node = self.host.broker.inventory(path)
            if node not in [self.files.value[key][BROKER] for key in ("originals", "candidates")]:
                raise RuntimeError("An unknown broker image occupies a transaction slot")
            result[path] = node
        return result

    def stop_broker(self):
        broker, _ = self.controllers()
        job = self.host.broker.job()
        if job is not None:
            if not job["ready"]:
                # Observe the same registered job until its executable identity
                # can be audited. Never bootout an unverified startup proxy.
                deadline = time.monotonic() + 10
                while job is not None and not job["ready"]:
                    if time.monotonic() >= deadline:
                        raise RuntimeError(
                            "Broker startup did not become auditable; retain recovery"
                        )
                    self.admit()
                    time.sleep(0.1)
                    job = self.host.broker.job()
            if job is not None:
                images = self.broker_images()
                audits = []
                for path, node in images.items():
                    # Wrong mapped generation is expected while selecting the
                    # known old/new inode. All other failures must still prevent
                    # a signal; only a complete successful audit is actionable.
                    try:
                        audits.append((path, self.host.broker.inspect(path, node)))
                    except RuntimeError:
                        continue
                if len(audits) != 1:
                    raise RuntimeError("Exactly one known broker image must be resident")
                path, audit = audits[0]
                self.context.observe_broker(audit)
                self.checkpoint("broker:recorded")
                broker.stop(audit, path)
                self.checkpoint("broker:stopped")
        # An updater can exit after bootout but before observing process exit.
        # Every recorded broker generation must end; no orphan PID is signalled.
        deadline = time.monotonic() + 10
        while not all(broker.ended(audit["process"]) for audit in self.context.value["brokers"]):
            if time.monotonic() >= deadline:
                raise RuntimeError("A recorded broker generation remains alive; retain recovery")
            self.admit()
            time.sleep(0.1)

    def run_target(self, target):
        broker, driver = self.controllers()
        key = "candidates" if target == "replacement" else "originals"
        self.context.step("startingBroker")
        started = broker.ensure_started(self.files.value[key][BROKER])
        self.context.observe_broker(started["audit"])
        self.checkpoint("broker:started")
        self.context.step("loadingDriver")
        images = self.host.driver.images(self.files)
        loaded = driver.ensure_image(target, images)
        self.checkpoint("driver:ready")
        self.context.step("validating" if target == "replacement" else "verifyingOriginal")
        validation = self.host.validate(target, self.work, self.context)
        self.checkpoint("validation:ready")
        self.host.withdraw(self.context)
        self.admit()
        # Validation may itself expose a disconnect. Do not commit based on a
        # host that was already superseded during the public endpoint checks.
        final_driver = self.host.driver.observe(self.host.driver.images(self.files))
        self.record.observe(final_driver)
        if (
            final_driver != [loaded["host"]]
            or self.host.broker.inspect(expected=self.files.value[key][BROKER]) != started["audit"]
        ):
            raise RuntimeError("Broker or driver changed during endpoint validation")
        return {"broker": started, "driver": loaded, "validation": validation}

    def certify(self, target):
        self.admit()
        continuity = self.guard.stop()
        if continuity.get("passed") is not True:
            raise RuntimeError("Hardware continuity did not pass")
        self.guard.close()
        self.guard = None
        self.host.admit(self.baseline)
        self.checkpoint("continuity:verified")
        if target == "replacement":
            self.files.commit()
            state, key = "committed", "candidates"
        else:
            self.files.require_locations(restored=True)
            self.files.phase("restored")
            state, key = "restored", "originals"
        self.checkpoint("files:decision")
        self.record.decide(
            state,
            self.files.value[key],
            self.context.value["manifest"],
            self.context.value["artifacts"],
        )
        self.checkpoint("driver:decision")
        return continuity

    def finish(self):
        decision = self.record.value.get("decision")
        if decision is None or decision["state"] != self.files.value["phase"]:
            raise RuntimeError("File and runtime terminal decisions do not agree")
        self.files.finish()
        self.checkpoint("files:finalized")
        self.host.cleanup_artifacts(decision.get("artifacts"))
        self.checkpoint("artifacts:finalized")
        self.record.finish_decision(self.files.targets)
        self.checkpoint("driver:finalized")

    def deploy(self, stage, manifest):
        result = {"status": "recoveryRequired", "mode": "coordinated-suite-update"}
        try:
            self.baseline = self.host.capture()
            self.host.admit(self.baseline)
            initial_broker = self.host.broker.inspect()
            self.files.prepare(stage)
            self.checkpoint("files:prepared")
            self.context = SuiteContext(self.files, self.host.broker.policy)
            self.context.begin(manifest, self.baseline, initial_broker)
            self.record.begin(
                self.files.value["token"],
                self.host.driver.observe(self.host.driver.images(self.files)),
            )
            self.checkpoint("runtime:prepared")
            self.guard = self.host.start_guard(self.work, self.baseline, self.context)
            self.guard.ready()
            self.admit()
            self.context.step("promoting")
            self.files.apply()
            self.checkpoint("files:promoted")
            self.context.step("stoppingBroker")
            self.stop_broker()
            result["activation"] = self.run_target("replacement")
            result["hardwareContinuity"] = self.certify("replacement")
            result["manifestSHA256"] = manifest
            self.finish()
            result["status"] = "passed"
        except Exception as error:
            result["error"] = str(error)
            if self.files.value and self.files.value["phase"] not in ("committed", "restored"):
                try:
                    if (
                        self.context is None
                        and self.files.value["phase"] in ("preparing", "prepared")
                        and not self.record.pending()
                    ):
                        self.files.rollback()
                        self.files.finish()
                        result["rollback"] = {"preparationOnly": True}
                    else:
                        result["rollback"] = self.restore()
                    result["status"] = "rolledBack"
                except Exception as failure:
                    result["recoveryError"] = str(failure)
        finally:
            self.close()
        return result

    def restore(self):
        if self.context is None:
            self.context = SuiteContext(self.files, self.host.broker.policy)
            self.context.load()
        self.record.bind(self.files.value["token"])
        self.host.quiesce(self.context)
        # Recovery preserves the current hardware/default baseline if the Mac
        # was rebooted or devices changed while the updater was absent.
        self.baseline = self.host.recovery_baseline(self.context.value["before"])
        self.host.admit(self.baseline)
        if self.guard:
            self.guard.close()
        self.guard = self.host.start_guard(self.work, self.baseline, self.context)
        self.guard.ready()
        self.admit()
        self.context.step("restoring")
        self.stop_broker()
        # Remember resident new images before moving their files back to slots.
        self.record.observe(self.host.driver.observe(self.host.driver.images(self.files)))
        self.files.rollback()
        self.checkpoint("files:restored")
        activation = self.run_target("original")
        continuity = self.certify("original")
        self.finish()
        return {"activation": activation, "hardwareContinuity": continuity}

    def recover(self):
        try:
            if not self.files.pending():
                if not self.record.pending():
                    return {"status": "nothingToRecover"}
                self.record.load()
                decision = self.record.value.get("decision")
                if decision is None:
                    raise RuntimeError("An orphan driver record has no terminal decision")
                self.host.cleanup_artifacts(decision.get("artifacts"))
                self.record.finish_decision(self.files.targets)
                return {
                    "status": "finalizationRecovered",
                    "disposition": decision["state"],
                    "manifestSHA256": decision["manifest"],
                }
            self.files.load()
            if not self.record.pending() and self.files.value["phase"] in ("preparing", "prepared"):
                try:
                    self.context = SuiteContext(self.files, self.host.broker.policy)
                    if self.context.pending():
                        self.context.load()
                        self.host.quiesce(self.context)
                except FileNotFoundError:
                    pass
                self.files.rollback()
                self.files.finish()
                return {"status": "preparationRecovered", "runtimeTouched": False}
            self.record.bind(self.files.value["token"])
            if self.files.value["phase"] in ("committed", "restored"):
                if self.record.value.get("decision") is None:
                    self.context = SuiteContext(self.files, self.host.broker.policy)
                    self.context.load()
                    key = "candidates" if self.files.value["phase"] == "committed" else "originals"
                    self.record.decide(
                        self.files.value["phase"],
                        self.files.value[key],
                        self.context.value["manifest"],
                        self.context.value["artifacts"],
                    )
                state = self.files.value["phase"]
                self.finish()
                return {"status": "finalizationRecovered", "disposition": state}
            return {"status": "originalInstallationRecovered", "rollback": self.restore()}
        finally:
            self.close()

    def close(self):
        if self.guard:
            self.guard.close()
            self.guard = None
        if self.context:
            self.context.close()
            self.context = None
