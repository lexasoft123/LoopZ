#include <loopz/device_intent_plan.hpp>
#include <loopz/sample_rates.hpp>
#include <cmath>
#include <limits>
#include <map>
#include <stdexcept>

namespace loopz {
AppRoutingSinkDemand appRoutingSinkDemand(const AppRoutingSinkFacts& facts) noexcept {
    if (facts.pendingReceipts || (facts.savedAuthoritative && facts.desiredApplications) ||
        facts.endpointOccupied || facts.defaultEndpoint)
        return AppRoutingSinkDemand::Required;
    return facts.savedAuthoritative && facts.recoveryExamined && facts.endpointsExamined &&
                   facts.defaultsExamined
               ? AppRoutingSinkDemand::Released
               : AppRoutingSinkDemand::Unknown;
}
std::optional<DeviceIntentRequest> nextAppRoutingSinkIntent(AppRoutingSinkDemand demand,
                                                            const DeviceIntentSnapshot& snapshot,
                                                            std::string_view operationId) {
    if (demand == AppRoutingSinkDemand::Unknown)
        return {};
    if (demand != AppRoutingSinkDemand::Required && demand != AppRoutingSinkDemand::Released)
        throw std::invalid_argument("Invalid app routing sink demand");
    if (snapshot.records.size() > 256)
        throw std::invalid_argument("Too many stored setup records");
    const DeviceIntentRequest* sink = nullptr;
    std::map<std::string, bool> mixIds;
    for (const auto& record : snapshot.records) {
        (void)decodeDeviceIntent(encodeDeviceIntent(record).dump());
        if (record.expectedRevision >= snapshot.revision)
            throw std::invalid_argument("Invalid routing sink snapshot revision");
        if (record.kind == DeviceIntentKind::AppRoutingSink) {
            if (sink)
                throw std::invalid_argument("Duplicate routing sink intent");
            sink = &record;
        } else if (!mixIds.emplace(record.mixId, true).second) {
            throw std::invalid_argument("Duplicate stored microphone identity");
        }
    }
    const auto action = demand == AppRoutingSinkDemand::Required ? DeviceIntentAction::Upsert
                                                                 : DeviceIntentAction::Remove;
    if ((!sink && action == DeviceIntentAction::Remove) || (sink && sink->action == action))
        return {};
    if (snapshot.revision == std::numeric_limits<uint64_t>::max())
        throw std::overflow_error("Routing sink setup revision is exhausted");
    DeviceIntentRequest request;
    request.operationId = std::string(operationId);
    request.expectedRevision = snapshot.revision;
    request.kind = DeviceIntentKind::AppRoutingSink;
    request.action = action;
    return decodeDeviceIntent(encodeDeviceIntent(request).dump());
}
std::optional<DeviceIntentRequest> nextSavedDeviceIntent(const RouteDocument& document,
                                                         bool authoritative,
                                                         const DeviceIntentSnapshot& snapshot,
                                                         std::string_view operationId) {
    if (!authoritative)
        return {};
    validateRouteDocument(document);
    std::map<std::string, DeviceIntentRequest> desired;
    for (const auto& device : document.devices) {
        if (!std::isfinite(device.sampleRate) || !supportedVirtualSampleRate(device.sampleRate))
            throw std::invalid_argument("Unsupported desired microphone sample rate");
        DeviceIntentRequest request;
        request.operationId = "00000000-0000-0000-0000-000000000000";
        request.mixId = device.id;
        request.name = device.name;
        request.sampleRate = static_cast<uint32_t>(device.sampleRate);
        request.channels = device.routing.outputChannels;
        // Validate every desired record before choosing a removal. An invalid
        // replacement document must not first remove otherwise healthy devices.
        (void)decodeDeviceIntent(encodeDeviceIntent(request).dump());
        if (!desired.emplace(device.id, std::move(request)).second)
            throw std::invalid_argument("Duplicate desired microphone identity");
    }
    if (snapshot.records.size() > 256)
        throw std::invalid_argument("Too many stored microphone records");
    std::map<std::string, const DeviceIntentRequest*> actual;
    bool sinkSeen = false;
    for (const auto& record : snapshot.records) {
        (void)decodeDeviceIntent(encodeDeviceIntent(record).dump());
        if (record.kind == DeviceIntentKind::AppRoutingSink) {
            if (record.expectedRevision >= snapshot.revision || sinkSeen)
                throw std::invalid_argument("Invalid stored routing sink snapshot");
            sinkSeen = true;
            continue; // Sink lifetime belongs to routing leases, never saved mixes.
        }
        if (record.expectedRevision >= snapshot.revision ||
            !actual.emplace(record.mixId, &record).second)
            throw std::invalid_argument("Invalid stored microphone snapshot");
    }
    const auto finish = [&](DeviceIntentRequest request) -> std::optional<DeviceIntentRequest> {
        if (snapshot.revision == std::numeric_limits<uint64_t>::max())
            throw std::overflow_error("Microphone setup revision is exhausted");
        request.expectedRevision = snapshot.revision;
        request.operationId = std::string(operationId);
        return decodeDeviceIntent(encodeDeviceIntent(request).dump());
    };
    // Free live-record capacity before adding a replacement to a full graph.
    for (const auto& [id, record] : actual)
        if (record->action == DeviceIntentAction::Upsert && !desired.contains(id)) {
            DeviceIntentRequest removal;
            removal.mixId = id;
            removal.action = DeviceIntentAction::Remove;
            return finish(std::move(removal));
        }
    for (const auto& device : document.devices) {
        const auto& wanted = desired.at(device.id);
        const auto found = actual.find(device.id);
        if (found == actual.end() || found->second->action == DeviceIntentAction::Remove ||
            found->second->name != wanted.name || found->second->sampleRate != wanted.sampleRate ||
            found->second->channels != wanted.channels)
            return finish(wanted);
    }
    return {};
}
} // namespace loopz
