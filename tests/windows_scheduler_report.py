"""Summarize xperf scheduler CSV for one pinned LoopZ process/thread.

Scheduled residency includes interrupts/DPCs; it is not thread CPU time.
Only complete switch-out/in pairs contribute gaps. No trace means no inference.
"""

import collections
import csv
import json
import re
import sys


def analyze(path, process_id, thread_id):
    headers = {}
    active = False
    last_out = ready_at = last_in = None
    intervals, runs = [], []
    counts = collections.Counter()
    priorities = collections.Counter()
    states = collections.Counter()

    def matches(process, thread):
        return int(thread, 0) == thread_id and re.fullmatch(
            rf"LoopZ[.]exe\s*\(\s*{process_id}\s*\)", process, re.IGNORECASE
        )

    with open(path, encoding="utf-8-sig", newline="") as stream:
        for raw in csv.reader(stream):
            if not raw:
                continue
            row = [value.strip() for value in raw]
            kind = row[0]
            if kind == "BeginHeader":
                continue
            if kind == "EndHeader":
                active = True
                for event, required in {
                    "CSwitch": [
                        "TimeStamp",
                        "New Process Name ( PID)",
                        "New TID",
                        "NPri",
                        "Old Process Name ( PID)",
                        "Old TID",
                        "OPri",
                        "OldState",
                        "Wait Reason",
                        "CPU",
                    ],
                    "ReadyThread": ["TimeStamp", "Rdy Process Name ( PID)", "Rdy TID"],
                }.items():
                    if any(field not in headers.get(event, []) for field in required):
                        raise ValueError(f"Missing scheduler schema: {event}")
                continue
            if not active:
                headers[kind] = row
                continue
            if kind not in ("CSwitch", "ReadyThread"):
                continue
            if len(row) != len(headers[kind]):
                raise ValueError(f"Malformed {kind} event")
            event = dict(zip(headers[kind], row))
            now = float(event["TimeStamp"])
            if kind == "ReadyThread":
                if matches(event["Rdy Process Name ( PID)"], event["Rdy TID"]):
                    counts["readyEvents"] += 1
                    if last_out and now >= last_out["atUs"] and ready_at is None:
                        ready_at = now
                continue
            if matches(event["New Process Name ( PID)"], event["New TID"]):
                counts["switchIns"] += 1
                priorities[event["NPri"]] += 1
                if last_out:
                    ready = (
                        last_out["atUs"]
                        if last_out["state"] in ("Ready", "Standby", "DeferredReady")
                        else ready_at
                    )
                    if now < last_out["atUs"] or (ready is not None and ready > now):
                        raise ValueError("Nonmonotonic target scheduling events")
                    intervals.append(
                        dict(
                            last_out,
                            inAtUs=now,
                            gapUs=now - last_out["atUs"],
                            readyAtUs=ready,
                            readyDelayUs=None if ready is None else now - ready,
                            waitForReadyUs=None if ready is None else ready - last_out["atUs"],
                            inPriority=event["NPri"],
                            inCpu=event["CPU"],
                        )
                    )
                last_in = {"atUs": now, "priority": event["NPri"], "cpu": event["CPU"]}
                last_out = ready_at = None
            if matches(event["Old Process Name ( PID)"], event["Old TID"]):
                counts["switchOuts"] += 1
                if last_in:
                    if now < last_in["atUs"]:
                        raise ValueError("Nonmonotonic target residency")
                    runs.append(
                        dict(last_in, outAtUs=now, scheduledResidencyUs=now - last_in["atUs"])
                    )
                last_in = None
                last_out = {
                    "atUs": now,
                    "priority": event["OPri"],
                    "state": event["OldState"],
                    "waitReason": event["Wait Reason"],
                    "cpu": event["CPU"],
                }
                ready_at = None
                states[event["OldState"] + "/" + event["Wait Reason"]] += 1
    if not active or not intervals:
        raise ValueError("No complete events for the pinned process/thread")
    return {
        "processId": process_id,
        "threadId": thread_id,
        "counts": dict(counts),
        "switchInPriorities": dict(priorities),
        "switchOutStates": dict(states),
        "completeIntervals": len(intervals),
        "gapsOver10ms": sum(x["gapUs"] > 10000 for x in intervals),
        "largestGaps": sorted(intervals, key=lambda x: x["gapUs"], reverse=True)[:30],
        "largestReadyDelays": sorted(
            (x for x in intervals if x["readyDelayUs"] is not None),
            key=lambda x: x["readyDelayUs"],
            reverse=True,
        )[:30],
        "largestScheduledResidencies": sorted(
            runs, key=lambda x: x["scheduledResidencyUs"], reverse=True
        )[:20],
        "scope": "Complete events during captured thread lifetime; scheduled residency includes interrupts and DPCs; trace is diagnostic only",
    }


if __name__ == "__main__":
    source, pid, tid, output = sys.argv[1:]
    result = analyze(source, int(pid), int(tid))
    with open(output, "w", encoding="utf-8") as stream:
        json.dump(result, stream, indent=2)
        stream.write("\n")
    print(
        json.dumps(
            {
                key: result[key]
                for key in ("processId", "threadId", "counts", "switchInPriorities", "gapsOver10ms")
            }
        )
    )
