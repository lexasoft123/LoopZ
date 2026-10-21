#import <Foundation/Foundation.h>
#include <loopz/macos/route_store.hpp>

#include <algorithm>
#include <array>
#include <charconv>
#include <cstdio>
#include <iostream>
#include <stdexcept>

namespace {
std::string inputJSON() {
    std::string result;
    std::array<char, 8192> buffer{};
    while (std::cin) {
        std::cin.read(buffer.data(), buffer.size());
        const auto count = static_cast<size_t>(std::cin.gcount());
        if (result.size() + count > loopz::macos::kMaximumRouteFileBytes)
            throw std::invalid_argument("Configuration input exceeds 16 MiB");
        result.append(buffer.data(), count);
    }
    if (std::cin.bad())
        throw std::runtime_error("Configuration input could not be read");
    return result;
}

int run(int argc, char** argv) {
    using namespace loopz;
    using namespace loopz::macos;
    if (argc < 3)
        throw std::invalid_argument(
            "usage: loopz-config read|validate|write PATH | create PATH NAME | "
            "rename PATH ID NAME | resize PATH ID CHANNELS | delete|enable|disable PATH ID");
    const std::string command = argv[1];
    const std::filesystem::path path = argv[2];
    if (command == "write" && argc == 3) {
        saveRouteDocumentFile(path, decodeRouteDocumentJSON(inputJSON()));
        std::puts("{\"saved\":true}");
        return 0;
    }
    LoadedRouteDocument loaded;
    if (!(command == "create" && !std::filesystem::exists(path) &&
          !std::filesystem::exists(path.string() + ".backup")))
        loaded = loadRouteDocumentFile(path);
    auto& document = loaded.document;
    if ((command == "read" || command == "validate") && argc == 3) {
        if (command == "read")
            std::puts(encodeRouteDocumentJSON(document).c_str());
        else
            std::printf("{\"valid\":true,\"devices\":%zu,\"recoveredBackup\":%s}\n",
                        document.devices.size(), loaded.recoveredBackup ? "true" : "false");
        if (loaded.recoveredBackup)
            std::fprintf(stderr, "Recovered the last valid backup: %s\n",
                         loaded.primaryError.c_str());
        return 0;
    }
    if (command == "create" && argc == 4) {
        VirtualDeviceDefinition device;
        device.id = [[[NSUUID UUID] UUIDString] lowercaseString].UTF8String;
        device.name = argv[3];
        device.routing.sources.push_back(
            {"pass-through", 2, 1, true, false, defaultChannelConnections(2, 2)});
        device.bindings.push_back({"pass-through", SourceKind::PassThrough, "", false});
        document.devices.push_back(std::move(device));
    } else if (command == "delete" && argc == 4) {
        removeDevice(document, argv[3]);
    } else if ((command == "enable" || command == "disable") && argc == 4) {
        const auto device =
            std::find_if(document.devices.begin(), document.devices.end(), [&](const auto& value) {
                return value.id == argv[3];
            });
        if (device == document.devices.end())
            throw std::invalid_argument("Configured device does not exist");
        device->routing.enabled = command == "enable";
    } else if ((command == "rename" || command == "resize") && argc == 5) {
        if (command == "rename") {
            const auto device = std::find_if(document.devices.begin(), document.devices.end(),
                                             [&](const auto& value) {
                                                 return value.id == argv[3];
                                             });
            if (device == document.devices.end())
                throw std::invalid_argument("Configured device does not exist");
            device->name = argv[4];
        } else {
            const std::string value = argv[4];
            uint32_t channels = 0;
            const auto [end, error] =
                std::from_chars(value.data(), value.data() + value.size(), channels);
            if (error != std::errc{} || end != value.data() + value.size())
                throw std::invalid_argument("Channel count must be an integer");
            resizeDeviceOutputs(document, argv[3], channels);
        }
    } else {
        throw std::invalid_argument("Unknown configuration command or argument count");
    }
    saveRouteDocumentFile(path, document);
    std::puts(encodeRouteDocumentJSON(document).c_str());
    return 0;
}
} // namespace

int main(int argc, char** argv) {
    @autoreleasepool {
        try {
            return run(argc, argv);
        } catch (const std::exception& error) {
            std::fprintf(stderr, "%s\n", error.what());
            return 1;
        }
    }
}
