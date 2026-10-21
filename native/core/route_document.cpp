#include <loopz/route_document.hpp>

#include <algorithm>
#include <cmath>
#include <functional>
#include <stdexcept>
#include <unordered_map>
#include <unordered_set>

namespace loopz {
namespace {
bool validText(const std::string& text) {
    return !text.empty() && text.size() <= 4096 && text.find('\0') == std::string::npos;
}

bool validDeviceChannels(uint32_t channels) {
    return channels >= 2 && channels <= kMaximumRoutingChannels && channels % 2 == 0;
}

const SourceRouting& sourceFor(const VirtualDeviceDefinition& device, const std::string& id) {
    const auto source = std::find_if(device.routing.sources.begin(), device.routing.sources.end(),
                                     [&](const auto& value) {
                                         return value.id == id;
                                     });
    if (source == device.routing.sources.end())
        throw std::invalid_argument("Capture binding has no routing source");
    return *source;
}
} // namespace

RoutingConfiguration monitorRouting(const VirtualDeviceDefinition& device,
                                    const MonitorDefinition& monitor) {
    RoutingConfiguration result;
    result.outputChannels = monitor.channels;
    result.gain = monitor.gain;
    result.enabled = monitor.enabled;
    result.muted = monitor.muted;
    result.safetyLimiter = false;
    result.sources.push_back(
        {device.id, device.routing.outputChannels, 1, true, false, monitor.connections});
    validateRoutingConfiguration(result);
    return result;
}

void validateRouteDocument(const RouteDocument& document) {
    if ((document.version != 1 && document.version != kRouteDocumentVersion) ||
        document.devices.size() > kMaximumVirtualDevices)
        throw std::invalid_argument("Unsupported route document version or device count");
    std::unordered_map<std::string, size_t> indices;
    for (size_t index = 0; index < document.devices.size(); ++index) {
        const auto& device = document.devices[index];
        if ((!device.formatRequestReceipt.empty() &&
             (document.version < 2 || device.formatRequestReceipt.size() > 128 ||
              !validText(device.formatRequestReceipt))))
            throw std::invalid_argument("Invalid format request receipt");
        if (!validText(device.id) || !validText(device.name) ||
            !indices.emplace(device.id, index).second || !std::isfinite(device.sampleRate) ||
            device.sampleRate <= 0 || !validDeviceChannels(device.routing.outputChannels))
            throw std::invalid_argument("Invalid or duplicate virtual device");
        validateRoutingConfiguration(device.routing);
    }
    std::vector<std::vector<size_t>> dependencies(document.devices.size());
    for (size_t index = 0; index < document.devices.size(); ++index) {
        const auto& device = document.devices[index];
        if (device.bindings.size() != device.routing.sources.size() || device.monitors.size() > 32)
            throw std::invalid_argument("Missing source bindings or excessive monitors");
        std::unordered_set<std::string> ids;
        bool passThrough = false;
        for (const auto& binding : device.bindings) {
            if (!validText(binding.id) || !ids.insert(binding.id).second)
                throw std::invalid_argument("Invalid or duplicate source binding");
            const auto& source = sourceFor(device, binding.id);
            if (binding.kind != SourceKind::PassThrough && !validText(binding.selector))
                throw std::invalid_argument("Source selector is missing");
            switch (binding.kind) {
            case SourceKind::Application:
                if (source.channels != 2)
                    throw std::invalid_argument("Application capture is stereo");
                break;
            case SourceKind::InputDevice:
                break;
            case SourceKind::PassThrough:
                if (passThrough || !binding.selector.empty() ||
                    source.channels != device.routing.outputChannels)
                    throw std::invalid_argument("Invalid pass-through source");
                passThrough = true;
                break;
            case SourceKind::VirtualDevice: {
                const auto target = indices.find(binding.selector);
                if (target == indices.end() ||
                    source.channels != document.devices[target->second].routing.outputChannels)
                    throw std::invalid_argument(
                        "Missing virtual source or mismatched channel count");
                dependencies[index].push_back(target->second);
                break;
            }
            default:
                throw std::invalid_argument("Unknown source kind");
            }
        }
        for (const auto& monitor : device.monitors) {
            if (!validText(monitor.id) || !ids.insert(monitor.id).second ||
                !validText(monitor.deviceSelector))
                throw std::invalid_argument("Invalid or duplicate monitor");
            (void)monitorRouting(device, monitor);
        }
    }
    std::vector<uint8_t> state(document.devices.size());
    std::function<void(size_t)> visit = [&](size_t index) {
        if (state[index] == 1)
            throw std::invalid_argument("Virtual-device routing would create a feedback cycle");
        if (state[index] == 2)
            return;
        state[index] = 1;
        for (auto dependency : dependencies[index])
            visit(dependency);
        state[index] = 2;
    };
    for (size_t index = 0; index < document.devices.size(); ++index)
        visit(index);
}

bool hasPlaybackEndpoint(const VirtualDeviceDefinition& device) noexcept {
    if (!device.routing.enabled)
        return false;
    for (const auto& binding : device.bindings) {
        if (binding.kind != SourceKind::PassThrough)
            continue;
        for (const auto& source : device.routing.sources)
            if (source.id == binding.id && source.enabled)
                return true;
    }
    return false;
}

void removeDevice(RouteDocument& document, const std::string& deviceId) {
    validateRouteDocument(document);
    if (std::none_of(document.devices.begin(), document.devices.end(), [&](const auto& device) {
            return device.id == deviceId;
        }))
        throw std::invalid_argument("Virtual device does not exist");
    for (const auto& device : document.devices)
        for (const auto& binding : device.bindings)
            if (binding.kind == SourceKind::VirtualDevice && binding.selector == deviceId)
                throw std::invalid_argument("Virtual device is still used by another source");
    RouteDocument candidate = document;
    std::erase_if(candidate.devices, [&](const auto& device) {
        return device.id == deviceId;
    });
    validateRouteDocument(candidate);
    document = std::move(candidate);
}

void removeDeviceOutputPair(RouteDocument& document, const std::string& deviceId,
                            uint32_t firstChannel) {
    validateRouteDocument(document);
    const auto selected =
        std::find_if(document.devices.begin(), document.devices.end(), [&](const auto& device) {
            return device.id == deviceId;
        });
    if (selected == document.devices.end())
        throw std::invalid_argument("Virtual device does not exist");
    const auto previous = selected->routing.outputChannels;
    if (previous <= 2 || firstChannel % 2 || firstChannel >= previous)
        throw std::invalid_argument(
            "Choose an existing output pair; at least one pair must remain");
    const auto removed = [firstChannel](uint32_t channel) {
        return channel == firstChannel || channel == firstChannel + 1;
    };
    auto remap = [&](std::vector<ChannelConnection>& wires, bool input, bool output) {
        std::erase_if(wires, [&](const auto& wire) {
            return (input && removed(wire.sourceChannel)) ||
                   (output && removed(wire.outputChannel));
        });
        for (auto& wire : wires) {
            if (input && wire.sourceChannel > firstChannel + 1)
                wire.sourceChannel -= 2;
            if (output && wire.outputChannel > firstChannel + 1)
                wire.outputChannel -= 2;
        }
    };
    RouteDocument candidate = document;
    for (auto& device : candidate.devices) {
        const bool own = device.id == deviceId;
        if (own)
            device.routing.outputChannels -= 2;
        for (auto& source : device.routing.sources) {
            const auto binding = std::find_if(device.bindings.begin(), device.bindings.end(),
                                              [&](const auto& value) {
                                                  return value.id == source.id;
                                              });
            const bool input =
                (own && binding->kind == SourceKind::PassThrough) ||
                (binding->kind == SourceKind::VirtualDevice && binding->selector == deviceId);
            if (input)
                source.channels -= 2;
            if (own || input)
                remap(source.connections, input, own);
        }
        if (own)
            for (auto& monitor : device.monitors)
                remap(monitor.connections, true, false);
    }
    validateRouteDocument(candidate);
    document = std::move(candidate);
}

void resizeDeviceOutputs(RouteDocument& document, const std::string& deviceId, uint32_t channels) {
    validateRouteDocument(document);
    if (!validDeviceChannels(channels))
        throw std::invalid_argument("Virtual channels must be pairs from 2 through 64");
    RouteDocument candidate = document;
    const auto selected =
        std::find_if(candidate.devices.begin(), candidate.devices.end(), [&](const auto& device) {
            return device.id == deviceId;
        });
    if (selected == candidate.devices.end())
        throw std::invalid_argument("Virtual device does not exist");
    const auto previousChannels = selected->routing.outputChannels;
    selected->routing.outputChannels = channels;
    for (auto& device : candidate.devices) {
        for (auto& source : device.routing.sources) {
            const auto binding = std::find_if(device.bindings.begin(), device.bindings.end(),
                                              [&](const auto& value) {
                                                  return value.id == source.id;
                                              });
            const bool ownPass = device.id == deviceId && binding->kind == SourceKind::PassThrough;
            const bool dependent =
                binding->kind == SourceKind::VirtualDevice && binding->selector == deviceId;
            if (ownPass || dependent)
                source.channels = channels;
            if (device.id == deviceId || dependent)
                std::erase_if(source.connections, [&](const auto& wire) {
                    return wire.sourceChannel >= source.channels ||
                           wire.outputChannel >= device.routing.outputChannels;
                });
            if (ownPass)
                for (uint32_t channel = previousChannels; channel < channels; ++channel)
                    source.connections.push_back({channel, channel});
        }
        if (device.id == deviceId)
            for (auto& monitor : device.monitors)
                std::erase_if(monitor.connections, [&](const auto& wire) {
                    return wire.sourceChannel >= channels;
                });
    }
    validateRouteDocument(candidate);
    document = std::move(candidate);
}
} // namespace loopz
