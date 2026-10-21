#include <loopz/macos/transport_message.hpp>

#include <array>
#include <cassert>
#include <cstdio>
#include <stdexcept>

namespace {
using namespace loopz;
using namespace loopz::macos;
constexpr uint64_t now = 1000000000;

template <typename Operation> void invalid(Operation operation) {
    bool rejected = false;
    try {
        operation();
    } catch (const std::invalid_argument&) {
        rejected = true;
    }
    assert(rejected);
}

RouteDocument document() {
    VirtualDeviceDefinition studio;
    studio.id = "studio";
    studio.name = "Studio 音";
    studio.routing.outputChannels = 8;
    VirtualDeviceDefinition call;
    call.id = "calls";
    call.name = "Calls";
    call.sampleRate = 44100;
    call.routing.outputChannels = 64;
    call.routing.sources = {{"pass", 64, 1, true, false, defaultChannelConnections(64, 64)}};
    call.bindings = {{"pass", SourceKind::PassThrough, "", false}};
    return {kRouteDocumentVersion, {studio, call}};
}

void mappingRoundTrip() {
    DeviceTransportPool pool;
    const std::array<uint32_t, 2> latencies{129, kMaximumInputLatencyFrames};
    pool.apply(document(), latencies);
    auto message = encodeDeviceTransports(pool.devices(), 17);
    auto remote = DeviceTransportSnapshot::decode(message);
    assert(remote.revision == 17 && remote.devices.size() == 2);
    assert(remote.devices[0].id() == "studio" && remote.devices[0].name() == "Studio 音");
    assert(!remote.devices[0].hasOutput() && remote.devices[1].hasOutput());
    assert(remote.devices[0].uid() == pool.devices()[0].uid);
    assert(remote.devices[0].inputLatencyFrames() == latencies[0] &&
           remote.devices[1].inputLatencyFrames() == latencies[1]);
    assert(remote.devices[0].view().channels() == 8 && remote.devices[1].view().channels() == 64);
    assert(remote.devices[1].view().sampleRate() == 44100);
    assert(pool.devices()[0].region->bytes() % kDeviceAudioMappingAlignment == 0);
    message = nullptr; // Imported mappings retain their own VM lifetime.

    std::array<float, 64> input{}, output{};
    input.fill(0.25f);
    for (size_t index = 0; index < pool.devices().size(); ++index) {
        const auto local = pool.devices()[index].region->view();
        const auto mapped = remote.devices[index].view();
        assert(mapped.mappingIdentity() == local.mappingIdentity());
        local.header().helperOnline.store(1);
        local.header().helperHeartbeatNs.store(now);
        const auto count = local.channels();
        assert(local.writeCapture(123, std::span(input).first(count), 1));
        assert(mapped.readCapture(123, std::span(output).first(count), 1, now).missingFrames == 0);
        for (uint32_t channel = 0; channel < count; ++channel)
            assert(output[channel] == 0.25f);
        mapped.header().driverOnline.store(1);
        mapped.header().driverHeartbeatNs.store(now);
        input.fill(-0.5f);
        assert(mapped.writePlayback(456, std::span(input).first(count), 1));
        assert(local.readPlayback(456, std::span(output).first(count), 1, now).missingFrames == 0);
        for (uint32_t channel = 0; channel < count; ++channel)
            assert(output[channel] == -0.5f);
        input.fill(0.25f);
    }

    // Move assignment retires its old imported mapping and pins the new one.
    auto moved = std::move(remote.devices[0]);
    moved = std::move(remote.devices[1]);
    assert(moved.id() == "calls" && moved.view().channels() == 64);
    assert(moved.inputLatencyFrames() == kMaximumInputLatencyFrames);
    pool.apply({}); // Original owner unmaps; imported memory remains accessible.
    const auto result = moved.view().readCapture(123, output, 1, now);
    assert(result.validBlock && result.missingFrames == 1);
    for (float sample : output)
        assert(sample == 0);
}

void malformedMessages() {
    DeviceTransportPool pool;
    pool.apply(document());
    invalid([&] {
        (void)encodeDeviceTransports(pool.devices(), 0);
    });
    invalid([] {
        (void)DeviceTransportSnapshot::decode(nullptr);
    });
    auto wrongType = xpc_array_create(nullptr, 0);
    invalid([&] {
        (void)DeviceTransportSnapshot::decode(wrongType);
    });
    for (auto latency :
         {xpc_string_create("129"), xpc_int64_create(129),
          xpc_uint64_create(kMaximumInputLatencyFrames + 1ULL), xpc_uint64_create(UINT64_MAX)}) {
        auto malformed = encodeDeviceTransports(pool.devices(), 1);
        auto entry = xpc_array_get_value(xpc_dictionary_get_value(malformed, "devices"), 0);
        xpc_dictionary_set_value(entry, "inputLatencyFrames", latency);
        invalid([&] {
            (void)DeviceTransportSnapshot::decode(malformed);
        });
    }
    auto missingLatency = encodeDeviceTransports(pool.devices(), 1);
    xpc_dictionary_set_value(
        xpc_array_get_value(xpc_dictionary_get_value(missingLatency, "devices"), 0),
        "inputLatencyFrames", nullptr);
    invalid([&] {
        (void)DeviceTransportSnapshot::decode(missingLatency);
    });
    auto outOfRange = pool.devices();
    outOfRange[0].inputLatencyFrames = kMaximumInputLatencyFrames + 1;
    invalid([&] {
        (void)encodeDeviceTransports(outOfRange, 1);
    });
    auto message = encodeDeviceTransports(pool.devices(), 1);
    xpc_dictionary_set_uint64(message, "protocol", 1);
    invalid([&] {
        (void)DeviceTransportSnapshot::decode(message);
    });
    xpc_dictionary_set_uint64(message, "protocol", kDeviceTransportProtocol);
    xpc_dictionary_set_string(message, "revision", "1");
    invalid([&] {
        (void)DeviceTransportSnapshot::decode(message);
    });
    xpc_dictionary_set_uint64(message, "revision", 1);
    xpc_dictionary_set_bool(message, "unknown", true);
    invalid([&] {
        (void)DeviceTransportSnapshot::decode(message);
    });

    message = encodeDeviceTransports(pool.devices(), 1);
    auto array = xpc_dictionary_get_value(message, "devices");
    auto first = xpc_array_get_value(array, 0);
    xpc_dictionary_set_uint64(first, "hasOutput", 1);
    invalid([&] {
        (void)DeviceTransportSnapshot::decode(message);
    });
    xpc_dictionary_set_bool(first, "hasOutput", false);
    xpc_dictionary_set_string(first, "uid", "another-device");
    invalid([&] {
        (void)DeviceTransportSnapshot::decode(message);
    });

    message = encodeDeviceTransports(pool.devices(), 1);
    array = xpc_dictionary_get_value(message, "devices");
    first = xpc_array_get_value(array, 0);
    xpc_array_append_value(array, first);
    invalid([&] {
        (void)DeviceTransportSnapshot::decode(message);
    });
    for (uint32_t index = 0; index < kMaximumVirtualDevices; ++index)
        xpc_array_append_value(array, first);
    invalid([&] {
        (void)DeviceTransportSnapshot::decode(message);
    });

    message = encodeDeviceTransports(pool.devices(), 1);
    array = xpc_dictionary_get_value(message, "devices");
    first = xpc_array_get_value(array, 0);
    xpc_dictionary_set_value(first, "audio", wrongType);
    invalid([&] {
        (void)DeviceTransportSnapshot::decode(message);
    });

    auto aliased = pool.devices();
    aliased[1].region = aliased[0].region;
    invalid([&] {
        (void)encodeDeviceTransports(aliased, 1);
    });
    message = encodeDeviceTransports(pool.devices(), 1);
    array = xpc_dictionary_get_value(message, "devices");
    first = xpc_array_get_value(array, 0);
    const auto second = xpc_array_get_value(array, 1);
    xpc_dictionary_set_value(second, "audio", xpc_dictionary_get_value(first, "audio"));
    invalid([&] {
        (void)DeviceTransportSnapshot::decode(message);
    });

    // A valid XPC memory object still needs the exact shared-memory schema.
    auto region = pool.devices()[0].region;
    message = encodeDeviceTransports(pool.devices(), 1);
    region->header().protocol = 999;
    invalid([&] {
        (void)DeviceTransportSnapshot::decode(message);
    });
    region->header().protocol = kDeviceAudioProtocol;
    assert(DeviceTransportSnapshot::decode(message).devices.size() == 2);
    assert(DeviceTransportSnapshot::decode(encodeDeviceTransports({}, 2)).devices.empty());
}
} // namespace

int main() {
    @autoreleasepool {
        mappingRoundTrip();
        malformedMessages();
        std::puts(
            "transport messages: actual XPC memory aliases, metadata, retained mapping lifetime "
            "and malformed snapshot rejection passed (local codec; no installed connection)");
    }
}
