#pragma once

namespace loopz::windows::probe {
// Acceptance fixture only. Always submits silent buffers, including while
// automatic stream routing changes the underlying endpoint.
int automaticSilence(unsigned seconds);
// Stops/reset the silent client before returning when the control predicate is
// true. This cannot submit nonzero samples on an automatic/default endpoint.
int automaticSilenceUntil(unsigned seconds, bool (*finished)(const void*), const void* context);
// Emits a marker only while the caller confirms an owned virtual Feed is the
// active endpoint. All other endpoints receive only silent buffers.
int automaticVirtualTone(unsigned seconds, bool (*allowed)(const void*), const void* context);
} // namespace loopz::windows::probe
