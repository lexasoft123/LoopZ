#include <loopz/device_intent.hpp>
#include <loopz/sample_rates.hpp>
#include <array>
#include <limits>
#include <set>
#include <stdexcept>

namespace loopz {
namespace {
[[noreturn]] void invalid() {
    throw std::invalid_argument("Invalid microphone setup request");
}
void fields(const JSON& value, std::initializer_list<const char*> names) {
    if (!value.is_object() || value.size() != names.size())
        invalid();
    for (const auto* name : names)
        if (!value.contains(name))
            invalid();
}
uint64_t integer(const JSON& value) {
    // Do not round floating-point revisions or reinterpret negative integers.
    if (!value.is_number_unsigned())
        invalid();
    return value.get<uint64_t>();
}
std::string text(const JSON& value) {
    if (!value.is_string())
        invalid();
    auto result = value.get<std::string>();
    if (result.empty() || result.size() > 4096 || result.find('\0') != std::string::npos)
        invalid();
    return result;
}
bool operationId(std::string_view id) {
    if (id.size() != 36)
        return false;
    for (size_t index = 0; index < id.size(); ++index) {
        const bool dash = index == 8 || index == 13 || index == 18 || index == 23;
        if (dash ? id[index] != '-'
                 : !((id[index] >= '0' && id[index] <= '9') ||
                     (id[index] >= 'a' && id[index] <= 'f')))
            return false;
    }
    return true;
}
} // namespace
DeviceIntentRequest decodeDeviceIntent(std::string_view bytes) {
    if (bytes.empty() || bytes.size() > kMaximumDeviceIntentBytes)
        invalid();
    // Reject duplicate keys rather than allowing an intermediary and the broker
    // to interpret the same request differently. The schema has two object levels.
    std::array<std::set<std::string>, 5> keys;
    const auto value = JSON::parse(bytes, [&](int depth, JSON::parse_event_t event, JSON& parsed) {
        if (depth < 0 || depth > 3)
            invalid();
        if (event == JSON::parse_event_t::object_start)
            keys[static_cast<size_t>(depth) + 1].clear();
        else if (event == JSON::parse_event_t::key &&
                 !keys[static_cast<size_t>(depth)].insert(parsed.get<std::string>()).second)
            invalid();
        return true;
    });
    if (!value.is_object() || !value.contains("action") || !value.at("action").is_string())
        invalid();
    const auto action = value.at("action").get<std::string>();
    if (!value.contains("version"))
        invalid();
    const auto version = integer(value.at("version"));
    if (version == 2) {
        fields(value, {"version", "operationId", "expectedRevision", "action", "kind"});
        if (value.at("kind") != "appRoutingSink" || (action != "upsert" && action != "remove"))
            invalid();
        DeviceIntentRequest sink;
        sink.kind = DeviceIntentKind::AppRoutingSink;
        sink.operationId = text(value.at("operationId"));
        sink.expectedRevision = integer(value.at("expectedRevision"));
        if (!operationId(sink.operationId) ||
            sink.expectedRevision == std::numeric_limits<uint64_t>::max())
            invalid();
        sink.action = action == "upsert" ? DeviceIntentAction::Upsert : DeviceIntentAction::Remove;
        return sink; // One fixed stereo 48 kHz sink per authenticated owner.
    }

    if (action == "upsert")
        fields(value, {"version", "operationId", "expectedRevision", "action", "device"});
    else if (action == "remove")
        fields(value, {"version", "operationId", "expectedRevision", "action", "mixId"});
    else
        invalid();
    if (integer(value.at("version")) != 1)
        invalid();
    DeviceIntentRequest result;
    result.operationId = text(value.at("operationId"));
    if (!operationId(result.operationId))
        invalid();
    result.expectedRevision = integer(value.at("expectedRevision"));
    if (result.expectedRevision == std::numeric_limits<uint64_t>::max())
        invalid(); // There must be room for the committed successor revision.
    if (action == "remove") {
        result.action = DeviceIntentAction::Remove;
        result.mixId = text(value.at("mixId"));
    } else {
        const auto& device = value.at("device");
        fields(device, {"id", "name", "sampleRate", "channels"});
        result.mixId = text(device.at("id"));
        result.name = text(device.at("name"));
        const auto rate = integer(device.at("sampleRate"));
        const auto channels = integer(device.at("channels"));
        if (rate > UINT32_MAX || channels > UINT32_MAX ||
            !supportedVirtualSampleRate(static_cast<double>(rate)) || channels == 0 ||
            channels > kMaximumRoutingChannels)
            invalid();
        result.sampleRate = static_cast<uint32_t>(rate);
        result.channels = static_cast<uint32_t>(channels);
    }
    return result;
}
std::string deviceIntentStorageKey(DeviceIntentKind kind, std::string_view mixId) {
    if (kind == DeviceIntentKind::AppRoutingSink) {
        if (!mixId.empty())
            invalid();
        return std::string(1, '\0') + "LoopZ.AppRoutingSink.v1";
    }
    if (kind != DeviceIntentKind::MicrophoneMix || mixId.empty() || mixId.size() > 4096 ||
        mixId.find('\0') != std::string_view::npos)
        invalid();
    return std::string(mixId);
}
JSON encodeDeviceIntent(const DeviceIntentRequest& request) {
    (void)deviceIntentStorageKey(request.kind, request.mixId);
    if (request.kind == DeviceIntentKind::AppRoutingSink) {
        if (!request.name.empty() || request.sampleRate != 48000 || request.channels != 2 ||
            (request.action != DeviceIntentAction::Upsert &&
             request.action != DeviceIntentAction::Remove))
            invalid();
        JSON sink{{"version", 2U},
                  {"kind", "appRoutingSink"},
                  {"operationId", request.operationId},
                  {"expectedRevision", request.expectedRevision},
                  {"action", request.action == DeviceIntentAction::Upsert ? "upsert" : "remove"}};
        (void)decodeDeviceIntent(sink.dump());
        return sink;
    }

    JSON result{{"version", 1U},
                {"operationId", request.operationId},
                {"expectedRevision", request.expectedRevision}};
    if (request.action == DeviceIntentAction::Remove) {
        result["action"] = "remove";
        result["mixId"] = request.mixId;
    } else if (request.action == DeviceIntentAction::Upsert) {
        result["action"] = "upsert";
        result["device"] = {{"id", request.mixId},
                            {"name", request.name},
                            {"sampleRate", request.sampleRate},
                            {"channels", request.channels}};
    } else {
        invalid();
    }
    (void)decodeDeviceIntent(result.dump());
    return result;
}
} // namespace loopz
