// Compile-only analysis of shared routines called on the small kernel stack.
// Maximum-sized wire packets are caller-owned nonpaged buffers, never locals.
#include "../native/platform/windows/driver/duplex_bridge.hpp"

extern "C" bool loopzAnalyzePlayback(loopz_driver::DuplexBridge* bridge,
                                     unsigned long long generation, unsigned long long now,
                                     loopz_driver::DuplexBridge::Packet* packet) {
    return bridge->takePlayback(generation, now, *packet);
}
extern "C" bool loopzAnalyzeIngress(loopz_driver::DuplexBridge* bridge,
                                    const unsigned char* samples, unsigned frames,
                                    unsigned long long time, unsigned long long now) {
    return bridge->pushPlayback(samples, frames, time, now);
}
extern "C" void loopzAnalyzeRecording(loopz_driver::DuplexBridge* bridge, unsigned char* samples,
                                      unsigned frames, unsigned long long now) {
    bridge->readRecording(samples, frames, now);
}
