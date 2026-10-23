#include <loopz/document_json.hpp>
#include <algorithm>
#include <cmath>
#include <stdexcept>

namespace loopz {
namespace {
[[noreturn]] void invalid() {
    throw std::invalid_argument("Invalid routing JSON field or value");
}
void fields(const JSON& v, std::initializer_list<const char*> required,
            std::initializer_list<const char*> optional = {}) {
    if (!v.is_object())
        invalid();
    for (auto key : required)
        if (!v.contains(key))
            invalid();
    for (const auto& entry : v.items()) {
        const auto match = [&](auto keys) {
            return std::any_of(keys.begin(), keys.end(), [&](const auto* k) {
                return entry.key() == k;
            });
        };
        if (!match(required) && !match(optional))
            invalid();
    }
}
const JSON& array(const JSON& v, size_t maximum) {
    if (!v.is_array() || v.size() > maximum)
        invalid();
    return v;
}
std::string text(const JSON& v) {
    if (!v.is_string())
        invalid();
    auto s = v.get<std::string>();
    if (s.size() > 4096 || s.find('\0') != std::string::npos)
        invalid();
    return s;
}
double number(const JSON& v) {
    if (!v.is_number())
        invalid();
    const double n = v.get<double>();
    if (!std::isfinite(n))
        invalid();
    return n;
}
uint32_t integer(const JSON& v) {
    const auto n = number(v);
    if (n < 0 || n > UINT32_MAX || std::floor(n) != n)
        invalid();
    return static_cast<uint32_t>(n);
}
bool boolean(const JSON& v) {
    if (!v.is_boolean())
        invalid();
    return v.get<bool>();
}
float gain(const JSON& v) {
    const auto n = number(v);
    if (n < 0 || n > 1)
        invalid();
    return static_cast<float>(n);
}
SourceKind kind(const JSON& v) {
    const auto s = text(v);
    if (s == "application")
        return SourceKind::Application;
    if (s == "input-device")
        return SourceKind::InputDevice;
    if (s == "virtual-device")
        return SourceKind::VirtualDevice;
    if (s == "pass-through")
        return SourceKind::PassThrough;
    invalid();
}
const char* kind(SourceKind v) {
    switch (v) {
    case SourceKind::Application:
        return "application";
    case SourceKind::InputDevice:
        return "input-device";
    case SourceKind::VirtualDevice:
        return "virtual-device";
    case SourceKind::PassThrough:
        return "pass-through";
    }
    invalid();
}
JSON wires(const std::vector<ChannelConnection>& v) {
    auto a = JSON::array();
    for (const auto& w : v)
        a.push_back({{"sourceChannel", w.sourceChannel},
                     {"outputChannel", w.outputChannel},
                     {"gain", w.gain}});
    return a;
}
std::vector<ChannelConnection> wires(const JSON& v) {
    std::vector<ChannelConnection> result;
    for (const auto& w : array(v, kMaximumRoutingChannels * kMaximumRoutingChannels)) {
        fields(w, {"sourceChannel", "outputChannel", "gain"});
        result.push_back(
            {integer(w.at("sourceChannel")), integer(w.at("outputChannel")), gain(w.at("gain"))});
    }
    return result;
}
edit::Controls controls(const JSON& v) {
    fields(v, {}, {"gain", "enabled", "muted"});
    edit::Controls c;
    if (v.contains("gain"))
        c.gain = gain(v.at("gain"));
    if (v.contains("enabled"))
        c.enabled = boolean(v.at("enabled"));
    if (v.contains("muted"))
        c.muted = boolean(v.at("muted"));
    return c;
}
std::vector<std::string> ids(const JSON& v, size_t maximum) {
    std::vector<std::string> result;
    for (const auto& id : array(v, maximum))
        result.push_back(text(id));
    return result;
}
} // namespace

JSON parseControlJSON(std::string_view bytes) {
    if (bytes.empty() || bytes.size() > 1024 * 1024)
        invalid();
    return JSON::parse(bytes, [](int depth, JSON::parse_event_t, JSON&) {
        if (depth > 32)
            throw std::invalid_argument("Routing JSON is nested too deeply");
        return true;
    });
}

JSON encodeDocument(const RouteDocument& document) {
    validateRouteDocument(document);
    JSON result{{"version", document.version}, {"devices", JSON::array()}};
    for (const auto& d : document.devices) {
        const auto& r = d.routing;
        JSON routing{{"outputChannels", r.outputChannels},
                     {"gain", r.gain},
                     {"enabled", r.enabled},
                     {"muted", r.muted},
                     {"safetyLimiter", r.safetyLimiter},
                     {"sources", JSON::array()}};
        for (const auto& s : r.sources)
            routing["sources"].push_back({{"id", s.id},
                                          {"channels", s.channels},
                                          {"gain", s.gain},
                                          {"enabled", s.enabled},
                                          {"muted", s.muted},
                                          {"connections", wires(s.connections)}});
        JSON bindings = JSON::array(), monitors = JSON::array();
        for (const auto& b : d.bindings)
            bindings.push_back({{"id", b.id},
                                {"kind", kind(b.kind)},
                                {"selector", b.selector},
                                {"muteOriginal", b.muteOriginal}});
        for (const auto& m : d.monitors)
            monitors.push_back({{"id", m.id},
                                {"deviceSelector", m.deviceSelector},
                                {"channels", m.channels},
                                {"gain", m.gain},
                                {"enabled", m.enabled},
                                {"muted", m.muted},
                                {"connections", wires(m.connections)}});
        result["devices"].push_back({{"id", d.id},
                                     {"name", d.name},
                                     {"sampleRate", d.sampleRate},
                                     {"routing", routing},
                                     {"bindings", bindings},
                                     {"monitors", monitors},
                                     {"formatRequestReceipt", d.formatRequestReceipt}});
    }
    return result;
}

RouteDocument decodeDocument(const JSON& v) {
    fields(v, {"version", "devices"});
    RouteDocument result;
    result.version = integer(v.at("version"));
    for (const auto& d : array(v.at("devices"), kMaximumVirtualDevices)) {
        fields(d, {"id", "name", "sampleRate", "routing", "bindings", "monitors"},
               {"formatRequestReceipt"});
        VirtualDeviceDefinition device;
        device.id = text(d.at("id"));
        device.name = text(d.at("name"));
        device.sampleRate = number(d.at("sampleRate"));
        if (d.contains("formatRequestReceipt"))
            device.formatRequestReceipt = text(d.at("formatRequestReceipt"));
        const auto& r = d.at("routing");
        fields(r, {"outputChannels", "gain", "enabled", "muted", "safetyLimiter", "sources"});
        device.routing.outputChannels = integer(r.at("outputChannels"));
        device.routing.gain = gain(r.at("gain"));
        device.routing.enabled = boolean(r.at("enabled"));
        device.routing.muted = boolean(r.at("muted"));
        device.routing.safetyLimiter = boolean(r.at("safetyLimiter"));
        for (const auto& s : array(r.at("sources"), kMaximumRoutingSources)) {
            fields(s, {"id", "channels", "gain", "enabled", "muted", "connections"});
            device.routing.sources.push_back({text(s.at("id")), integer(s.at("channels")),
                                              gain(s.at("gain")), boolean(s.at("enabled")),
                                              boolean(s.at("muted")), wires(s.at("connections"))});
        }
        for (const auto& b : array(d.at("bindings"), kMaximumRoutingSources)) {
            fields(b, {"id", "kind", "selector", "muteOriginal"});
            device.bindings.push_back({text(b.at("id")), kind(b.at("kind")), text(b.at("selector")),
                                       boolean(b.at("muteOriginal"))});
        }
        for (const auto& m : array(d.at("monitors"), 32)) {
            fields(m,
                   {"id", "deviceSelector", "channels", "gain", "enabled", "muted", "connections"});
            device.monitors.push_back({text(m.at("id")), text(m.at("deviceSelector")),
                                       integer(m.at("channels")), gain(m.at("gain")),
                                       boolean(m.at("enabled")), boolean(m.at("muted")),
                                       wires(m.at("connections"))});
        }
        result.devices.push_back(std::move(device));
    }
    validateRouteDocument(result);
    return result;
}

std::vector<edit::Command> decodeCommands(const JSON& value) {
    using namespace edit;
    std::vector<Command> result;
    for (const auto& v : array(value, kMaximumCommands)) {
        const auto t = text(v.at("type"));
        const auto s = [&](const char* key) {
            return text(v.at(key));
        };
        const auto n = [&](const char* key) {
            return integer(v.at(key));
        };
        if (t == "createDevice") {
            fields(v, {"type", "id", "name"}, {"channels", "sampleRate", "passThrough"});
            CreateDevice c{s("id"), s("name")};
            if (v.contains("channels"))
                c.channels = n("channels");
            if (v.contains("sampleRate"))
                c.sampleRate = number(v.at("sampleRate"));
            if (v.contains("passThrough"))
                c.passThrough = boolean(v.at("passThrough"));
            result.push_back(c);
        } else if (t == "duplicateDevice") {
            fields(v, {"type", "device", "id", "name"});
            result.push_back(DuplicateDevice{s("device"), s("id"), s("name")});
        } else if (t == "renameDevice") {
            fields(v, {"type", "device", "name"});
            result.push_back(RenameDevice{s("device"), s("name")});
        } else if (t == "deleteDevice") {
            fields(v, {"type", "device"});
            result.push_back(DeleteDevice{s("device")});
        } else if (t == "resizeDevice") {
            fields(v, {"type", "device", "channels"});
            result.push_back(ResizeDevice{s("device"), n("channels")});
        } else if (t == "removeOutputPair") {
            fields(v, {"type", "device", "firstChannel"});
            result.push_back(RemoveOutputPair{s("device"), n("firstChannel")});
        } else if (t == "setDeviceRate") {
            fields(v, {"type", "device", "sampleRate"});
            result.push_back(SetDeviceRate{s("device"), number(v.at("sampleRate"))});
        } else if (t == "setDeviceControls") {
            fields(v, {"type", "device", "controls"}, {"safetyLimiter"});
            SetDeviceControls c{s("device"), controls(v.at("controls")), {}};
            if (v.contains("safetyLimiter"))
                c.safetyLimiter = boolean(v.at("safetyLimiter"));
            result.push_back(c);
        } else if (t == "addSource") {
            fields(v, {"type", "device", "id", "kind", "selector"},
                   {"channels", "muteOriginal", "connectByDefault"});
            AddSource c{s("device"), s("id"), kind(v.at("kind")), s("selector")};
            if (v.contains("channels"))
                c.channels = n("channels");
            if (v.contains("muteOriginal"))
                c.muteOriginal = boolean(v.at("muteOriginal"));
            if (v.contains("connectByDefault"))
                c.connectByDefault = boolean(v.at("connectByDefault"));
            result.push_back(c);
        } else if (t == "removeSource") {
            fields(v, {"type", "device", "source"});
            result.push_back(RemoveSource{s("device"), s("source")});
        } else if (t == "setSourceControls") {
            fields(v, {"type", "device", "source", "controls"}, {"muteOriginal"});
            SetSourceControls c{s("device"), s("source"), controls(v.at("controls")), {}};
            if (v.contains("muteOriginal"))
                c.muteOriginal = boolean(v.at("muteOriginal"));
            result.push_back(c);
        } else if (t == "setSourceWiring") {
            fields(v, {"type", "device", "source", "connections"});
            result.push_back(SetSourceWiring{s("device"), s("source"), wires(v.at("connections"))});
        } else if (t == "setInputChannels") {
            fields(v, {"type", "device", "source", "channels"});
            result.push_back(SetInputChannels{s("device"), s("source"), n("channels")});
        } else if (t == "setSourceSelector") {
            fields(v, {"type", "device", "source", "selector"});
            result.push_back(SetSourceSelector{s("device"), s("source"), s("selector")});
        } else if (t == "addMonitor") {
            fields(v, {"type", "device", "id", "selector"}, {"channels", "connectByDefault"});
            AddMonitor c{s("device"), s("id"), s("selector")};
            if (v.contains("channels"))
                c.channels = n("channels");
            if (v.contains("connectByDefault"))
                c.connectByDefault = boolean(v.at("connectByDefault"));
            result.push_back(c);
        } else if (t == "removeMonitor") {
            fields(v, {"type", "device", "monitor"});
            result.push_back(RemoveMonitor{s("device"), s("monitor")});
        } else if (t == "setMonitorControls") {
            fields(v, {"type", "device", "monitor", "controls"});
            result.push_back(
                SetMonitorControls{s("device"), s("monitor"), controls(v.at("controls"))});
        } else if (t == "setMonitorWiring") {
            fields(v, {"type", "device", "monitor", "connections"});
            result.push_back(
                SetMonitorWiring{s("device"), s("monitor"), wires(v.at("connections"))});
        } else if (t == "setMonitorEndpoint") {
            fields(v, {"type", "device", "monitor", "selector", "channels"});
            result.push_back(
                SetMonitorEndpoint{s("device"), s("monitor"), s("selector"), n("channels")});
        } else if (t == "reorderDevices") {
            fields(v, {"type", "ids"});
            result.push_back(ReorderDevices{ids(v.at("ids"), kMaximumVirtualDevices)});
        } else if (t == "reorderSources") {
            fields(v, {"type", "device", "ids"});
            result.push_back(ReorderSources{s("device"), ids(v.at("ids"), kMaximumRoutingSources)});
        } else if (t == "reorderMonitors") {
            fields(v, {"type", "device", "ids"});
            result.push_back(ReorderMonitors{s("device"), ids(v.at("ids"), 32)});
        } else
            invalid();
    }
    return result;
}
} // namespace loopz
