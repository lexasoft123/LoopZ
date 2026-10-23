#include "device_operations.hpp"
#include "driver/adapter_configuration.hpp"
#include <objbase.h>
#include <array>
#include <stdexcept>
#include <iostream>
#include <source_location>

namespace {
void require(bool value, std::source_location where = std::source_location::current()) {
    if (!value)
        throw std::runtime_error("Device operation regression failed at line " +
                                 std::to_string(where.line()));
}
struct Fixture {
    std::wstring path;
    HKEY key{};
    Fixture() {
        GUID guid{};
        require(SUCCEEDED(CoCreateGuid(&guid)));
        std::array<wchar_t, 40> text{};
        require(StringFromGUID2(guid, text.data(), static_cast<int>(text.size())) != 0);
        path = L"Software\\LoopZ\\Tests\\DeviceOperations\\" + std::wstring(text.data());
        DWORD disposition{};
        require(RegCreateKeyExW(HKEY_CURRENT_USER, path.c_str(), 0, nullptr, 0, KEY_ALL_ACCESS,
                                nullptr, &key, &disposition) == ERROR_SUCCESS);
        if (disposition != REG_CREATED_NEW_KEY) {
            RegCloseKey(key);
            key = nullptr;
            path.clear();
            throw std::runtime_error("Device test registry key already exists");
        }
    }
    ~Fixture() {
        if (key)
            RegCloseKey(key);
        if (!path.empty())
            RegDeleteTreeW(HKEY_CURRENT_USER, path.c_str());
    }
};
DWORD pending(HKEY key) {
    DWORD value{}, size = sizeof(value), type{};
    require(RegQueryValueExW(key, L"LoopZFormatPending", nullptr, &type,
                             reinterpret_cast<BYTE*>(&value), &size) == ERROR_SUCCESS);
    require(type == REG_DWORD && size == sizeof(value));
    return value;
}
void acknowledge(HKEY key) {
    const DWORD zero{};
    require(RegSetValueExW(key, L"LoopZFormatPending", 0, REG_DWORD,
                           reinterpret_cast<const BYTE*>(&zero), sizeof(zero)) == ERROR_SUCCESS);
}
template <class Exception = std::invalid_argument, class Operation>
void rejects(Operation operation, std::source_location where = std::source_location::current()) {
    bool rejected = false;
    try {
        operation();
    } catch (const Exception&) {
        rejected = true;
    }
    require(rejected, where);
}
} // namespace
int main(int argc, char** argv) try {
    using namespace loopz::windows;
    if (argc == 3 && std::string_view(argv[1]) == "--reject-sink-package") {
        const std::string package(argv[2]);
        try {
            upsertAppRoutingSink(INVALID_HANDLE_VALUE, L"S-1-5-18",
                                 std::wstring(package.begin(), package.end()));
        } catch (const std::runtime_error& error) {
            if (std::string_view(error.what()) !=
                "App routing sink requires the supported render-only driver package")
                throw;
            std::cout
                << "PASS: unsupported installed sink package rejected before device registration\n";
            return 0;
        }
        throw std::runtime_error("Unsupported sink package unexpectedly accepted");
    }
    require(argc == 1);
    Fixture fixture;
    const loopz_driver::AudioFormat desired{96000, 8};
    require(recordDeviceFormat(fixture.key, desired));
    require(pending(fixture.key) == 1);
    require(recordDeviceFormat(fixture.key, desired)); // Pending work must retry.
    acknowledge(fixture.key);
    require(!recordDeviceFormat(fixture.key, desired));
    loopz_driver::AudioFormatRecord record{};
    DWORD size = sizeof(record), type{};
    require(RegQueryValueExW(fixture.key, L"LoopZAudioFormat", nullptr, &type,
                             reinterpret_cast<BYTE*>(&record), &size) == ERROR_SUCCESS);
    require(type == REG_BINARY && size == sizeof(record) && record.valid());
    require(record.format.sampleRate == 96000 && record.format.channels == 8);
    rejects([&] {
        recordDeviceFormat(fixture.key, {48000, 3});
    });
    require(!recordDeviceFormat(fixture.key, desired));
    {
        acknowledge(fixture.key);
        const auto sink = loopz_driver::appRoutingSinkRecord();
        require(RegSetValueExW(fixture.key, L"LoopZAudioFormat", 0, REG_BINARY,
                               reinterpret_cast<const BYTE*>(&sink),
                               sizeof(sink)) == ERROR_SUCCESS);
        rejects<std::runtime_error>([&] {
            recordDeviceFormat(fixture.key, desired);
        });
        loopz_driver::AudioFormatRecord unchanged{};
        size = sizeof(unchanged);
        require(RegQueryValueExW(fixture.key, L"LoopZAudioFormat", nullptr, &type,
                                 reinterpret_cast<BYTE*>(&unchanged), &size) == ERROR_SUCCESS);
        require(unchanged.version == sink.version && unchanged.reserved == sink.reserved &&
                unchanged.format.sampleRate == sink.format.sampleRate &&
                unchanged.format.channels == sink.format.channels && pending(fixture.key) == 0);
        const std::array<loopz_driver::AudioFormatRecord, 2> future{{{3, 0, {48000, 2}}, {}}};
        require(RegSetValueExW(fixture.key, L"LoopZAudioFormat", 0, REG_BINARY,
                               reinterpret_cast<const BYTE*>(future.data()),
                               sizeof(future)) == ERROR_SUCCESS);
        rejects<std::runtime_error>([&] {
            recordDeviceFormat(fixture.key, desired);
        });
        require(pending(fixture.key) == 0);
        const loopz_driver::AudioFormatRecord legacy{1, 0, desired};
        require(RegSetValueExW(fixture.key, L"LoopZAudioFormat", 0, REG_BINARY,
                               reinterpret_cast<const BYTE*>(&legacy),
                               sizeof(legacy)) == ERROR_SUCCESS);
        HKEY writeOnly{};
        require(RegOpenKeyExW(HKEY_CURRENT_USER, fixture.path.c_str(), 0, KEY_SET_VALUE,
                              &writeOnly) == ERROR_SUCCESS);
        rejects<std::runtime_error>([&] {
            recordDeviceFormat(writeOnly, {48000, 2});
        });
        RegCloseKey(writeOnly);
        require(pending(fixture.key) == 0);
    }
    const DWORD malformed = 7;
    require(RegSetValueExW(fixture.key, L"LoopZAudioFormat", 0, REG_DWORD,
                           reinterpret_cast<const BYTE*>(&malformed),
                           sizeof(malformed)) == ERROR_SUCCESS);
    require(recordDeviceFormat(fixture.key, desired));
    acknowledge(fixture.key);
    const wchar_t invalidPending[] = L"invalid";
    require(RegSetValueExW(fixture.key, L"LoopZFormatPending", 0, REG_SZ,
                           reinterpret_cast<const BYTE*>(invalidPending),
                           sizeof(invalidPending)) == ERROR_SUCCESS);
    require(recordDeviceFormat(fixture.key, desired));

    require(!deviceOwnerMatches(fixture.key, L"S-1-5-18"));
    alignas(DWORD) std::array<BYTE, SECURITY_MAX_SID_SIZE> sid{};
    DWORD sidSize = static_cast<DWORD>(sid.size());
    require(CreateWellKnownSid(WinLocalSystemSid, nullptr, sid.data(), &sidSize) != FALSE);
    require(RegSetValueExW(fixture.key, L"LoopZOwnerSid", 0, REG_BINARY, sid.data(), sidSize) ==
            ERROR_SUCCESS);
    require(deviceOwnerMatches(fixture.key, L"S-1-5-18"));
    require(!deviceOwnerMatches(fixture.key, L"S-1-5-19"));
    require(RegSetValueExW(fixture.key, L"LoopZOwnerSid", 0, REG_BINARY, sid.data(), sidSize - 1) ==
            ERROR_SUCCESS);
    require(!deviceOwnerMatches(fixture.key, L"S-1-5-18"));
    require(RegSetValueExW(fixture.key, L"LoopZOwnerSid", 0, REG_SZ, sid.data(), sidSize) ==
            ERROR_SUCCESS);
    require(!deviceOwnerMatches(fixture.key, L"S-1-5-18"));
    rejects([&] {
        deviceOwnerMatches(fixture.key, L"SY");
    });
    rejects([&] {
        removeMixDevice(L"S-1-5-18", "");
    });
    rejects([&] {
        removeMixDevice(L"not-a-sid", "test-mix");
    });

    {
        Fixture sink;
        rejects<std::runtime_error>([&] {
            recordAppRoutingSinkConfiguration(sink.key, false);
        });
        require(recordAppRoutingSinkConfiguration(sink.key, true));
        require(pending(sink.key) == 1);
        require(recordAppRoutingSinkConfiguration(sink.key, false));
        acknowledge(sink.key);
        require(!recordAppRoutingSinkConfiguration(sink.key, false));
        loopz_driver::AudioFormatRecord configuration{};
        DWORD kind{}, bytes = sizeof(configuration);
        require(RegQueryValueExW(sink.key, L"LoopZAudioFormat", nullptr, &kind,
                                 reinterpret_cast<BYTE*>(&configuration), &bytes) == ERROR_SUCCESS);
        loopz_driver::AdapterConfiguration decoded;
        require(kind == REG_BINARY && bytes == sizeof(configuration) &&
                loopz_driver::decodeAdapterConfiguration(configuration, decoded) &&
                !decoded.exposesMicrophone());
        rejects<std::runtime_error>([&] {
            recordDeviceFormat(sink.key, {48000, 2});
        });
        require(pending(sink.key) == 0);
        const DWORD invalidSinkPending = 7;
        require(RegSetValueExW(sink.key, L"LoopZFormatPending", 0, REG_BINARY,
                               reinterpret_cast<const BYTE*>(&invalidSinkPending),
                               sizeof(invalidSinkPending)) == ERROR_SUCCESS);
        rejects<std::runtime_error>([&] {
            recordAppRoutingSinkConfiguration(sink.key, false);
        });
    }
    {
        Fixture mixNode;
        (void)recordDeviceFormat(mixNode.key,
                                 {48000, 2}); // Missing record already defaults to stereo.
        acknowledge(mixNode.key);
        rejects<std::runtime_error>([&] {
            recordAppRoutingSinkConfiguration(mixNode.key, false);
        });
        rejects<std::runtime_error>([&] {
            recordAppRoutingSinkConfiguration(mixNode.key, true);
        });
        require(pending(mixNode.key) == 0 && !recordDeviceFormat(mixNode.key, {48000, 2}));
    }
    rejects([&] {
        upsertAppRoutingSink(INVALID_HANDLE_VALUE, L"S-1-5-18", L"../oem1.inf");
    });
    rejects([&] {
        upsertAppRoutingSink(INVALID_HANDLE_VALUE, L"not-a-sid", L"oem1.inf");
    });
    rejects([&] {
        removeAppRoutingSink(L"not-a-sid");
    });
    rejects([&] {
        removeAppRoutingSink(L"SY");
    });
    // Invalid requests must fail before any device API can use this invalid set.
    loopz::VirtualDeviceDefinition mix;
    mix.id = "test-mix";
    mix.name = "Test mix";
    mix.sampleRate = 48000;
    mix.routing.outputChannels = 2;
    rejects([&] {
        upsertMixDevice(INVALID_HANDLE_VALUE, L"S-1-5-18", mix, L"../oem1.inf");
    });
    rejects([&] {
        upsertMixDevice(INVALID_HANDLE_VALUE, L"not-a-sid", mix, L"oem1.inf");
    });
    rejects([&] {
        upsertMixDevice(INVALID_HANDLE_VALUE, L"SY", mix, L"oem1.inf");
    });
    mix.name = std::string("bad\0name", 8);
    rejects([&] {
        upsertMixDevice(INVALID_HANDLE_VALUE, L"S-1-5-18", mix, L"oem1.inf");
    });
    mix.name = std::string(1, static_cast<char>(0xff));
    rejects([&] {
        upsertMixDevice(INVALID_HANDLE_VALUE, L"S-1-5-18", mix, L"oem1.inf");
    });
    mix.name = "Test mix";
    mix.routing.outputChannels = 3;
    rejects([&] {
        upsertMixDevice(INVALID_HANDLE_VALUE, L"S-1-5-18", mix, L"oem1.inf");
    });
    std::cout << "Device format persistence and request preflight passed; no devices modified\n";
} catch (const std::exception& error) {
    std::cerr << error.what() << "\n";
    return 1;
}
