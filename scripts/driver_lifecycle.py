"""Restore a known driver image using only audited isolated-host generations."""

import os
import time

import driver_host
from driver_record import generation_key, validate_process
from process_code import process_generation, same_generation


def actor_ended(actor):
    try:
        current = process_generation(actor["pid"])
    except ProcessLookupError:
        return True
    return not same_generation(current, actor)


class DriverLifecycle:
    def __init__(
        self,
        inspector,
        record,
        admit,
        poke,
        *,
        actor=None,
        signaller=driver_host.signal_checked,
        ended=actor_ended,
        clock=time.monotonic,
        sleep=time.sleep,
        timeout=25,
        checkpoint=lambda event: None,
    ):
        if not callable(admit) or not callable(poke) or not 0 < timeout <= 30:
            raise ValueError(
                "Driver lifecycle requires admission, catalog refresh and a bounded wait"
            )
        self.inspector, self.record, self.admit, self.poke = inspector, record, admit, poke
        self.actor = actor or process_generation(os.getpid(), record.owner)
        validate_process(self.actor, owner=record.owner)
        self.signaller, self.ended = signaller, ended
        self.clock, self.sleep, self.timeout, self.checkpoint = clock, sleep, timeout, checkpoint

    def may_signal(self, host):
        attempts = self.record.entry(host)["attempts"]
        for attempt in attempts:
            if same_generation(attempt["actor"], self.actor):
                return False
            if not self.ended(attempt["actor"]):
                raise RuntimeError(
                    "A previous recovery actor is still alive; no signal retry is safe"
                )
        return True

    def clean_target(self, host, target):
        entry = self.record.entry(host)
        return host["images"] == [target] and entry["seen"] == [target] and not entry["attempts"]

    def ensure_image(self, target, images):
        if self.record.value.get("decision") is not None:
            raise RuntimeError("Terminal recovery permits file cleanup, not runtime transitions")
        if target not in images:
            raise RuntimeError("The desired driver image has no verified recovery file")
        deadline = self.clock() + self.timeout
        while True:
            self.admit()
            try:
                hosts = self.inspector.observe(images)
            except ProcessLookupError:
                hosts = None  # The observed generation ended; do not guess another PID.
            if hosts is not None:
                self.record.observe(hosts)
                if len(hosts) == 1 and self.clean_target(hosts[0], target):
                    self.admit()
                    try:
                        verified = self.inspector.observe(images)
                    except ProcessLookupError:
                        verified = None
                    if verified is not None:
                        self.record.observe(verified)
                    if verified == hosts:
                        return {"target": target, "host": hosts[0], "actor": self.actor}
                else:
                    obsolete = [host for host in hosts if not self.clean_target(host, target)]
                    for host in obsolete:
                        if not self.may_signal(host):
                            continue
                        self.admit()
                        try:
                            current = self.inspector.inspect(host["process"]["pid"], images)
                        except ProcessLookupError:
                            continue
                        if current != host:
                            self.record.observe([current])
                            continue
                        reference = self.record.intent(host, self.actor, target)
                        self.checkpoint("signal:intent")
                        error = None
                        try:
                            # The frozen helper performs one final kernel
                            # generation check immediately before its single-PID
                            # SIGKILL. No Core Audio/global service is signalled.
                            self.signaller({**host["process"]}, force=True)
                        except (OSError, RuntimeError) as failure:
                            error = failure
                        self.checkpoint("signal:dispatched")
                        self.record.outcome(reference, error)
                        self.checkpoint("signal:recorded")
            if self.clock() >= deadline:
                raise RuntimeError(
                    "The expected isolated driver runtime was not verified; retain recovery state"
                )
            self.poke()
            self.sleep(0.1)
