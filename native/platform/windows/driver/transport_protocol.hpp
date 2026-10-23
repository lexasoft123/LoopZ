// LoopZ contributors. MIT license. Shared kernel/user wire layout; no pointers.
#pragma once
#include "duplex_bridge.hpp"

namespace loopz_driver {
constexpr unsigned transportVersion = 2;
enum class TransportProperty : unsigned {
    Capabilities = 1,
    Connect,
    Disconnect,
    Playback,
    Processed,
    Diagnostics
};
struct TransportInstance {
    unsigned version = transportVersion;
    unsigned reserved = 0;
    unsigned long long generation = 0;
};
struct TransportCapabilities {
    unsigned version = transportVersion;
    unsigned sampleRate = 48000;
    unsigned channels = 2;
    unsigned bitsPerSample = 16;
    unsigned maxPacketFrames = DuplexBridge::packetFrames;
    unsigned reserved = 0;
    unsigned long long leaseDuration = DuplexBridge::leaseDuration;
};
struct TransportConnection {
    unsigned long long generation = 0;
};
struct TransportProcessed {
    unsigned frames = 0;
    unsigned reserved = 0;
    unsigned char samples[DuplexBridge::packetFrames * AudioFormat::maxChannels *
                          AudioFormat::bytesPerSample]{};
};
// Additive float contract. No existing driver/client selects this until
// explicit version negotiation is implemented; legacy v2 defaults stay intact.
constexpr unsigned floatTransportVersion = 3;
struct FloatTransportInstance {
    unsigned version = floatTransportVersion;
    unsigned reserved = 0;
    unsigned long long generation = 0;
};
struct FloatTransportCapabilities {
    unsigned version = floatTransportVersion;
    unsigned sampleRate = 48000;
    unsigned channels = 2;
    unsigned bitsPerSample = 32;
    unsigned maxPacketFrames = FloatDuplexBridge::packetFrames;
    unsigned reserved = 0;
    unsigned long long leaseDuration = FloatDuplexBridge::leaseDuration;
};
struct FloatTransportProcessed {
    unsigned frames = 0;
    unsigned reserved = 0;
    unsigned char samples[FloatDuplexBridge::packetFrames * AudioFormat::maxChannels * 4]{};
};
static_assert(sizeof(FloatTransportInstance) == sizeof(TransportInstance));
static_assert(sizeof(FloatTransportCapabilities) == sizeof(TransportCapabilities));
static_assert(sizeof(FloatTransportProcessed) == sizeof(TransportProcessed));
static_assert(sizeof(FloatDuplexBridge::Packet) == sizeof(DuplexBridge::Packet));
static_assert(sizeof(FloatTransportProcessed) < 65536);
static_assert(sizeof(unsigned) == 4 && sizeof(unsigned long long) == 8);
static_assert(sizeof(TransportInstance) == 16);
static_assert(sizeof(TransportCapabilities) == 32);
static_assert(sizeof(TransportConnection) == 8);
static_assert(sizeof(TransportProcessed) == 61448);
static_assert(sizeof(DuplexBridge::Packet) == 61464);
static_assert(sizeof(DuplexBridge::Diagnostics) == 72);
} // namespace loopz_driver
