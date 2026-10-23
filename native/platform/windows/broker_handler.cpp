#include "broker_handler.hpp"
#include <array>
#include <bcrypt.h>
#include <set>
#include <stdexcept>
#include <algorithm>

namespace loopz::windows {
namespace {
struct StatusQuery {
    bool status{};
    std::optional<std::string> mixId;
    bool snapshot{};
    std::optional<uint64_t> revision;
    size_t offset{};
    std::string tag;
    DeviceIntentKind kind{DeviceIntentKind::MicrophoneMix};
};
StatusQuery statusQuery(std::string_view bytes) {
    if (bytes.empty() || bytes.size() > kMaximumDeviceIntentBytes)
        throw std::invalid_argument("Invalid microphone setup request");
    std::array<std::set<std::string>, 5> keys;
    const auto value = JSON::parse(bytes, [&](int depth, JSON::parse_event_t event, JSON& parsed) {
        if (depth < 0 || depth > 3)
            throw std::invalid_argument("Setup request is nested too deeply");
        if (event == JSON::parse_event_t::object_start)
            keys[static_cast<size_t>(depth) + 1].clear();
        else if (event == JSON::parse_event_t::key &&
                 !keys[static_cast<size_t>(depth)].insert(parsed.get<std::string>()).second)
            throw std::invalid_argument("Duplicate setup request field");
        return true;
    });
    if (!value.is_object() || !value.contains("action"))
        return {};
    if (value.at("action") == "snapshot") {
        if (!value.contains("version") || !value.at("version").is_number_integer() ||
            value.at("version") != 1 || (value.size() != 2 && value.size() != 5))
            throw std::invalid_argument("Invalid setup snapshot query");
        StatusQuery result;
        result.snapshot = true;
        if (value.size() == 5) {
            if (!value.contains("offset") || !value.at("offset").is_number_unsigned() ||
                value.at("offset").get<uint64_t>() > 256 || !value.contains("revision") ||
                !value.at("revision").is_number_unsigned() || !value.contains("tag") ||
                !value.at("tag").is_string())
                throw std::invalid_argument("Invalid setup snapshot cursor");
            result.tag = value.at("tag").get<std::string>();
            if (result.tag.size() != 64 ||
                result.tag.find_first_not_of("0123456789abcdef") != std::string::npos)
                throw std::invalid_argument("Invalid setup snapshot tag");
            result.offset = value.at("offset").get<size_t>();
            result.revision = value.at("revision").get<uint64_t>();
        }
        return result;
    }
    if (value.at("action") != "status")
        return {};
    if (value.contains("version") && value.at("version").is_number_unsigned() &&
        value.at("version") == 2U) {
        if (value.size() != 3 || !value.contains("kind") || value.at("kind") != "appRoutingSink")
            throw std::invalid_argument("Invalid routing sink status query");
        StatusQuery result;
        result.status = true;
        result.kind = DeviceIntentKind::AppRoutingSink;
        result.mixId = std::string{};
        return result;
    }
    if ((value.size() != 2 && value.size() != 3) || !value.contains("version") ||
        !value.at("version").is_number_integer() || value.at("version") != 1)
        throw std::invalid_argument("Invalid setup status query");
    StatusQuery result;
    result.status = true;
    if (value.size() == 3) {
        if (!value.contains("mixId") || !value.at("mixId").is_string())
            throw std::invalid_argument("Invalid setup status mix identity");
        auto mix = value.at("mixId").get<std::string>();
        if (mix.empty() || mix.size() > 4096 || mix.find('\0') != std::string::npos)
            throw std::invalid_argument("Invalid setup status mix identity");
        result.mixId = std::move(mix);
    }
    return result;
}
std::string snapshotTag(const DeviceIntentSnapshot& snapshot) {
    JSON records = JSON::array();
    for (const auto& record : snapshot.records)
        records.push_back(encodeDeviceIntent(record));
    auto bytes = JSON{{"revision", snapshot.revision}, {"records", std::move(records)}}.dump();
    BCRYPT_ALG_HANDLE algorithm{};
    if (BCryptOpenAlgorithmProvider(&algorithm, BCRYPT_SHA256_ALGORITHM, nullptr, 0) < 0)
        throw std::runtime_error("Cannot initialize setup snapshot tag");
    struct Algorithm {
        BCRYPT_ALG_HANDLE value;
        ~Algorithm() {
            BCryptCloseAlgorithmProvider(value, 0);
        }
    } close{algorithm};
    std::array<unsigned char, 32> hash{};
    if (BCryptHash(algorithm, nullptr, 0, reinterpret_cast<PUCHAR>(bytes.data()),
                   static_cast<ULONG>(bytes.size()), hash.data(),
                   static_cast<ULONG>(hash.size())) < 0)
        throw std::runtime_error("Cannot compute setup snapshot tag");
    constexpr char hex[] = "0123456789abcdef";
    std::string result;
    for (const auto byte : hash) {
        result += hex[byte >> 4];
        result += hex[byte & 15];
    }
    return result;
}
const char* phaseName(BrokerDevicePhase phase) {
    switch (phase) {
    case BrokerDevicePhase::Applying:
        return "applying";
    case BrokerDevicePhase::Ready:
        return "ready";
    case BrokerDevicePhase::Removed:
        return "removed";
    case BrokerDevicePhase::Queued:
        return "queued";
    case BrokerDevicePhase::RebootRequired:
        return "reboot_required";
    case BrokerDevicePhase::Error:
        return "error";
    }
    return "queued";
}
} // namespace
BrokerCommandResult handleBrokerMessage(DeviceIntentStore& store, const BrokerMessage& message,
                                        const BrokerProgressQuery& progress) {
    try {
        const auto query = statusQuery(message.bytes);
        if (query.snapshot) {
            const auto snapshot = store.snapshot(message.owner);
            const auto tag = snapshotTag(snapshot);
            if (query.revision && (*query.revision != snapshot.revision || query.tag != tag))
                return {{{"ok", false},
                         {"code", "snapshot_changed"},
                         {"error", "Microphone setup changed while reading its snapshot"}},
                        false};
            if (query.offset > snapshot.records.size())
                throw std::invalid_argument("Setup snapshot cursor exceeds record count");
            JSON records = JSON::array();
            size_t size = 512; // Bounded envelope plus commas; IDs are not repeated as cursors.
            auto next = query.offset;
            while (next < snapshot.records.size()) {
                auto record = encodeDeviceIntent(snapshot.records[next]);
                const auto bytes = record.dump().size() + 1;
                if (size + bytes > 60000) {
                    if (records.empty())
                        throw std::runtime_error("Setup snapshot record exceeds reply limit");
                    break;
                }
                size += bytes;
                records.push_back(std::move(record));
                ++next;
            }
            JSON reply{{"ok", true},
                       {"revision", snapshot.revision},
                       {"tag", tag},
                       {"records", std::move(records)},
                       {"nextOffset", next < snapshot.records.size() ? JSON(next) : JSON(nullptr)}};
            return {std::move(reply), false};
        }
        if (query.status) {
            const auto snapshot = store.snapshot(message.owner);
            JSON reply{{"ok", true},
                       {"revision", snapshot.revision},
                       {"desiredRecordCount", snapshot.records.size()}};
            if (query.mixId) {
                JSON device{{"phase", "unrequested"}};
                if (query.kind == DeviceIntentKind::MicrophoneMix)
                    device["id"] = *query.mixId;
                else
                    device["kind"] = "appRoutingSink";
                const auto found = std::find_if(
                    snapshot.records.begin(), snapshot.records.end(), [&](const auto& record) {
                        return record.kind == query.kind && record.mixId == *query.mixId;
                    });
                if (found != snapshot.records.end()) {
                    const auto revision = found->expectedRevision + 1;
                    device["revision"] = revision;
                    device["phase"] = "queued";
                    if (progress) {
                        const auto applied = progress(message.owner, *query.mixId, query.kind);
                        if (applied && applied->owner == message.owner &&
                            applied->mixId == *query.mixId && applied->kind == query.kind &&
                            applied->revision == revision) {
                            device["phase"] = phaseName(applied->phase);
                            if (!applied->error.empty())
                                device["error"] = applied->error;
                        }
                    }
                }
                reply["device"] = std::move(device);
            }
            return {std::move(reply), false};
        }
        const auto request = decodeDeviceIntent(message.bytes);
        const auto accepted = store.accept(message.owner, request);
        return {{{"ok", true}, {"acceptedRevision", accepted}}, true};
    } catch (const std::exception& error) {
        return {{{"ok", false}, {"code", "request_failed"}, {"error", error.what()}}, false};
    }
}
void runBrokerRequests(BrokerPipe& pipe, DeviceIntentStore& store, HANDLE reconcileEvent,
                       const BrokerProgressQuery& progress) {
    if (!reconcileEvent || reconcileEvent == INVALID_HANDLE_VALUE)
        throw std::invalid_argument("Setup requests require a reconciliation event");
    for (;;) {
        BrokerMessage message;
        const auto received = pipe.receive(message);
        if (received == BrokerReceive::Stopped)
            return;
        if (received == BrokerReceive::Rejected)
            continue;
        const auto result = handleBrokerMessage(store, message, progress);
        if (result.reconcileRequested && !SetEvent(reconcileEvent))
            throw std::runtime_error("Cannot wake setup reconciliation after durable acceptance");
        (void)pipe.reply(result.reply.dump(-1, ' ', false, JSON::error_handler_t::replace));
    }
}
} // namespace loopz::windows
