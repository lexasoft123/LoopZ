#include "../native/platform/windows/probe_recording_limits.hpp"
#include <cassert>
#include <cstdint>
#include <initializer_list>

int main() {
    using loopz::windows::validProbeRecording;
    for (unsigned rate : {44100u, 48000u, 88200u, 96000u, 176400u, 192000u})
        for (unsigned channels = 2; channels <= 64; channels += 2)
            assert(validProbeRecording(rate, channels, 30000));
    assert(validProbeRecording(48000, 2, 300000));
    assert(validProbeRecording(88200, 54, 60000));
    assert(!validProbeRecording(88200, 54, 120000));
    assert(!validProbeRecording(192000, 64, 31000));
    assert(!validProbeRecording(48000, 2, 300001));
    assert(!validProbeRecording(48000, 2, 1499));
    assert(!validProbeRecording(48000, 3, 1500));
    assert(!validProbeRecording(0, 2, 1500));
    assert(!validProbeRecording(UINT32_MAX, UINT32_MAX, UINT32_MAX));
    using loopz::windows::validStressProbeRecording;
    assert(validStressProbeRecording(192000, 64, 300000));
    assert(validStressProbeRecording(192000, 64, 120000));
    assert(!validStressProbeRecording(192000, 64, 300001));
    assert(!validStressProbeRecording(48000, 2, 1499));
    assert(!validStressProbeRecording(48000, 3, 300000));
    assert(!validStressProbeRecording(UINT32_MAX, UINT32_MAX, UINT32_MAX));
}
