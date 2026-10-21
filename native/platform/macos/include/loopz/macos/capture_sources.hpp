#pragma once

#include <loopz/source_registry.hpp>
#include <loopz/audio_delivery_timing.hpp>
#include <loopz/audio_resampler.hpp>
#include <loopz/audio_history.hpp>
#include <optional>

namespace loopz::macos {

// Construction allocates reader-visible state only: no tap, microphone,
// permission request or device format/buffer mutation. The registry acquires
// the endpoint after a committed scene requests it. Input selectors may be a
// concrete UID or "default"; LoopZ's native inputs are rejected as physical
// sources so internal routing remains subject to document cycle validation.
std::shared_ptr<SourceEndpoint> createCaptureSource(SourceKind kind, const std::string& selector,
                                                    uint32_t channelCapacity);

struct CaptureSourceOptions {
    // Applies only to each private process-tap aggregate, never to the
    // application's selected physical output or its shared buffer period.
    uint32_t applicationBufferFrames{128};
    bool deliveryDiagnostics{false};
    // Explicit diagnostic input mode only. App taps retain their existing
    // history filter; no default capture or automatic timing policy changes.
    ResamplingFilter inputFilter{ResamplingFilter::LinearPhase};
};
// Validate before constructing endpoints or acquiring native resources.
SourceRegistry::Factory captureSourceFactory(CaptureSourceOptions options = {});
// Diagnostic control owner only, after close() has joined all input callbacks.
// Running sources and sources without diagnostics return no snapshot.
std::optional<AudioDeliverySnapshot> stoppedInputDelivery(const SourceEndpoint& source);
// Diagnostic counters only; a live read is an independent atomic observation.
// Read after stopping the runtime for the final joined-reader snapshot.
std::optional<AudioHistoryReadSnapshot> inputHistoryReadDiagnostics(const SourceEndpoint& source);
// Bound observations before stopping other hardware clients. Snapshot remains
// available only after this source's callbacks have joined.
void finishInputDelivery(SourceEndpoint& source) noexcept;
} // namespace loopz::macos
