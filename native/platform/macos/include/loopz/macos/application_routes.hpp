#pragma once

#include <loopz/macos/capture.hpp>
#include <loopz/application_processes.hpp>
#include <CoreAudio/CoreAudio.h>
#include <set>

namespace loopz::macos {
struct ApplicationAudioProcessIdentity {
    AudioObjectID object;
    ProcessInstance process;
    bool operator==(const ApplicationAudioProcessIdentity&) const = default;
};
struct ApplicationAudioSelection {
    std::vector<AudioObjectID> processes;
    std::set<AudioDeviceID> outputs;
    bool producing{false};
    std::vector<ApplicationAudioProcessIdentity> identities;
};
struct ApplicationOutputStream {
    AudioObjectID object;
    AudioStreamBasicDescription format;
    uint32_t firstChannel;
};
// All functions are capture-free control snapshots. Every failed/racing read
// invalidates the snapshot; unknown routes never become an empty selection.
bool selectApplicationAudio(const std::string& bundle, ApplicationAudioSelection& selection,
                            std::string& error);
bool applicationOutputStreams(AudioDeviceID device, std::vector<ApplicationOutputStream>& streams,
                              uint32_t& channels, std::string& error);
std::vector<ApplicationCaptureRoute> captureRoutes(const ApplicationAudioSelection& selected,
                                                   std::string& error);
} // namespace loopz::macos
