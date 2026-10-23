#include "../native/platform/windows/driver/transport_negotiation.hpp"
#include <cassert>
#include <vector>
using namespace loopz_driver;
TransportCapabilities caps(unsigned version) {
    TransportCapabilities result;
    result.version = version;
    result.bitsPerSample = version == 3 ? 32 : 16;
    result.maxPacketFrames = version == 3 ? 240 : 480;
    return result;
}
int main() {
    TransportConfiguration output;
    std::vector<unsigned> versions;
    auto result = negotiateTransport(
        [&](unsigned version, auto& reply) {
            versions.push_back(version);
            reply = caps(version);
            return CapabilityQueryResult::Supported;
        },
        output);
    assert(result == TransportNegotiationResult::Ready && versions == std::vector<unsigned>{3});
    assert(output.version == 3 && output.sampleBytes == 4 && output.packetFrames == 240 &&
           output.frameBytes() == 8);
    versions.clear();
    result = negotiateTransport(
        [&](unsigned version, auto& reply) {
            versions.push_back(version);
            if (version == 3)
                return CapabilityQueryResult::Unsupported;
            reply = caps(version);
            return CapabilityQueryResult::Supported;
        },
        output);
    assert(result == TransportNegotiationResult::Ready &&
           (versions == std::vector<unsigned>{3, 2}));
    assert(output.version == 2 && output.sampleBytes == 2 && output.frameBytes() == 4);
    versions.clear();
    result = negotiateTransport(
        [&](unsigned version, auto& reply) {
            versions.push_back(version);
            reply = caps(version);
            return CapabilityQueryResult::Supported;
        },
        output, false);
    assert(result == TransportNegotiationResult::Ready && versions == std::vector<unsigned>{2});
    for (auto error : {CapabilityQueryResult::Failed, CapabilityQueryResult::Unsupported}) {
        versions.clear();
        result = negotiateTransport(
            [&](unsigned version, auto&) {
                versions.push_back(version);
                return error;
            },
            output);
        assert(result != TransportNegotiationResult::Ready && output.version == 0);
        assert(versions.size() == (error == CapabilityQueryResult::Failed ? 1U : 2U));
    }
    // A successful but malformed v3 reply must not trigger a legacy retry.
    for (unsigned damage = 0; damage < 7; ++damage) {
        unsigned calls = 0;
        result = negotiateTransport(
            [&](unsigned version, auto& reply) {
                ++calls;
                reply = caps(version);
                switch (damage) {
                case 0:
                    reply.version = 2;
                    break;
                case 1:
                    reply.bitsPerSample = 16;
                    break;
                case 2:
                    reply.maxPacketFrames = 480;
                    break;
                case 3:
                    reply.sampleRate = 12345;
                    break;
                case 4:
                    reply.channels = 65;
                    break;
                case 5:
                    reply.reserved = 1;
                    break;
                case 6:
                    reply.leaseDuration += 1;
                    break;
                }
                return CapabilityQueryResult::Supported;
            },
            output);
        assert(result == TransportNegotiationResult::InvalidCapabilities && calls == 1 &&
               output.version == 0);
    }
    for (unsigned version : {2U, 3U})
        for (unsigned rate : {44100U, 48000U, 88200U, 96000U, 176400U, 192000U})
            for (unsigned channels = 2; channels <= 64; channels += 2) {
                auto valid = caps(version);
                valid.sampleRate = rate;
                valid.channels = channels;
                assert(validTransportCapabilities(valid, version));
                assert(!validTransportCapabilities(valid, 99));
            }
}
