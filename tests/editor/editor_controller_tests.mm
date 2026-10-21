#import <Foundation/Foundation.h>
#include <loopz/macos/editor_controller.hpp>

#include <array>
#include <cassert>
#include <cstdio>
#include <fstream>
#include <map>
#include <sys/stat.h>

namespace {
using namespace loopz;
using namespace loopz::macos;
using namespace loopz::edit;

class Link final : public DriverConfigurationLink {
  public:
    uint64_t sessionId{1}, submissions{0}, latest{0};
    std::map<uint64_t, DriverConfigurationResult> results;
    uint64_t session() const noexcept override {
        return sessionId;
    }
    int32_t submit(std::span<const DeviceTransport>, uint64_t revision) override {
        ++submissions;
        latest = revision;
        results[revision] = {revision, DriverConfigurationPhase::Committed, 0};
        return 0;
    }
    DriverConfigurationResult status(uint64_t revision) override {
        if (!revision)
            return latest ? results.at(latest)
                          : DriverConfigurationResult{0, DriverConfigurationPhase::Committed, 0};
        return results.at(revision);
    }
    int32_t cancel(uint64_t) override {
        return 0;
    }
    void invalidate() noexcept override {
        sessionId = 0;
    }
};

template <typename Error, typename Operation> void rejects(Operation operation) {
    bool rejected = false;
    try {
        operation();
    } catch (const Error&) {
        rejected = true;
    }
    assert(rejected);
}

RouteDocument initial() {
    const std::array<Command, 2> commands{
        CreateDevice{"mix", "Mix"}, AddSource{"mix", "mic", SourceKind::InputDevice, "uid", 1}};
    auto document = edit::applyCommands({}, commands);
    document.devices[0].formatRequestReceipt = "native-origin:31";
    return document;
}

EditorCommit apply(EditorController& editor, const EditorRevision& revision,
                   std::initializer_list<Command> commands) {
    return editor.apply(revision, {commands.begin(), commands.size()});
}

void settle(ConfigurationController& controller) {
    for (uint64_t now = 0; now < 5000; now += 100) {
        controller.poll(now);
        if (controller.status().phase == RestorationPhase::Restored)
            return;
    }
    assert(false && "Configuration did not settle");
}

std::string bytes(const std::filesystem::path& path) {
    std::ifstream file(path, std::ios::binary);
    assert(file.good());
    return {std::istreambuf_iterator<char>(file), {}};
}

void conflictsAndPersistence(const std::filesystem::path& root) {
    const auto path = root / "routes.json";
    Link link;
    ConfigurationController configuration(link, path, nullptr, false);
    configuration.saveDesired(initial());
    EditorController first(configuration, "engine-epoch-one");
    EditorController second(configuration, "engine-epoch-one");
    const auto before = first.snapshot();
    assert(before.document && before.revision.generation == 1);
    const auto stale = second.snapshot().revision;
    const auto committed =
        apply(first, before.revision,
              {
                  RenameDevice{"mix", "Renamed"},
                  SetSourceControls{"mix", "mic", {.gain = 0.25f, .muted = true}, {}},
                  AddMonitor{"mix", "phones", "output-uid"},
              });
    assert(committed.changed && committed.revision.generation == before.revision.generation + 1);
    assert(link.submissions == 0); // Saved intent isn't a native acknowledgement.
    const auto pending = first.snapshot();
    assert(pending.restoration.activeGeneration == 0);
    assert(pending.restoration.desiredGeneration == committed.revision.generation);
    const auto saved = loadRouteDocumentFile(path).document;
    assert(saved.devices[0].name == "Renamed");
    assert(saved.devices[0].routing.sources[1].gain == 0.25f);
    assert(saved.devices[0].routing.sources[1].muted && saved.devices[0].monitors.size() == 1);
    assert(saved.devices[0].formatRequestReceipt == "native-origin:31");
    rejects<StaleEditorRevision>([&] {
        apply(second, stale, {DeleteDevice{"mix"}});
    });
    assert(bytes(path) == encodeRouteDocumentJSON(saved));
    settle(configuration);
    const auto restored = first.snapshot();
    assert(restored.restoration.phase == RestorationPhase::Restored);
    assert(restored.restoration.activeGeneration == committed.revision.generation);
    assert(configuration.active()->devices[0].name == "Renamed");
    assert(link.submissions == 1);

    // A fresh helper can load the same file and have the same generation. Its
    // epoch must still prevent a request captured before the restart.
    Link restartedLink;
    ConfigurationController restarted(restartedLink, path);
    EditorController afterRestart(restarted, "engine-epoch-two");
    assert(afterRestart.snapshot().revision.generation == before.revision.generation);
    rejects<StaleEditorRevision>([&] {
        apply(afterRestart, before.revision, {RenameDevice{"mix", "Replay"}});
    });
    assert(restarted.desired()->devices[0].name == "Renamed");
}

void noopAndRejectedBatches(const std::filesystem::path& root) {
    const auto path = root / "noop.json";
    Link link;
    ConfigurationController configuration(link, path, nullptr, false);
    configuration.saveDesired(initial());
    EditorController editor(configuration, "engine-epoch");
    const auto revision = editor.snapshot().revision;
    const auto original = bytes(path);
    const auto writeTime = std::filesystem::last_write_time(path);
    struct stat before{}, after{};
    assert(stat(path.c_str(), &before) == 0);
    const auto noop = apply(editor, revision, {RenameDevice{"mix", "Mix"}});
    assert(!noop.changed && noop.revision == revision);
    assert(stat(path.c_str(), &after) == 0 && after.st_ino == before.st_ino);
    assert(std::filesystem::last_write_time(path) == writeTime);
    assert(bytes(path) == original);
    rejects<std::invalid_argument>([&] {
        apply(editor, revision,
              {RenameDevice{"mix", "Partial"}, SetSourceWiring{"mix", "mic", {{0, 0}, {0, 0}}}});
    });
    assert(editor.snapshot().revision == revision);
    assert(configuration.desired()->devices[0].name == "Mix" && bytes(path) == original);
    rejects<std::invalid_argument>([&] {
        apply(editor, revision, {SetDeviceRate{"mix", 96001}});
    });
    assert(configuration.desired()->devices[0].sampleRate == 48000 && bytes(path) == original);
    assert(editor.snapshot().revision == revision);
}

void nativeChangesInvalidateOldEditors(const std::filesystem::path& root) {
    const auto path = root / "rates.json";
    Link link;
    ConfigurationController configuration(link, path, nullptr, false);
    configuration.saveDesired(initial());
    EditorController editor(configuration, "engine-epoch");
    const auto stale = editor.snapshot().revision;
    auto updated = *configuration.desired();
    updated.devices[0].sampleRate = 44100;
    updated.devices[0].formatRequestReceipt = "native-origin:32";
    configuration.saveDesired(updated);
    rejects<StaleEditorRevision>([&] {
        apply(editor, stale, {SetDeviceRate{"mix", 48000}});
    });
    const auto current = editor.snapshot();
    const auto committed = apply(editor, current.revision, {RenameDevice{"mix", "Current"}});
    assert(committed.revision.generation == current.revision.generation + 1);
    assert(configuration.desired()->devices[0].sampleRate == 44100);
    assert(configuration.desired()->devices[0].formatRequestReceipt == "native-origin:32");
    assert(loadRouteDocumentFile(path).document.devices[0].sampleRate == 44100);
}

void disconnectedEditsAndRecovery(const std::filesystem::path& root) {
    const auto path = root / "disconnected.json";
    Link link;
    link.sessionId = 0;
    ConfigurationController configuration(link, path, nullptr, false);
    configuration.saveDesired(initial());
    EditorController editor(configuration, "engine-epoch");
    const auto revision = editor.snapshot().revision;
    const auto committed = apply(
        editor, revision, {ResizeDevice{"mix", 8}, AddMonitor{"mix", "phones", "output-uid", 8}});
    configuration.poll(0);
    assert(!link.submissions);
    assert(editor.snapshot().revision == committed.revision);
    assert(editor.snapshot().restoration.activeGeneration == 0);
    assert(loadRouteDocumentFile(path).document.devices[0].routing.outputChannels == 8);
    link.sessionId = 5;
    settle(configuration);
    assert(configuration.active()->devices[0].routing.outputChannels == 8);
    assert(configuration.active()->devices[0].monitors[0].channels == 8);
    assert(editor.snapshot().restoration.activeGeneration == committed.revision.generation);
}

void historyPersistenceAndReceipts(const std::filesystem::path& root) {
    const auto path = root / "history.json";
    Link link;
    ConfigurationController configuration(link, path, nullptr, false);
    configuration.saveDesired(initial());
    EditorController editor(configuration, "history-epoch");
    const auto initialRevision = editor.snapshot().revision;
    const auto renamed = apply(editor, initialRevision, {RenameDevice{"mix", "Changed"}});
    assert(editor.snapshot().history.canUndo && !editor.snapshot().history.canRedo);
    rejects<StaleEditorRevision>([&] {
        editor.moveHistory(initialRevision, EditorHistoryDirection::Undo);
    });
    const auto undone = editor.moveHistory(renamed.revision, EditorHistoryDirection::Undo);
    assert(undone.revision.generation == renamed.revision.generation + 1);
    assert(configuration.desired()->devices[0].name == "Mix");
    assert(editor.snapshot().history.canRedo && !editor.snapshot().history.canUndo);
    apply(editor, undone.revision, {RenameDevice{"mix", "Mix"}});
    assert(editor.snapshot().history.canRedo); // No-op must not erase redo.
    editor.moveHistory(undone.revision, EditorHistoryDirection::Redo);
    assert(loadRouteDocumentFile(path).document.devices[0].name == "Changed");

    auto native = *configuration.desired();
    native.devices[0].formatRequestReceipt = "native-origin:32";
    configuration.saveDesired(native);
    auto snapshot = editor.snapshot();
    assert(snapshot.history.canUndo);
    editor.moveHistory(snapshot.revision, EditorHistoryDirection::Undo);
    const auto saved = loadRouteDocumentFile(path).document;
    assert(saved.devices[0].name == "Mix" &&
           saved.devices[0].formatRequestReceipt == "native-origin:32");
    assert(editor.snapshot().history.canRedo);
    native = *configuration.desired();
    native.devices[0].sampleRate = 44100;
    configuration.saveDesired(native);
    snapshot = editor.snapshot();
    assert(!snapshot.history.canUndo && !snapshot.history.canRedo);
    rejects<std::invalid_argument>([&] {
        editor.moveHistory(snapshot.revision, EditorHistoryDirection::Redo);
    });
    apply(editor, snapshot.revision, {RenameDevice{"mix", "Branch"}});
    editor.moveHistory(editor.snapshot().revision, EditorHistoryDirection::Undo);
    assert(configuration.desired()->devices[0].sampleRate == 44100);

    EditorController restarted(configuration, "new-history-epoch");
    assert(!restarted.snapshot().history.canUndo && !restarted.snapshot().history.canRedo);
    rejects<StaleEditorRevision>([&] {
        restarted.moveHistory(editor.snapshot().revision, EditorHistoryDirection::Undo);
    });
}

void historyInitializationDeletionAndFailedSave(const std::filesystem::path& root) {
    const auto parent = root / "history-storage";
    std::filesystem::create_directory(parent);
    const auto path = parent / "routes.json";
    Link link;
    ConfigurationController configuration(link, path, nullptr, false);
    EditorController editor(configuration, "create-history");
    const std::array<Command, 1> create{CreateDevice{"stable", "First"}};
    editor.initialize(editor.snapshot().revision, create);
    assert(editor.snapshot().history.canUndo);
    editor.moveHistory(editor.snapshot().revision, EditorHistoryDirection::Undo);
    assert(configuration.desired()->devices.empty() && std::filesystem::is_regular_file(path));
    editor.moveHistory(editor.snapshot().revision, EditorHistoryDirection::Redo);
    assert(configuration.desired()->devices[0].id == "stable");
    apply(editor, editor.snapshot().revision, {DeleteDevice{"stable"}});
    editor.moveHistory(editor.snapshot().revision, EditorHistoryDirection::Undo);
    assert(configuration.desired()->devices[0].id == "stable");
    assert(editor.snapshot().history.canRedo);

    const auto revision = editor.snapshot().revision;
    const auto moved = root / "retained-history-storage";
    std::filesystem::rename(parent, moved);
    {
        std::ofstream blocker(parent);
        blocker << "not a directory";
    }
    rejects<std::exception>([&] {
        editor.moveHistory(revision, EditorHistoryDirection::Redo);
    });
    assert(editor.snapshot().revision == revision && editor.snapshot().history.canRedo);
    assert(configuration.desired()->devices[0].id == "stable");
    std::filesystem::remove(parent);
    std::filesystem::rename(moved, parent);
    editor.moveHistory(revision, EditorHistoryDirection::Redo);
    assert(configuration.desired()->devices.empty());
    assert(loadRouteDocumentFile(path).document.devices.empty());
    settle(configuration);
    assert(configuration.active()->devices.empty());
}

void missingDocumentAndSaveFailure(const std::filesystem::path& root) {
    const auto parent = root / "blocker";
    {
        std::ofstream file(parent);
        file << "not a directory";
    }
    Link link;
    ConfigurationController configuration(link, parent / "routes.json", nullptr, false);
    EditorController editor(configuration, "engine-epoch");
    assert(!editor.snapshot().document);
    rejects<std::runtime_error>([&] {
        apply(editor, editor.snapshot().revision, {CreateDevice{"first", "First"}});
    });
    assert(!configuration.desired());
    configuration.setDesired(initial());
    const auto revision = editor.snapshot().revision;
    rejects<std::exception>([&] {
        apply(editor, revision, {RenameDevice{"mix", "Unsaved"}});
    });
    assert(configuration.desired()->devices[0].name == "Mix");
    assert(editor.snapshot().revision == revision && bytes(parent) == "not a directory");
    std::filesystem::remove(parent);
    std::filesystem::create_directory(parent);
    const auto committed = apply(editor, revision, {RenameDevice{"mix", "Saved"}});
    assert(committed.changed && committed.revision.generation == revision.generation + 1);
    assert(loadRouteDocumentFile(parent / "routes.json").document.devices[0].name == "Saved");
    rejects<std::invalid_argument>([&] {
        EditorController invalid(configuration, "");
    });
}
} // namespace

int main() {
    @autoreleasepool {
        const auto root = std::filesystem::path(NSTemporaryDirectory().UTF8String) /
                          [NSUUID UUID].UUIDString.UTF8String;
        std::filesystem::create_directories(root);
        conflictsAndPersistence(root);
        noopAndRejectedBatches(root);
        nativeChangesInvalidateOldEditors(root);
        disconnectedEditsAndRecovery(root);
        historyPersistenceAndReceipts(root);
        historyInitializationDeletionAndFailedSave(root);
        missingDocumentAndSaveFailure(root);
        std::filesystem::remove_all(root);
        std::puts("Editor revision conflicts, durable batches and recovery passed");
    }
}
