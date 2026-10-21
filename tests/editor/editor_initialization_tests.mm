#import <Foundation/Foundation.h>
#include <loopz/macos/engine_runtime.hpp>

#include <array>
#include <cassert>
#include <fstream>
#include <future>
#include <sys/stat.h>

namespace {
using namespace loopz;
using namespace loopz::macos;
class Disconnected final : public DriverConfigurationLink {
  public:
    uint64_t session() const noexcept override {
        return 0;
    }
    int32_t submit(std::span<const DeviceTransport>, uint64_t) override {
        assert(false);
        return -1;
    }
    DriverConfigurationResult status(uint64_t) override {
        assert(false);
        return {};
    }
    int32_t cancel(uint64_t) override {
        assert(false);
        return -1;
    }
    void invalidate() noexcept override {}
};
template <typename Operation> void refuses(Operation operation) {
    bool failed = false;
    try {
        operation();
    } catch (const std::exception&) {
        failed = true;
    }
    assert(failed);
}
std::string bytes(const std::filesystem::path& path) {
    std::ifstream input(path);
    return {std::istreambuf_iterator<char>(input), {}};
}
EngineRuntimeOptions options(const std::filesystem::path& path) {
    EngineRuntimeOptions result;
    result.documentPath = path;
    return result;
}
void firstRun(const std::filesystem::path& path) {
    Disconnected link;
    EngineRuntime runtime(link, options(path));
    runtime.start();
    runtime.poll();
    const auto before = runtime.editorSnapshot();
    assert(!before.document && before.canCreateDocument && before.revision.generation == 0);
    const std::array<edit::Command, 1> commands{edit::CreateDevice{"first", "First", 8}};
    const auto created = runtime.initializeEditor(before.revision, commands);
    assert(created.changed && created.revision.generation == 1);
    assert(runtime.status().documentError.empty());
    const auto saved = loadRouteDocumentFile(path).document;
    assert(saved.devices.size() == 1 && saved.devices[0].id == "first");
    assert(saved.devices[0].routing.outputChannels == 8);
    assert(!runtime.editorSnapshot().canCreateDocument);
    struct stat attributes{};
    assert(stat(path.c_str(), &attributes) == 0 && (attributes.st_mode & 0777) == 0600 &&
           attributes.st_nlink == 1);
    const auto original = bytes(path);
    refuses([&] {
        runtime.initializeEditor(before.revision, commands);
    });
    assert(bytes(path) == original);
    runtime.poll();
    assert(runtime.status().configuration.activeGeneration == 0 &&
           !runtime.status().render.running);
}
void validationBeforeCreation(const std::filesystem::path& path) {
    Disconnected link;
    EngineRuntime runtime(link, options(path));
    const auto revision = runtime.editorSnapshot().revision;
    const std::array<edit::Command, 1> badRate{edit::CreateDevice{"first", "First", 2, 96001}};
    refuses([&] {
        runtime.initializeEditor(revision, badRate);
    });
    assert(!std::filesystem::exists(path) && runtime.editorSnapshot().revision == revision);
    const std::array<edit::Command, 2> empty{edit::CreateDevice{"first", "First"},
                                             edit::DeleteDevice{"first"}};
    refuses([&] {
        runtime.initializeEditor(revision, empty);
    });
    assert(!std::filesystem::exists(path));
    auto validation = options(path);
    validation.installationValidation = true;
    EngineRuntime readOnly(link, validation);
    const std::array<edit::Command, 1> command{edit::CreateDevice{"first", "First"}};
    refuses([&] {
        readOnly.initializeEditor(readOnly.editorSnapshot().revision, command);
    });
    assert(!std::filesystem::exists(path));
}
void existingData(const std::filesystem::path& root) {
    const std::array<edit::Command, 1> command{edit::CreateDevice{"first", "First"}};
    for (const auto& suffix : {std::string(""), std::string(".backup")}) {
        const auto path = root / (suffix.empty() ? "existing.json" : "backup.json");
        const std::string future = "{\"version\":999,\"future\":true}";
        {
            std::ofstream file(path.string() + suffix);
            file << future;
        }
        Disconnected link;
        EngineRuntime runtime(link, options(path));
        assert(!runtime.editorSnapshot().canCreateDocument);
        refuses([&] {
            runtime.initializeEditor(runtime.editorSnapshot().revision, command);
        });
        assert(bytes(path.string() + suffix) == future);
        if (!suffix.empty())
            assert(!std::filesystem::exists(path));
    }
}
void simultaneousCreators(const std::filesystem::path& path) {
    Disconnected a, b;
    EngineRuntime first(a, options(path)), second(b, options(path));
    const auto one = first.editorSnapshot().revision, two = second.editorSnapshot().revision;
    auto create = [&](EngineRuntime& runtime, const EditorRevision& revision,
                      const std::string& id) {
        try {
            const std::array<edit::Command, 1> command{edit::CreateDevice{id, id}};
            runtime.initializeEditor(revision, command);
            return 1;
        } catch (const std::exception&) {
            return 0;
        }
    };
    auto left = std::async(std::launch::async, [&] {
        return create(first, one, "left");
    });
    auto right = std::async(std::launch::async, [&] {
        return create(second, two, "right");
    });
    const auto l = left.get(), r = right.get();
    assert(l + r == 1);
    const auto saved = loadRouteDocumentFile(path).document;
    assert(saved.devices.size() == 1 && saved.devices[0].id == (l ? "left" : "right"));
    assert(!std::filesystem::exists(path.string() + ".backup"));
}
} // namespace
int main() {
    @autoreleasepool {
        const auto root = std::filesystem::path(NSTemporaryDirectory().UTF8String) /
                          [NSUUID UUID].UUIDString.UTF8String;
        std::filesystem::create_directories(root);
        firstRun(root / "first.json");
        validationBeforeCreation(root / "invalid.json");
        existingData(root);
        simultaneousCreators(root / "concurrent.json");
        std::filesystem::remove_all(root);
    }
}
