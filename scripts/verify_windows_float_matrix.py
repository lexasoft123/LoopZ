#!/usr/bin/env python3
"""Read-only audit of installed exclusive float32/PCM16 format acceptance reports.

A report audit does not prove process provenance, cleanup of installed devices,
or preservation of user settings. Keep the separate installed/cleanup evidence.
"""

import argparse
import json
import math
from pathlib import Path

RATES = (44100, 48000, 88200, 96000, 176400, 192000)


def require(condition, message):
    if not condition:
        raise ValueError(message)


def finite(value):
    return isinstance(value, (int, float)) and not isinstance(value, bool) and math.isfinite(value)


def verify_profile(profile, recording_ms, public_format="float32"):
    require(public_format in ("float32", "pcm16"), "Unsupported public format")
    rate, channels = profile["sampleRate"], profile["channels"]
    label = f"{rate} Hz / {channels} channels"
    require(rate in RATES and channels in range(2, 65, 2), f"{label}: unsupported format")
    require(profile.get("ok") is True, f"{label}: native probe failed")
    require(profile.get("probe") == "exclusive-" + public_format, f"{label}: wrong public format")
    require(profile.get("recordingMs") == recording_ms, f"{label}: wrong recording duration")
    require(profile.get("precisionProbe") is False, f"{label}: unexpected precision probe")
    require(profile.get("expectedGain") == 0.5, f"{label}: wrong gain fixture")
    diagnostics = profile["diagnostics"]
    require(
        diagnostics.get("bitsPerSample") == (32 if public_format == "float32" else 16),
        f"{label}: wrong public sample width",
    )
    for counter in (
        "captureDiscontinuities",
        "capturePositionGaps",
        "captureTimestampErrors",
        "captureSilentFrames",
    ):
        require(diagnostics.get(counter) == 0, f"{label}: {counter} is not zero")
    require(diagnostics.get("longestAllChannelsZeroRun", -1) in (0, 1), f"{label}: audio gap")
    # Recompute the native continuous-sinusoid bound rather than trusting the
    # report's declared threshold. Preserve its isolated-zero/quantization policy.
    expected_step = 2 * 0.026 * math.sin(math.pi * (443 + (channels - 1) * 37) / rate) + 2 / 32768
    step = diagnostics.get("maximumSampleStep")
    require(finite(step) and 0 <= step <= expected_step, f"{label}: sample-step discontinuity")
    declared = diagnostics.get("maximumExpectedSampleStep")
    require(
        finite(declared) and math.isclose(declared, expected_step, rel_tol=1e-12),
        f"{label}: altered sample-step limit",
    )
    signal = profile["signal"]
    require(len(signal) == channels, f"{label}: incomplete channel measurements")
    for index, channel in enumerate(signal):
        require(
            channel.get("frames", 0) >= rate * recording_ms / 1000 * 0.95,
            f"{label}: channel {index} recording too short",
        )
        target, other, peak = (channel.get(key) for key in ("target", "other", "peak"))
        require(
            all(finite(value) for value in (target, other, peak))
            and 0.024 < target < 0.026
            and 0 <= other < 0.0004
            and 0 <= peak < 0.026,
            f"{label}: channel {index} gain/leakage/peak failed",
        )
    transport = profile["transportTiming"]
    require(
        transport.get("version") == 3 and transport.get("bitsPerSample") == 32,
        f"{label}: private float transport missing",
    )
    require(transport.get("deadlineMisses") == 0, f"{label}: private worker deadline failure")
    return rate, channels


def verify_native_flow_state(transport, rate):
    require(
        type(transport.get("publicationFlowVersion")) is int
        and transport["publicationFlowVersion"] == 1
        and transport.get("publicationWaitEnabled") is True
        and transport.get("streamingChunksEnabled") is True
        and transport.get("publicationBatchFrames") == rate // 100,
        "Native paired flow is not active",
    )
    require(
        type(transport.get("publicationWaitTimeouts")) is int
        and transport["publicationWaitTimeouts"] == 0
        and type(transport.get("maximumPublicationWaitNs")) is int
        and 0 <= transport["maximumPublicationWaitNs"] <= 30_000_000,
        "Native wait budget failed",
    )


def verify_matrix(
    report, probe_sha256, recording_ms=1500, public_format="float32", native_flow=False
):
    require(type(native_flow) is bool, "Invalid native flow mode")
    require(public_format in ("float32", "pcm16"), "Unsupported public format")
    require(1500 <= recording_ms <= 300000, "Unsupported recording duration")
    require(report.get("mode") == "full", "This is not a full matrix")
    require(
        report.get("ok") is True and report.get("cleanup") is True, "Run not successful/cleaned"
    )
    require(not report.get("running") and not report.get("error"), "Run is active or failed")
    require(not report.get("cleanupErrors"), "Cleanup errors remain")
    expected_probe = "--format-roundtrip-exclusive" + (
        "-float" if public_format == "float32" else ""
    )
    require(report.get("probeMode") == expected_probe, "Wrong probe mode")
    require(report.get("probeHash", "").lower() == probe_sha256.lower(), "Probe hash mismatch")
    require(not report.get("savedMixesTemporarilyPaused"), "Saved mixes were paused")
    require(report.get("recordingMs") == recording_ms, "Unexpected recording duration")
    expected = {(rate, channels) for rate in RATES for channels in range(2, 65, 2)}
    profiles = report["profiles"]
    require(
        report.get("expectedProfiles") == 192 and len(profiles) == 192, "Incomplete profile count"
    )
    observed = [verify_profile(profile, recording_ms, public_format) for profile in profiles]
    if native_flow:
        for profile in profiles:
            verify_native_flow_state(profile["transportTiming"], profile["sampleRate"])
    require(len(set(observed)) == len(observed), "Duplicate rate/channel profiles")
    require(set(observed) == expected, "Missing rate/channel coverage")
    return {"ok": True, "profiles": len(observed), "recordingMs": recording_ms}


def verify_streaming_recording(
    report, probe_sha256, rate, channels, recording_ms, flow_experiment=False, native_flow=False
):
    require(type(flow_experiment) is bool, "Invalid experimental audit mode")
    require(
        type(native_flow) is bool and not (native_flow and flow_experiment), "Ambiguous flow mode"
    )
    paired_flow = flow_experiment or native_flow
    require(15000 <= recording_ms <= 300000, "Stress audit requires 15..300 seconds")
    require(report.get("mode") == "single", "Stress audit requires a single profile")
    require(
        report.get("ok") is True and report.get("cleanup") is True, "Run not successful/cleaned"
    )
    require(not report.get("running") and not report.get("error"), "Run is active or failed")
    require(
        not report.get("cleanupErrors") and not report.get("workerFaultMode"), "Fault/cleanup run"
    )
    require(report.get("automaticSetup") is True, "Installed coordinator was not used")
    require(not report.get("savedMixesTemporarilyPaused"), "Saved mixes were paused")
    require(report.get("probeMode") == "--format-stress-exclusive-float", "Wrong stress probe")
    require(report.get("probeHash", "").lower() == probe_sha256.lower(), "Probe hash mismatch")
    require(report.get("recordingMs") == recording_ms, "Unexpected stress duration")
    require(
        report.get("expectedProfiles") == 1 and len(report["profiles"]) == 1, "Wrong profile count"
    )
    profile = report["profiles"][0]
    require(verify_profile(profile, recording_ms) == (rate, channels), "Wrong stress format")
    require(profile.get("analysisMode") == "bounded-streaming", "Recorder was not bounded")
    diagnostics = profile["diagnostics"]
    for name in ("probeDroppedFrames", "probeInvalidSamples", "probePendingPackets"):
        require(
            type(diagnostics.get(name)) is int and diagnostics[name] == 0, f"{name} is not zero"
        )
    storage = diagnostics.get("probeSampleStorageBytes")
    require(
        type(storage) is int and 0 < storage <= 32 * 4096 * channels * 4, "Recorder storage bound"
    )
    accepted, analyzed, buffer = (
        diagnostics.get(name)
        for name in ("probeAcceptedFrames", "probeAnalyzedFrames", "inputBufferFrames")
    )
    require(
        type(accepted) is int and type(analyzed) is int and accepted == analyzed and analyzed > 0,
        "Recorder did not analyze every accepted frame",
    )
    require(type(buffer) is int and 0 < buffer <= rate // 10, "Unexpected client buffer")
    minimum_frames = rate * recording_ms // 1000 - 2 * buffer
    require(
        analyzed >= minimum_frames and all(c["frames"] == analyzed for c in profile["signal"]),
        "Recording coverage is incomplete",
    )
    frequency, start, end = (
        diagnostics.get(name)
        for name in ("qpcFrequency", "recordingStartQpcTicks", "streamEndQpcTicks")
    )
    require(all(type(v) is int and v > 0 for v in (frequency, start, end)), "Missing QPC evidence")
    require((end - start) * rate >= frequency * minimum_frames, "QPC recording too short")
    rates_before = report.get("renderRatesBeforeFixture", {})
    rates_during = report.get("renderRatesDuringFixture", {})
    require(
        rates_before
        and all(finite(v) and v > 0 and rates_during.get(k) == v for k, v in rates_before.items()),
        "Saved mix render rates changed or are unproven",
    )
    require(report.get("renderRateBeforeProbe") == rate, "Wrong native render domain")
    observations = []
    for observation in profile.get("duringProbe", []):
        begin, finish = observation["beginQpcTicks"], observation["endQpcTicks"]
        require(
            type(begin) is int and type(finish) is int and begin <= finish,
            "Invalid observation clock",
        )
        within = start <= begin and finish <= end
        require(observation.get("withinRecording") is within, "Incorrect recording-window claim")
        if within:
            require(observation.get("renderRate") == rate, "Render domain changed")
            transport = observation["transportTiming"]
            require(
                transport["driverQueue"].get("available") is True, "Driver counters unavailable"
            )
            require(
                transport.get("publicationWaitEnabled") is paired_flow
                and transport.get("streamingChunksEnabled") is paired_flow
                and transport.get("publicationBatchFrames") == rate // 100,
                "Worker experiment settings changed",
            )
            if native_flow:
                verify_native_flow_state(transport, rate)
            if not paired_flow:
                require(transport.get("publicationFlowVersion", 0) == 0, "Unexpected native flow")
            if paired_flow:
                require(
                    type(transport.get("publicationWaits")) is int
                    and transport["publicationWaits"] >= 0
                    and type(transport.get("publicationWaitTimeouts")) is int
                    and transport["publicationWaitTimeouts"] == 0
                    and type(transport.get("maximumPublicationWaitNs")) is int
                    and 0 <= transport["maximumPublicationWaitNs"] <= 30_000_000,
                    "Experimental publication wait failed or exceeded worker deadline",
                )
            require(
                observation["sceneTiming"].get("enabled") is False, "Stage profiling is enabled"
            )
            observations.append(observation)
    require(len(observations) >= 2, "Missing within-recording observations")
    require(
        observations[0]["beginQpcTicks"] - start <= 2 * frequency
        and end - observations[-1]["endQpcTicks"] <= 2 * frequency,
        "Unobserved recording boundaries",
    )
    for previous, following in zip(observations, observations[1:]):
        require(
            previous["endQpcTicks"] <= following["beginQpcTicks"]
            and following["beginQpcTicks"] - previous["endQpcTicks"] <= 2 * frequency,
            "Missing or unordered recording observations",
        )
    paths = (
        ("publicationRead", "missingFrames"),
        ("publicationRead", "invalidBlocks"),
        ("publicationRead", "graphFailures"),
        ("transportTiming", "restartAttempts"),
        ("transportTiming", "deadlineMisses"),
        ("transportTiming", "completedIngressZeroRuns"),
        ("transportTiming", "timestampResets"),
        ("transportTiming", "driverQueue", "underruns"),
        ("transportTiming", "driverQueue", "leaseSilentFrames"),
        ("transportTiming", "driverQueue", "droppedFrames"),
        ("renderTiming", "reanchors"),
        ("renderTiming", "skippedFrames"),
        ("transportTiming", "threadId"),
        ("renderTiming", "threadId"),
    )
    for path in paths:
        values = []
        for observation in observations:
            value = observation
            for component in path:
                value = value[component]
            values.append(value)
        require(all(type(v) is int and v >= 0 for v in values), f"Invalid counter {path}")
        require(all(v == values[0] for v in values), f"Recording counter changed: {path}")
        if path[-1] == "threadId":
            require(values[0] > 0, "Unidentified audio thread")
    result = {
        "ok": True,
        "sampleRate": rate,
        "channels": channels,
        "recordingMs": recording_ms,
        "observations": len(observations),
        "sampleStorageBytes": storage,
    }
    if paired_flow:
        waits = [o["transportTiming"]["publicationWaits"] for o in observations]
        require(waits == sorted(waits) and waits[-1] > 0, "Wait policy was reset or unexercised")
        final = profile["transportTiming"]
        require(
            final.get("publicationWaitEnabled") is True
            and final.get("streamingChunksEnabled") is True
            and final.get("publicationBatchFrames") == rate // 100
            and type(final.get("publicationWaits")) is int
            and final["publicationWaits"] >= waits[-1]
            and type(final.get("publicationWaitTimeouts")) is int
            and final["publicationWaitTimeouts"] == 0
            and type(final.get("maximumPublicationWaitNs")) is int
            and 0 <= final["maximumPublicationWaitNs"] <= 30_000_000,
            "Final experimental wait state failed",
        )
        if native_flow:
            verify_native_flow_state(final, rate)
            result.update(nativeFlowVersion=1)
        else:
            result.update(diagnosticOnly=True, flowExperiment=True)
    return result


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("report", type=Path)
    parser.add_argument("--probe-sha256", required=True)
    parser.add_argument("--recording-ms", type=int, default=1500)
    parser.add_argument("--public-format", choices=("float32", "pcm16"), default="float32")
    parser.add_argument("--streaming-single", nargs=2, type=int, metavar=("RATE", "CHANNELS"))
    parser.add_argument(
        "--flow-experiment",
        action="store_true",
        help="Audit paired wait/chunks as diagnostic only; does not establish normal-mode acceptance",
    )
    parser.add_argument(
        "--native-flow", action="store_true", help="Require native paired flow version 1"
    )
    args = parser.parse_args()
    require(
        not args.streaming_single or args.public_format == "float32",
        "Stress recorder requires float32",
    )
    require(
        not args.flow_experiment or args.streaming_single, "Flow audit requires streaming recording"
    )
    require(not (args.native_flow and args.flow_experiment), "Ambiguous flow mode")
    report = json.loads(args.report.read_text(encoding="utf-8-sig"))
    result = (
        verify_streaming_recording(
            report,
            args.probe_sha256,
            *args.streaming_single,
            args.recording_ms,
            args.flow_experiment,
            args.native_flow,
        )
        if args.streaming_single
        else verify_matrix(
            report, args.probe_sha256, args.recording_ms, args.public_format, args.native_flow
        )
    )
    print(json.dumps(result))


if __name__ == "__main__":
    main()
