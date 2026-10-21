#import <Foundation/Foundation.h>
#include <loopz/macos/route_store.hpp>

#include <atomic>
#include <cassert>
#include <fstream>
#include <iterator>
#include <latch>
#include <thread>
#include <sys/stat.h>

namespace {
using namespace loopz;
using namespace loopz::macos;

RouteDocument example() {
    VirtualDeviceDefinition studio;
    studio.id = "studio";
    studio.name = "Studio 音";
    studio.routing.sources = {
        {"app", 2, 0.5f, true, false, {{0, 0}, {1, 1}}},
        {"microphone", 1, 0.25f, true, true, {{0, 0}, {0, 1}}},
        {"pass", 2, 1, false, false, {{0, 0}, {1, 1}}},
    };
    studio.bindings = {{"app", SourceKind::Application, "com.example.player", false},
                       {"microphone", SourceKind::InputDevice, "input-device", true},
                       {"pass", SourceKind::PassThrough, "", true}};
    VirtualDeviceDefinition call;
    call.id = "call";
    call.name = "Call";
    call.sampleRate = 44100;
    call.routing.safetyLimiter = false;
    call.routing.sources = {{"studio-source", 2, 1, true, false, {{0, 1}, {1, 0}}}};
    call.bindings = {{"studio-source", SourceKind::VirtualDevice, "studio", true}};
    call.monitors = {{"headphones", "output-device", 4, 0.75f, true, false, {{0, 1}, {1, 3}}}};
    return {kRouteDocumentVersion, {studio, call}};
}

template <typename Error = std::exception, typename Operation> void rejected(Operation operation) {
    bool failed = false;
    try {
        operation();
    } catch (const Error&) {
        failed = true;
    }
    assert(failed);
}

std::string bytes(const std::filesystem::path& path) {
    std::ifstream file(path, std::ios::binary);
    return {std::istreambuf_iterator<char>(file), std::istreambuf_iterator<char>()};
}

void write(const std::filesystem::path& path, const std::string& value) {
    std::ofstream file(path, std::ios::binary | std::ios::trunc);
    file << value;
    assert(file.good());
}

void codecValidation() {
    auto legacy = example();
    legacy.version = 1;
    const auto legacyJSON = encodeRouteDocumentJSON(legacy);
    assert(legacyJSON.find("formatRequestReceipt") == std::string::npos);
    assert(decodeRouteDocumentJSON(legacyJSON).version == 1);
    legacy.devices[0].formatRequestReceipt = "receipt";
    rejected<std::invalid_argument>([&] {
        (void)encodeRouteDocumentJSON(legacy);
    });
    legacy.version = kRouteDocumentVersion;
    const auto upgraded = decodeRouteDocumentJSON(encodeRouteDocumentJSON(legacy));
    assert(upgraded.version == kRouteDocumentVersion &&
           upgraded.devices[0].formatRequestReceipt == "receipt");
    legacy.devices[0].formatRequestReceipt = std::string(129, 'x');
    rejected<std::invalid_argument>([&] {
        (void)encodeRouteDocumentJSON(legacy);
    });
    const auto encoded = encodeRouteDocumentJSON(example());
    const auto decoded = decodeRouteDocumentJSON(encoded);
    assert(encodeRouteDocumentJSON(decoded) == encoded);
    assert(decoded.devices[0].name == "Studio 音");
    assert(decoded.devices[0].routing.sources[1].muted);
    assert(!decoded.devices[0].bindings[0].muteOriginal);
    assert(decoded.devices[1].sampleRate == 44100);
    assert(decoded.devices[1].monitors[0].connections[1].outputChannel == 3);
    rejected<std::invalid_argument>([] {
        (void)decodeRouteDocumentJSON("[]");
    });
    rejected<std::invalid_argument>([] {
        (void)decodeRouteDocumentJSON("{");
    });
    rejected<std::invalid_argument>([] {
        (void)decodeRouteDocumentJSON(R"({"version":true,"devices":[]})");
    });
    rejected<UnsupportedRouteVersion>([] {
        (void)decodeRouteDocumentJSON(R"({"version":99,"devices":[]})");
    });
    const auto edit = [&](void (^change)(NSMutableDictionary*)) {
        NSData* data = [NSData dataWithBytes:encoded.data() length:encoded.size()];
        NSMutableDictionary* object =
            [NSJSONSerialization JSONObjectWithData:data
                                            options:NSJSONReadingMutableContainers
                                              error:nil];
        change(object);
        NSData* result = [NSJSONSerialization dataWithJSONObject:object options:0 error:nil];
        return std::string(static_cast<const char*>(result.bytes), result.length);
    };
    rejected<std::invalid_argument>([&] {
        (void)decodeRouteDocumentJSON(edit(^(NSMutableDictionary* root) {
            root[@"devices"][0][@"routing"][@"enabled"] = @1;
        }));
    });
    rejected<std::invalid_argument>([&] {
        (void)decodeRouteDocumentJSON(edit(^(NSMutableDictionary* root) {
            root[@"devices"][0][@"routing"][@"outputChannels"] = @2.5;
        }));
    });
    rejected<std::invalid_argument>([&] {
        (void)decodeRouteDocumentJSON(edit(^(NSMutableDictionary* root) {
            root[@"devices"][0][@"routing"][@"sources"][0][@"connections"][0][@"outputChannel"] =
                @64;
        }));
    });
    rejected<std::invalid_argument>([&] {
        (void)decodeRouteDocumentJSON(edit(^(NSMutableDictionary* root) {
            root[@"extra"] = @"unsupported";
        }));
    });
}

void filesAndRecovery(const std::filesystem::path& folder) {
    const auto path = folder / "routes/configuration.json";
    auto document = example();
    saveRouteDocumentFile(path, document);
    assert(!loadRouteDocumentFile(path).recoveredBackup);
    struct stat info{};
    assert(::stat(path.c_str(), &info) == 0 && (info.st_mode & 0777) == 0600);
    document.devices[0].name = "Second version";
    saveRouteDocumentFile(path, document);
    const auto before = bytes(path);
    document.devices[0].routing.outputChannels = 3;
    rejected([&] {
        saveRouteDocumentFile(path, document);
    });
    assert(bytes(path) == before);
    document.devices[0].routing.outputChannels = 2;
    write(path, "{incomplete");
    const auto recovered = loadRouteDocumentFile(path);
    assert(recovered.recoveredBackup && !recovered.primaryError.empty());
    assert(recovered.document.devices[0].name == "Studio 音");
    saveRouteDocumentFile(path, document);
    assert(!loadRouteDocumentFile(path).recoveredBackup);
    assert(decodeRouteDocumentJSON(bytes(path.string() + ".backup")).devices[0].name ==
           "Studio 音");

    // Never downgrade a future document by silently loading an older backup.
    const std::string future = R"({"version":99,"devices":[]})";
    write(path, future);
    rejected<UnsupportedRouteVersion>([&] {
        (void)loadRouteDocumentFile(path);
    });
    rejected<UnsupportedRouteVersion>([&] {
        saveRouteDocumentFile(path, document);
    });
    assert(bytes(path) == future);

    for (const auto& evolved :
         {R"({"version":99})", R"({"version":99,"devices":[],"newField":true})"}) {
        write(path, evolved);
        rejected<UnsupportedRouteVersion>([&] {
            (void)loadRouteDocumentFile(path);
        });
        rejected<UnsupportedRouteVersion>([&] {
            saveRouteDocumentFile(path, document);
        });
        assert(bytes(path) == evolved);
    }

    // Preserve newer schemas even when only the backup remains readable.
    const auto backup = std::filesystem::path(path.string() + ".backup");
    write(backup, future);
    write(path, "{incomplete");
    rejected<UnsupportedRouteVersion>([&] {
        (void)loadRouteDocumentFile(path);
    });
    rejected<UnsupportedRouteVersion>([&] {
        saveRouteDocumentFile(path, document);
    });
    assert(bytes(path) == "{incomplete" && bytes(backup) == future);
    std::filesystem::remove(path);
    rejected<UnsupportedRouteVersion>([&] {
        saveRouteDocumentFile(path, document);
    });
    assert(!std::filesystem::exists(path) && bytes(backup) == future);
    write(path, future);

    const auto symlink = folder / "linked.json";
    std::filesystem::create_symlink(path, symlink);
    rejected([&] {
        saveRouteDocumentFile(symlink, document);
    });
    assert(bytes(path) == future);
}

void atomicReaders(const std::filesystem::path& folder) {
    const auto path = folder / "concurrent.json";
    auto document = example();
    saveRouteDocumentFile(path, document);
    std::atomic<bool> failed{false};
    std::atomic<uint32_t> reads{0};
    std::latch firstRead(1);
    {
        std::jthread reader([&](std::stop_token stop) {
            try {
                while (!stop.stop_requested()) {
                    const auto result = loadRouteDocumentFile(path);
                    assert(!result.recoveredBackup && result.document.devices.size() == 2);
                    ++reads;
                    if (reads.load() == 1)
                        firstRead.count_down();
                }
            } catch (...) {
                failed.store(true);
                if (reads.load() == 0)
                    firstRead.count_down();
            }
        });
        firstRead.wait();
        for (uint32_t index = 0; index < 20; ++index) {
            document.devices[0].name = "Saved " + std::to_string(index);
            saveRouteDocumentFile(path, document);
        }
    }
    assert(!failed.load() && reads.load() > 0);
    assert(loadRouteDocumentFile(path).document.devices[0].name == "Saved 19");
}
} // namespace

int main() {
    @autoreleasepool {
        codecValidation();
        auto pattern =
            (std::filesystem::temp_directory_path() / "loopz-route-store-XXXXXX").string();
        std::vector<char> temporary(pattern.begin(), pattern.end());
        temporary.push_back('\0');
        const auto* created = ::mkdtemp(temporary.data());
        assert(created);
        const std::filesystem::path directory(created);
        try {
            filesAndRecovery(directory);
            atomicReaders(directory);
        } catch (...) {
            std::filesystem::remove_all(directory);
            throw;
        }
        std::filesystem::remove_all(directory);
        std::puts("route store: schema, round trip, atomic save, backup and future-version checks "
                  "passed");
    }
}
