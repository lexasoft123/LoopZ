#pragma once
#include "transport_protocol.hpp"
namespace loopz_driver {
enum class CapabilityQueryResult { Supported, Unsupported, Failed };
enum class TransportNegotiationResult { Ready, Unsupported, InvalidCapabilities, QueryFailed };
struct TransportConfiguration {
    unsigned version{}, sampleBytes{}, packetFrames{};
    AudioFormat format{};
    unsigned frameBytes() const noexcept {
        return format.channels * sampleBytes;
    }
};
inline bool validTransportCapabilities(const TransportCapabilities& caps,
                                       unsigned expected) noexcept {
    const bool floating = expected == floatTransportVersion;
    if (!floating && expected != transportVersion)
        return false;
    return caps.version == expected && AudioFormat{caps.sampleRate, caps.channels}.valid() &&
           caps.bitsPerSample == (floating ? 32U : 16U) &&
           caps.maxPacketFrames ==
               (floating ? FloatDuplexBridge::packetFrames : DuplexBridge::packetFrames) &&
           !caps.reserved && caps.leaseDuration == DuplexBridge::leaseDuration;
}
// Query maps only an explicit unsupported-version/property result to Unsupported.
// Malformed success, access denial and other failures never authorize downgrade.
// Query receives (requested version, output caps); it performs no Connect request.
template <class Query>
TransportNegotiationResult negotiateTransport(Query&& query, TransportConfiguration& output,
                                              bool preferFloat = true) {
    output = {};
    TransportCapabilities caps{};
    unsigned version = preferFloat ? floatTransportVersion : transportVersion;
    auto result = query(version, caps);
    if (preferFloat && result == CapabilityQueryResult::Unsupported) {
        version = transportVersion;
        caps = {};
        result = query(version, caps);
    }
    if (result == CapabilityQueryResult::Unsupported)
        return TransportNegotiationResult::Unsupported;
    if (result != CapabilityQueryResult::Supported)
        return TransportNegotiationResult::QueryFailed;
    if (!validTransportCapabilities(caps, version))
        return TransportNegotiationResult::InvalidCapabilities;
    output = {
        version, caps.bitsPerSample / 8, caps.maxPacketFrames, {caps.sampleRate, caps.channels}};
    return TransportNegotiationResult::Ready;
}
} // namespace loopz_driver
