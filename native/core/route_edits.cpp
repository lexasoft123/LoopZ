#include <loopz/route_edits.hpp>

#include <algorithm>
#include <stdexcept>
#include <unordered_set>

namespace loopz::edit {
namespace {
VirtualDeviceDefinition& deviceFor(RouteDocument& document, const std::string& id) {
    const auto found =
        std::find_if(document.devices.begin(), document.devices.end(), [&](const auto& device) {
            return device.id == id;
        });
    if (found == document.devices.end())
        throw std::invalid_argument("Virtual device does not exist");
    return *found;
}

template <typename Item>
Item& itemFor(std::vector<Item>& items, const std::string& id, const char* error) {
    const auto found = std::find_if(items.begin(), items.end(), [&](const auto& item) {
        return item.id == id;
    });
    if (found == items.end())
        throw std::invalid_argument(error);
    return *found;
}

SourceRouting& sourceFor(VirtualDeviceDefinition& device, const std::string& id) {
    return itemFor(device.routing.sources, id, "Routing source does not exist");
}
SourceBinding& bindingFor(VirtualDeviceDefinition& device, const std::string& id) {
    return itemFor(device.bindings, id, "Source binding does not exist");
}
MonitorDefinition& monitorFor(VirtualDeviceDefinition& device, const std::string& id) {
    return itemFor(device.monitors, id, "Monitor does not exist");
}

template <typename Item> void controls(Item& item, const Controls& patch) {
    if (patch.gain)
        item.gain = *patch.gain;
    if (patch.enabled)
        item.enabled = *patch.enabled;
    if (patch.muted)
        item.muted = *patch.muted;
}

void trimWiring(std::vector<ChannelConnection>& connections, uint32_t inputs, uint32_t outputs) {
    std::erase_if(connections, [&](const auto& wire) {
        return wire.sourceChannel >= inputs || wire.outputChannel >= outputs;
    });
}

template <typename Item>
void reorder(std::vector<Item>& items, const std::vector<std::string>& ids) {
    if (ids.size() != items.size())
        throw std::invalid_argument("Reordering must include every existing identity once");
    std::unordered_set<std::string> seen;
    std::vector<Item> ordered;
    ordered.reserve(items.size());
    for (const auto& id : ids) {
        if (!seen.insert(id).second)
            throw std::invalid_argument("Reordering contains a duplicate identity");
        ordered.push_back(itemFor(items, id, "Reordering contains an unknown identity"));
    }
    items = std::move(ordered);
}

class Reducer {
  public:
    explicit Reducer(RouteDocument& document) : document_(document) {}

    void operator()(const CreateDevice& command) {
        VirtualDeviceDefinition device;
        device.id = command.id;
        device.name = command.name;
        device.sampleRate = command.sampleRate;
        device.routing.outputChannels = command.channels;
        if (command.passThrough) {
            device.routing.sources.push_back(
                {"pass-through", command.channels, 1, true, false,
                 defaultChannelConnections(command.channels, command.channels)});
            device.bindings.push_back({"pass-through", SourceKind::PassThrough, "", false});
        }
        document_.devices.push_back(std::move(device));
    }
    void operator()(const DuplicateDevice& command) {
        auto duplicate = deviceFor(document_, command.device);
        duplicate.id = command.id;
        duplicate.name = command.name;
        // A native format-request receipt belongs to the original device.
        duplicate.formatRequestReceipt.clear();
        document_.devices.push_back(std::move(duplicate));
    }
    void operator()(const RenameDevice& command) {
        deviceFor(document_, command.device).name = command.name;
    }
    void operator()(const DeleteDevice& command) {
        removeDevice(document_, command.device);
    }
    void operator()(const ResizeDevice& command) {
        resizeDeviceOutputs(document_, command.device, command.channels);
    }
    void operator()(const RemoveOutputPair& command) {
        removeDeviceOutputPair(document_, command.device, command.firstChannel);
    }
    void operator()(const SetDeviceControls& command) {
        auto& route = deviceFor(document_, command.device).routing;
        controls(route, command.controls);
        if (command.safetyLimiter)
            route.safetyLimiter = *command.safetyLimiter;
    }
    void operator()(const SetDeviceRate& command) {
        deviceFor(document_, command.device).sampleRate = command.sampleRate;
    }
    void operator()(const AddSource& command) {
        auto& device = deviceFor(document_, command.device);
        uint32_t channels = command.channels;
        switch (command.kind) {
        case SourceKind::Application:
            channels = 2;
            break;
        case SourceKind::InputDevice:
            break;
        case SourceKind::VirtualDevice:
            channels = deviceFor(document_, command.selector).routing.outputChannels;
            break;
        case SourceKind::PassThrough:
            channels = device.routing.outputChannels;
            break;
        default:
            throw std::invalid_argument("Unknown source kind");
        }
        SourceRouting source;
        source.id = command.id;
        source.channels = channels;
        if (command.connectByDefault)
            source.connections = defaultChannelConnections(channels, device.routing.outputChannels);
        device.routing.sources.push_back(std::move(source));
        device.bindings.push_back(
            {command.id, command.kind, command.selector,
             command.kind == SourceKind::Application && command.muteOriginal});
    }
    void operator()(const RemoveSource& command) {
        auto& device = deviceFor(document_, command.device);
        (void)sourceFor(device, command.source);
        std::erase_if(device.routing.sources, [&](const auto& source) {
            return source.id == command.source;
        });
        std::erase_if(device.bindings, [&](const auto& binding) {
            return binding.id == command.source;
        });
    }
    void operator()(const SetSourceControls& command) {
        auto& device = deviceFor(document_, command.device);
        controls(sourceFor(device, command.source), command.controls);
        if (command.muteOriginal) {
            auto& binding = bindingFor(device, command.source);
            if (binding.kind != SourceKind::Application)
                throw std::invalid_argument("Original playback mute applies only to applications");
            binding.muteOriginal = *command.muteOriginal;
        }
    }
    void operator()(const SetSourceWiring& command) {
        sourceFor(deviceFor(document_, command.device), command.source).connections =
            command.connections;
    }
    void operator()(const SetInputChannels& command) {
        auto& device = deviceFor(document_, command.device);
        if (bindingFor(device, command.source).kind != SourceKind::InputDevice)
            throw std::invalid_argument(
                "Only physical input channel counts can be edited directly");
        auto& source = sourceFor(device, command.source);
        source.channels = command.channels;
        trimWiring(source.connections, source.channels, device.routing.outputChannels);
    }
    void operator()(const SetSourceSelector& command) {
        auto& device = deviceFor(document_, command.device);
        auto& binding = bindingFor(device, command.source);
        if (binding.kind == SourceKind::PassThrough)
            throw std::invalid_argument("Pass-through has no external selector");
        binding.selector = command.selector;
        if (binding.kind == SourceKind::VirtualDevice) {
            auto& source = sourceFor(device, command.source);
            source.channels = deviceFor(document_, command.selector).routing.outputChannels;
            trimWiring(source.connections, source.channels, device.routing.outputChannels);
        }
    }
    void operator()(const AddMonitor& command) {
        auto& device = deviceFor(document_, command.device);
        MonitorDefinition monitor;
        monitor.id = command.id;
        monitor.deviceSelector = command.selector;
        monitor.channels = command.channels;
        if (command.connectByDefault)
            monitor.connections =
                defaultChannelConnections(device.routing.outputChannels, command.channels);
        device.monitors.push_back(std::move(monitor));
    }
    void operator()(const RemoveMonitor& command) {
        auto& device = deviceFor(document_, command.device);
        (void)monitorFor(device, command.monitor);
        std::erase_if(device.monitors, [&](const auto& monitor) {
            return monitor.id == command.monitor;
        });
    }
    void operator()(const SetMonitorControls& command) {
        controls(monitorFor(deviceFor(document_, command.device), command.monitor),
                 command.controls);
    }
    void operator()(const SetMonitorWiring& command) {
        monitorFor(deviceFor(document_, command.device), command.monitor).connections =
            command.connections;
    }
    void operator()(const SetMonitorEndpoint& command) {
        auto& device = deviceFor(document_, command.device);
        auto& monitor = monitorFor(device, command.monitor);
        monitor.deviceSelector = command.selector;
        monitor.channels = command.channels;
        trimWiring(monitor.connections, device.routing.outputChannels, monitor.channels);
    }
    void operator()(const ReorderDevices& command) {
        reorder(document_.devices, command.ids);
    }
    void operator()(const ReorderSources& command) {
        auto& device = deviceFor(document_, command.device);
        reorder(device.routing.sources, command.ids);
        reorder(device.bindings, command.ids);
    }
    void operator()(const ReorderMonitors& command) {
        reorder(deviceFor(document_, command.device).monitors, command.ids);
    }

  private:
    RouteDocument& document_;
};
} // namespace

RouteDocument applyCommands(const RouteDocument& document, std::span<const Command> commands) {
    if (commands.empty() || commands.size() > kMaximumCommands)
        throw std::invalid_argument("An edit requires between 1 and 128 commands");
    validateRouteDocument(document);
    RouteDocument candidate = document;
    candidate.version = kRouteDocumentVersion;
    Reducer reducer(candidate);
    for (const auto& command : commands) {
        std::visit(reducer, command);
        validateRouteDocument(candidate);
    }
    return candidate;
}
} // namespace loopz::edit
