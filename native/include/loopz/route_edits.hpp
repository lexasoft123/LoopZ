#pragma once

#include <loopz/route_document.hpp>

#include <optional>
#include <variant>

namespace loopz::edit {

inline constexpr size_t kMaximumCommands = 128;

// Stable identities come from the caller's platform identity generator. An edit
// never derives an identity from a display name or a position in the device list.
struct CreateDevice {
    std::string id, name;
    uint32_t channels{2};
    double sampleRate{48000};
    bool passThrough{true};
};
struct DuplicateDevice {
    std::string device, id, name;
};
struct RenameDevice {
    std::string device, name;
};
struct DeleteDevice {
    std::string device;
};
struct ResizeDevice {
    std::string device;
    uint32_t channels;
};
struct RemoveOutputPair {
    std::string device;
    uint32_t firstChannel;
};
struct Controls {
    std::optional<float> gain;
    std::optional<bool> enabled, muted;
};
struct SetDeviceControls {
    std::string device;
    Controls controls;
    std::optional<bool> safetyLimiter;
};
struct SetDeviceRate {
    std::string device;
    double sampleRate;
};
struct AddSource {
    std::string device, id;
    SourceKind kind;
    std::string selector;
    // Used only for physical inputs. Application capture is stereo; virtual and
    // pass-through channel counts follow the selected device automatically.
    uint32_t channels{2};
    bool muteOriginal{true};
    bool connectByDefault{true};
};
struct RemoveSource {
    std::string device, source;
};
struct SetSourceControls {
    std::string device, source;
    Controls controls;
    std::optional<bool> muteOriginal;
};
struct SetSourceWiring {
    std::string device, source;
    std::vector<ChannelConnection> connections;
};
struct SetInputChannels {
    std::string device, source;
    uint32_t channels;
};
struct SetSourceSelector {
    std::string device, source, selector;
};
struct AddMonitor {
    std::string device, id, selector;
    uint32_t channels{2};
    bool connectByDefault{true};
};
struct RemoveMonitor {
    std::string device, monitor;
};
struct SetMonitorControls {
    std::string device, monitor;
    Controls controls;
};
struct SetMonitorWiring {
    std::string device, monitor;
    std::vector<ChannelConnection> connections;
};
struct SetMonitorEndpoint {
    std::string device, monitor, selector;
    uint32_t channels;
};
struct ReorderDevices {
    std::vector<std::string> ids;
};
struct ReorderSources {
    std::string device;
    std::vector<std::string> ids;
};
struct ReorderMonitors {
    std::string device;
    std::vector<std::string> ids;
};

using Command =
    std::variant<CreateDevice, DuplicateDevice, RenameDevice, DeleteDevice, ResizeDevice,
                 SetDeviceControls, SetDeviceRate, AddSource, RemoveSource, SetSourceControls,
                 SetSourceWiring, SetInputChannels, SetSourceSelector, AddMonitor, RemoveMonitor,
                 SetMonitorControls, SetMonitorWiring, SetMonitorEndpoint, ReorderDevices,
                 ReorderSources, ReorderMonitors, RemoveOutputPair>;

// Control-thread reducer shared by every editor frontend. Each command must
// preserve a valid graph, including disabled dependencies. The batch is all or
// nothing: it returns a validated candidate and never mutates its input. A
// backend must persist/activate the candidate through its configuration owner;
// returning a candidate does not mean that native devices have changed.
RouteDocument applyCommands(const RouteDocument& document, std::span<const Command> commands);

} // namespace loopz::edit
