#pragma once

#include <loopz/routing.hpp>

namespace loopz {

inline constexpr uint32_t kRouteDocumentVersion = 2;
inline constexpr uint32_t kMaximumVirtualDevices = 32;

enum class SourceKind { Application, InputDevice, VirtualDevice, PassThrough };

// Selectors are opaque application/device identities interpreted by a backend.
// A VirtualDevice selector is another device's stable document id.
struct SourceBinding {
    std::string id;
    SourceKind kind;
    std::string selector;
    bool muteOriginal{true};
};

struct MonitorDefinition {
    std::string id;
    std::string deviceSelector;
    uint32_t channels{2};
    float gain{1};
    bool enabled{true};
    bool muted{false};
    std::vector<ChannelConnection> connections;
};

struct VirtualDeviceDefinition {
    std::string id;
    std::string name;
    double sampleRate{48000};
    RoutingConfiguration routing;
    std::vector<SourceBinding> bindings;
    std::vector<MonitorDefinition> monitors;
    // Opaque durable receipt for a backend format request. Kept with the
    // settings so replay after a helper restart cannot overwrite a newer edit.
    std::string formatRequestReceipt;
};

struct RouteDocument {
    uint32_t version{kRouteDocumentVersion};
    std::vector<VirtualDeviceDefinition> devices;
};

// Control-thread operations. Validation includes all stored virtual-device
// dependencies, including disabled routes, to prevent re-enable feedback loops.
void validateRouteDocument(const RouteDocument& document);
RoutingConfiguration monitorRouting(const VirtualDeviceDefinition& device,
                                    const MonitorDefinition& monitor);
bool hasPlaybackEndpoint(const VirtualDeviceDefinition& device) noexcept;

// Preserves existing wiring, adds only new pass-through identity wires, removes
// deleted sockets and updates dependent virtual sources. Failed edits leave the
// original document intact. Backend activation is a separate transaction.
void resizeDeviceOutputs(RouteDocument& document, const std::string& deviceId, uint32_t channels);

// Remove an arbitrary stereo pair (zero-based even firstChannel). Surviving
// channels keep their relative order; source, pass-through, monitor and direct
// virtual-source wiring follows the renumbering, preserving all surviving gains.
void removeDeviceOutputPair(RouteDocument& document, const std::string& deviceId,
                            uint32_t firstChannel);

// Rejects deletion while another stored source references the device, including
// disabled sources. The caller must explicitly remove those connections first.
void removeDevice(RouteDocument& document, const std::string& deviceId);

} // namespace loopz
