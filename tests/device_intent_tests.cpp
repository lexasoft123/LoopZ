#include <loopz/device_intent.hpp>
#include <loopz/sample_rates.hpp>
#include <bit>
#include <cassert>
#include <iostream>
#include <limits>

using namespace loopz;
template <class F> void rejects(F&& action) {
    bool rejected = false;
    try {
        action();
    } catch (const std::exception&) {
        rejected = true;
    }
    assert(rejected);
}
int main() {
    DeviceIntentRequest request;
    request.operationId = "12345678-1234-1234-1234-123456789abc";
    request.mixId = "mix";
    request.name = "Русский 混音";
    for (const auto rate : kVirtualSampleRates)
        for (uint32_t channels = 1; channels <= 64; ++channels) {
            request.sampleRate = rate;
            request.channels = channels;
            const auto decoded = decodeDeviceIntent(encodeDeviceIntent(request).dump());
            assert(decoded.mixId == request.mixId && decoded.name == request.name);
            assert(decoded.operationId == request.operationId && decoded.expectedRevision == 0);
            assert(decoded.sampleRate == rate && decoded.channels == channels);
        }
    request.expectedRevision = (uint64_t(1) << 60) + 1;
    auto valid = encodeDeviceIntent(request);
    assert(decodeDeviceIntent(valid.dump()).expectedRevision == request.expectedRevision);
    for (const auto* key : {"ownerSid", "sid", "path", "driverPath", "command", "reconcile"}) {
        auto bad = valid;
        bad[key] = "untrusted";
        rejects([&] {
            (void)decodeDeviceIntent(bad.dump());
        });
        bad = valid;
        bad["device"][key] = "untrusted";
        rejects([&] {
            (void)decodeDeviceIntent(bad.dump());
        });
    }
    for (const auto* key : {"version", "operationId", "expectedRevision", "action", "device"}) {
        auto bad = valid;
        bad.erase(key);
        rejects([&] {
            (void)decodeDeviceIntent(bad.dump());
        });
    }
    for (const auto& value : {JSON(-1), JSON(1.0), JSON(true), JSON("1"), JSON(nullptr),
                              JSON(std::numeric_limits<uint64_t>::max())}) {
        auto bad = valid;
        bad["expectedRevision"] = value;
        rejects([&] {
            (void)decodeDeviceIntent(bad.dump());
        });
    }
    for (const auto* key : {"channels", "sampleRate"})
        for (const auto& value : {JSON(-1), JSON(2.0), JSON(true), JSON(uint64_t(1) << 40)}) {
            auto bad = valid;
            bad["device"][key] = value;
            rejects([&] {
                (void)decodeDeviceIntent(bad.dump());
            });
        }
    for (const auto channels : {0U, 65U}) {
        auto bad = valid;
        bad["device"]["channels"] = channels;
        rejects([&] {
            (void)decodeDeviceIntent(bad.dump());
        });
    }
    for (const auto rate : {0U, 8000U, 44099U, 192001U}) {
        auto bad = valid;
        bad["device"]["sampleRate"] = rate;
        rejects([&] {
            (void)decodeDeviceIntent(bad.dump());
        });
    }
    for (const auto* key : {"action", "version"}) {
        auto bad = valid;
        bad[key] = key == std::string_view("action") ? JSON("removeAll") : JSON(2U);
        rejects([&] {
            (void)decodeDeviceIntent(bad.dump());
        });
    }
    for (const auto* key : {"id", "name"})
        for (const auto& value :
             {JSON(""), JSON(std::string(4097, 'x')), JSON(std::string("a\0b", 3))}) {
            auto bad = valid;
            bad["device"][key] = value;
            rejects([&] {
                (void)decodeDeviceIntent(bad.dump());
            });
        }
    for (const auto* id : {"", "../file", "12345678-1234-1234-1234-123456789abC"}) {
        auto bad = valid;
        bad["operationId"] = id;
        rejects([&] {
            (void)decodeDeviceIntent(bad.dump());
        });
    }
    auto bytes = valid.dump();
    bytes.insert(1, "\"action\":\"remove\",");
    rejects([&] {
        (void)decodeDeviceIntent(bytes);
    });
    bytes = valid.dump();
    bytes.insert(bytes.find("\"device\":{") + 10, "\"name\":\"duplicate\",");
    rejects([&] {
        (void)decodeDeviceIntent(bytes);
    });
    rejects([&] {
        (void)decodeDeviceIntent(std::string(kMaximumDeviceIntentBytes + 1, ' '));
    });
    rejects([&] {
        (void)decodeDeviceIntent(std::string(40, '[') + std::string(40, ']'));
    });
    bytes = valid.dump();
    bytes.insert(bytes.find("mix"), 1, std::bit_cast<char>(uint8_t{0xff}));
    rejects([&] {
        (void)decodeDeviceIntent(bytes);
    });
    request.action = DeviceIntentAction::Remove;
    auto removal = encodeDeviceIntent(request);
    assert(removal.size() == 5 && removal.contains("mixId") && !removal.contains("device"));
    auto decoded = decodeDeviceIntent(removal.dump());
    assert(decoded.action == DeviceIntentAction::Remove && decoded.mixId == request.mixId);
    removal["device"] = valid["device"];
    rejects([&] {
        (void)decodeDeviceIntent(removal.dump());
    });
    request.action = DeviceIntentAction::Upsert;
    request.mixId.assign(4096, '\x01');
    request.name.assign(4096, '\x02');
    assert(decodeDeviceIntent(encodeDeviceIntent(request).dump()).name == request.name);
    DeviceIntentRequest sink;
    sink.operationId = request.operationId;
    sink.kind = DeviceIntentKind::AppRoutingSink;
    for (auto action : {DeviceIntentAction::Upsert, DeviceIntentAction::Remove}) {
        sink.action = action;
        const auto encoded = encodeDeviceIntent(sink);
        assert(encoded.at("version") == 2U && encoded.size() == 5);
        const auto decodedSink = decodeDeviceIntent(encoded.dump());
        assert(decodedSink.kind == DeviceIntentKind::AppRoutingSink && decodedSink.mixId.empty() &&
               decodedSink.name.empty() && decodedSink.sampleRate == 48000 &&
               decodedSink.channels == 2 && decodedSink.action == action);
        for (auto field : {"ownerSid", "mixId", "device", "sampleRate", "channels", "name"}) {
            auto bad = encoded;
            bad[field] = "forged";
            rejects([&] {
                (void)decodeDeviceIntent(bad.dump());
            });
        }
    }
    assert(deviceIntentStorageKey(sink.kind, {}).front() == '\0');
    assert(deviceIntentStorageKey(DeviceIntentKind::MicrophoneMix, "LoopZ.AppRoutingSink.v1") !=
           deviceIntentStorageKey(sink.kind, {}));
    sink.mixId = "forged";
    rejects([&] {
        (void)encodeDeviceIntent(sink);
    });
    sink.mixId.clear();
    sink.channels = 64;
    rejects([&] {
        (void)encodeDeviceIntent(sink);
    });
    std::cout << "PASS: device intent formats, Unicode, exact revisions, explicit removal, "
                 "forged fields, duplicate keys and bounded malformed requests\n";
}
