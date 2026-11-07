#include <loopz/runtime/control.hpp>
#include <array>
#include <algorithm>
#include <charconv>
#include <cmath>
#include <fcntl.h>
#include <fstream>
#include <random>
#include <map>
#include <sys/stat.h>
#include <unistd.h>

namespace loopz::runtime {
namespace {
struct FD {
    int value;
    ~FD() {
        if (value >= 0)
            ::close(value);
    }
};
std::runtime_error io(const char* message) {
    return std::runtime_error(message);
}
std::string readState(const std::filesystem::path& path) {
    FD fd{::open(path.c_str(), O_RDONLY | O_CLOEXEC | O_NOFOLLOW | O_NONBLOCK)};
    if (fd.value < 0)
        throw io("Cannot open saved routing");
    struct stat st{};
    if (::fstat(fd.value, &st) || !S_ISREG(st.st_mode) || st.st_size < 0 || st.st_size > 1048576)
        throw io("Saved routing must be a bounded regular file");
    std::string bytes;
    std::array<char, 8192> block{};
    for (;;) {
        auto n = ::read(fd.value, block.data(), block.size());
        if (n < 0 && errno == EINTR)
            continue;
        if (n < 0)
            throw io("Cannot read saved routing");
        if (!n)
            return bytes;
        bytes.append(block.data(), static_cast<size_t>(n));
        if (bytes.size() > 1048576)
            throw io("Saved routing exceeds size limit");
    }
}
class DurabilityUncertain : public std::runtime_error {
  public:
    using std::runtime_error::runtime_error;
};
void saveState(const std::filesystem::path& path, const std::string& bytes) {
    auto parent = path.parent_path();
    if (parent.empty())
        parent = ".";
    if (std::filesystem::create_directories(parent) && ::chmod(parent.c_str(), 0700))
        throw io("Cannot protect routing directory");
    auto pattern = (parent / ".loopz-state-XXXXXX").string();
    std::vector<char> name(pattern.begin(), pattern.end());
    name.push_back(0);
    FD fd{::mkstemp(name.data())};
    if (fd.value < 0)
        throw io("Cannot create routing replacement");
    struct Temporary {
        const char* name;
        ~Temporary() {
            ::unlink(name);
        }
    } temporary{name.data()};
    if (::fchmod(fd.value, 0600))
        throw io("Cannot protect routing replacement");
    size_t written = 0;
    while (written < bytes.size()) {
        auto n = ::write(fd.value, bytes.data() + written, bytes.size() - written);
        if (n < 0 && errno == EINTR)
            continue;
        if (n <= 0)
            throw io("Cannot write routing replacement");
        written += static_cast<size_t>(n);
    }
    if (::fsync(fd.value))
        throw io("Cannot flush routing replacement");
    if (::rename(name.data(), path.c_str()))
        throw io("Cannot publish routing replacement");
    FD directory{::open(parent.c_str(), O_RDONLY | O_DIRECTORY | O_CLOEXEC)};
    // A failure after rename is uncertain: callers must reconcile from disk.
    if (directory.value < 0 || ::fsync(directory.value))
        throw DurabilityUncertain("Routing published; directory durability uncertain");
}
uint64_t generation(const JSON& value) {
    if (!value.is_string())
        throw std::invalid_argument("Generation must be a decimal string");
    auto s = value.get<std::string>();
    uint64_t result{};
    auto [end, ec] = std::from_chars(s.data(), s.data() + s.size(), result);
    if (s.empty() || (s.size() > 1 && s[0] == '0') || ec != std::errc{} ||
        end != s.data() + s.size())
        throw std::invalid_argument("Invalid generation");
    return result;
}
JSON reply(bool ok, const char* code, const std::string& detail = {}) {
    return {{"version", 1}, {"ok", ok}, {"code", code}, {"detail", detail}};
}
JSON meter(const AudioMeterSnapshot& m) {
    JSON peak = JSON::array(), rms = JSON::array();
    for (uint32_t c = 0; c < m.channels; ++c) {
        peak.push_back(m.peak[c]);
        rms.push_back(m.rms[c]);
    }
    return {{"valid", m.frames > 0},
            {"channels", m.channels},
            {"frames", m.frames},
            {"peak", peak},
            {"rms", rms}};
}
JSON reportedFrames(uint32_t frames) {
    return frames ? JSON(frames) : JSON(nullptr);
}
JSON endpoint(const EndpointStatus& e) {
    JSON result = {{"id", e.id},
                   {"connected", e.connected},
                   {"error", e.error},
                   {"xruns", std::to_string(e.audio.xruns)},
                   {"callbacks", std::to_string(e.audio.callbacks)},
                   {"renderedFrames", std::to_string(e.audio.renderedFrames)},
                   {"renderFailures", std::to_string(e.audio.renderFailures)},
                   {"invalidCallbacks", std::to_string(e.audio.invalidCallbacks)},
                   {"discontinuities", std::to_string(e.audio.discontinuities)},
                   {"deadlineMisses", std::to_string(e.audio.deadlineMisses)},
                   {"sampleRate", e.audio.format.sampleRate > 0 ? JSON(e.audio.format.sampleRate)
                                                                : JSON(nullptr)},
                   {"inputChannels", e.audio.format.inputChannels},
                   {"outputChannels", e.audio.format.outputChannels},
                   {"outputPeriodFrames", reportedFrames(e.audio.diagnostics.outputPeriodFrames)},
                   {"outputBufferFrames", reportedFrames(e.audio.diagnostics.outputBufferFrames)},
                   {"latencyFrames",
                    {{"inputDevice", reportedFrames(e.audio.latency.inputDeviceFrames)},
                     {"outputDevice", reportedFrames(e.audio.latency.outputDeviceFrames)},
                     {"buffer", reportedFrames(e.audio.latency.bufferFrames)},
                     {"externalRoute", reportedFrames(e.audio.latency.externalRouteFrames)}}},
                   {"periodFrames", reportedFrames(e.audio.format.nominalBufferFrames)},
                   {"streamGeneration", std::to_string(e.audio.streamGeneration)}};
    if (e.captureTelemetry) {
        result["xruns"] = nullptr;
        result["renderedFrames"] = nullptr;
        result["renderFailures"] = nullptr;
        result["invalidCallbacks"] = nullptr;
        result["streamGeneration"] = nullptr;
        result["callbacksScope"] = "admitted-history-blocks";
        result["deadlineMisses"] = nullptr;
        result["periodFrames"] = nullptr;
        result["timestampQuality"] = static_cast<int>(e.timestampQuality);
        result["sourceSequenceAvailable"] = e.sourceSequenceAvailable;
        result["sourceCounterHardware"] = e.sourceCounterHardware;
        result["sourceCounterOrigin"] = e.sourceSequenceAvailable ? "received-pcm" : "unavailable";
        result["observedXruns"] =
            e.captureMetadataValid ? JSON(std::to_string(e.observedXruns)) : JSON(nullptr);
        result["captureBoundaries"] = std::to_string(e.captureBoundaries);
        if (e.captureMetadataValid) {
            const auto& metadata = e.captureMetadata;
            result["streamGeneration"] = std::to_string(metadata.streamGeneration);
            result["capture"] = {
                {"clockDomain", std::to_string(metadata.clockDomainId)},
                {"streamGeneration", std::to_string(metadata.streamGeneration)},
                {"sequence", std::to_string(metadata.sequence)},
                {"sourceFrame", (metadata.flags & singz::AudioInputSourceFrameValid)
                                    ? JSON(std::to_string(metadata.sourceFrame))
                                    : JSON(nullptr)},
                {"sampleHostTimeNs", (metadata.flags & singz::AudioInputSampleHostTimeValid)
                                         ? JSON(std::to_string(metadata.sampleHostTimeNs))
                                         : JSON(nullptr)},
                {"callbackHostTimeNs", (metadata.flags & singz::AudioInputCallbackHostTimeValid)
                                           ? JSON(std::to_string(metadata.callbackHostTimeNs))
                                           : JSON(nullptr)},
                {"flags", metadata.flags},
                {"discontinuity", static_cast<int>(metadata.discontinuity)}};
        } else
            result["capture"] = nullptr;
        result["measuredRate"] =
            e.captureHistory.measuredRate > 0 ? JSON(e.captureHistory.measuredRate) : JSON(nullptr);
        result["callbacks"] = std::to_string(e.captureHistory.callbacks);
        const auto& reads = e.captureReads;
        JSON missing = JSON::object();
        constexpr std::array names{"invalidRequest", "offline",           "anchorUnavailable",
                                   "clockMapping",   "resamplingSupport", "generationChanged"};
        for (size_t index = 0; index < names.size(); ++index)
            missing[names[index]] = std::to_string(reads.missingFrames[index]);
        result["historyReads"] = {
            {"enabled", reads.enabled},
            {"reads", std::to_string(reads.reads)},
            {"requestedFrames", std::to_string(reads.requestedFrames)},
            {"missingFrames", missing},
            {"unpublishedSampleReads", std::to_string(reads.unpublishedSampleReads)},
            {"expiredSampleReads", std::to_string(reads.expiredSampleReads)},
            {"racingSampleReads", std::to_string(reads.racingSampleReads)},
            {"absentSampleReads", std::to_string(reads.absentSampleReads)},
            {"maximumVerifiedClockStepNs", std::to_string(reads.maximumVerifiedClockStepNs)},
            {"verifiedClockSteps", std::to_string(reads.verifiedClockSteps)}};
        result["historyClock"] = {
            {"projected", e.captureHistoryProjected},
            {"maximumRawPhaseStepNs", std::to_string(e.captureMaximumRawPhaseStepNs)},
            {"maximumProjectionErrorNs", std::to_string(e.captureMaximumProjectionErrorNs)}};
    }
    return result;
}
} // namespace
Control::Control(std::filesystem::path state) : state_(std::move(state)) {
    std::random_device random;
    for (int i = 0; i < 4; ++i)
        epoch_ += std::to_string(random()) + "-";
    try {
        inventory_ = audio_.inventory();
    } catch (const std::exception& error) {
        inventoryError_ = error.what();
    }
    try {
        if (std::filesystem::exists(std::filesystem::symlink_status(state_))) {
            auto saved = parseControlJSON(readState(state_));
            if (saved.at("version") != 1)
                throw io("Unsupported saved Engine state version");
            desiredGeneration_ = generation(saved.at("generation"));
            desired_ = decodeDocument(saved.at("document"));
            activate();
        }
    } catch (const std::exception& error) {
        storageError_ = error.what();
    }
}
void Control::activate() {
    if (!desired_)
        return;
    try {
        auto candidate = audio_.prepare(*desired_);
        audio_.commit(std::move(candidate));
        activeGeneration_ = desiredGeneration_;
        activationError_.clear();
    } catch (const std::exception& error) {
        activationError_ = error.what();
    }
}
void Control::poll() {
    try {
        inventory_ = audio_.inventory();
        inventoryError_.clear();
    } catch (const std::exception& error) {
        inventoryError_ = error.what();
    }
    audio_.poll();
    if (desired_ && activeGeneration_ != desiredGeneration_)
        activate();
}
JSON Control::snapshot() {
    audio_.requestTelemetryPreview();
    const auto runtime = audio_.status();
    const auto& inventory = inventory_;
    JSON devices = JSON::array(), inputs = JSON::array(), outputs = JSON::array();
    JSON buses = JSON::array(), monitors = JSON::array(), sources = JSON::array();
    for (const auto& bus : runtime.devices) {
        JSON entries = JSON::array();
        for (const auto& source : bus.sources)
            entries.push_back({{"id", source.id},
                               {"online", source.status.online},
                               {"missingFrames", std::to_string(source.status.missingFrames)},
                               {"meter", meter(source.meter)}});
        buses.push_back({{"id", bus.id},
                         {"active", bus.active},
                         {"meter", meter(bus.meter)},
                         {"sources", entries}});
    }
    for (const auto& monitor : runtime.monitors)
        monitors.push_back(
            {{"device", monitor.device},
             {"monitor", monitor.monitor},
             {"meter", meter(monitor.meter)},
             {"sourceOnline", monitor.status.sourceOnline},
             {"callbacks", std::to_string(monitor.status.callbacks)},
             {"graphFailures", std::to_string(monitor.status.graphFailures)},
             {"invalidBlocks", std::to_string(monitor.status.invalidBlocks)},
             {"missingFrames", std::to_string(monitor.status.missingFrames)},
             {"measuredOutputRate", monitor.status.measuredOutputRate},
             {"historyBudgetFrames",
              monitor.historyBudgetKnown ? JSON(monitor.historyDelayFrames) : JSON(nullptr)},
             {"historyBudgetKnown", monitor.historyBudgetKnown},
             {"historyBudget", monitor.historyBudgetKnown
                                   ? JSON{{"queuedOutput", monitor.queuedOutputFrames},
                                          {"outputPeriod", monitor.outputPeriodFrames},
                                          {"filterFuture", monitor.filterFutureFrames},
                                          {"schedulerReserve", monitor.schedulerReserveFrames}}
                                   : JSON(nullptr)}});
    for (auto& monitor : monitors) {
        if (!desired_)
            continue;
        for (const auto& device : desired_->devices)
            for (const auto& definition : device.monitors) {
                if (device.id != monitor["device"].get<std::string>() ||
                    definition.id != monitor["monitor"].get<std::string>())
                    continue;
                auto output = std::find_if(runtime.outputs.begin(), runtime.outputs.end(),
                                           [&](const auto& e) {
                                               return e.id == definition.deviceSelector;
                                           });
                monitor["complete"] = output != runtime.outputs.end() && output->connected;
                monitor["connected"] = output != runtime.outputs.end() && output->connected;
                monitor["error"] =
                    output != runtime.outputs.end() ? output->error : activationError_;
            }
    }
    std::map<std::pair<std::string, std::string>, JSON> sourceConnections;
    if (desired_)
        for (const auto& device : desired_->devices)
            for (const auto& binding : device.bindings) {
                if (binding.kind != SourceKind::InputDevice &&
                    binding.kind != SourceKind::Application)
                    continue;
                const auto route = std::find_if(device.routing.sources.begin(),
                                                device.routing.sources.end(), [&](const auto& r) {
                                                    return r.id == binding.id;
                                                });
                const bool demanded = route != device.routing.sources.end() && route->enabled &&
                                      device.routing.enabled;
                const std::string kind =
                    binding.kind == SourceKind::InputDevice ? "input-device" : "application";
                bool connected = false, complete = false;
                std::string error;
                if (binding.kind == SourceKind::InputDevice) {
                    const auto found = std::find_if(runtime.inputs.begin(), runtime.inputs.end(),
                                                    [&](const auto& e) {
                                                        return e.id == binding.selector;
                                                    });
                    if (found != runtime.inputs.end()) {
                        connected = found->connected;
                        complete =
                            connected && found->captureHistory.online &&
                            found->timestampQuality != singz::AudioInputTimestampQuality::Unknown;
                        error = found->error;
                        if (connected && !complete && error.empty())
                            error = "Capture clock is not ready";
                    } else
                        error = activationError_;
                } else {
                    const auto bus = std::find_if(runtime.devices.begin(), runtime.devices.end(),
                                                  [&](const auto& b) {
                                                      return b.id == device.id;
                                                  });
                    if (bus != runtime.devices.end()) {
                        const auto source = std::find_if(bus->sources.begin(), bus->sources.end(),
                                                         [&](const auto& v) {
                                                             return v.id == binding.id;
                                                         });
                        connected = source != bus->sources.end() && source->status.online;
                        complete = connected;
                    }
                    if (!connected)
                        error = "Native source is offline";
                }
                auto [entry, inserted] = sourceConnections.try_emplace(
                    {kind, binding.selector}, JSON{{"kind", kind},
                                                   {"selector", binding.selector},
                                                   {"demanded", demanded},
                                                   {"connected", connected},
                                                   {"complete", complete},
                                                   {"error", error}});
                if (!inserted) {
                    entry->second["demanded"] = entry->second["demanded"].get<bool>() || demanded;
                    entry->second["connected"] =
                        entry->second["connected"].get<bool>() || connected;
                    entry->second["complete"] = entry->second["complete"].get<bool>() || complete;
                    if (entry->second["complete"].get<bool>())
                        entry->second["error"] = "";
                }
            }
    for (const auto& [identity, connection] : sourceConnections) {
        (void)identity;
        sources.push_back(connection);
    }
    for (const auto& d : inventory.devices)
        devices.push_back({{"uid", d.uid},
                           {"name", d.label},
                           {"inputChannels", d.inputChannels},
                           {"outputChannels", d.outputChannels},
                           {"inputChannelNames", d.inputChannelLabels},
                           {"outputChannelNames", d.outputChannelLabels},
                           {"defaultInput", d.defaultInput},
                           {"defaultOutput", d.defaultOutput},
                           {"rate", d.nominalSampleRate}});
    for (const auto& e : runtime.inputs)
        inputs.push_back(endpoint(e));
    for (const auto& e : runtime.outputs)
        outputs.push_back(endpoint(e));
    if (activeGeneration_ != desiredGeneration_)
        for (auto& m : monitors)
            m["meter"]["valid"] = false;
    const bool restored = desired_ && activeGeneration_ == desiredGeneration_ && runtime.restored;
    return {{"version", 1},
            {"connection", {{"connected", true}, {"ready", storageError_.empty()}}},
            {"inventory",
             {{"applications", JSON::array()},
              {"devices", devices},
              {"devicesFresh", inventoryError_.empty()},
              {"devicesError", inventoryError_}}},
            {"snapshot",
             {{"version", 1},
              {"canCreateDocument", !desired_ && storageError_.empty()},
              {"revision", {{"epoch", epoch_}, {"generation", std::to_string(desiredGeneration_)}}},
              {"document", desired_ ? encodeDocument(*desired_) : JSON(nullptr)},
              {"history",
               {{"canUndo", desired_ && history_.canUndo(encodeDocument(*desired_).dump())},
                {"canRedo", desired_ && history_.canRedo(encodeDocument(*desired_).dump())}}},
              {"restoration",
               {{"phase", restored    ? "restored"
                          : !desired_ ? "no-document"
                          : activeGeneration_ == desiredGeneration_ && runtime.active ? "partial"
                                                                                      : "retry"},
                {"desiredGeneration", std::to_string(desiredGeneration_)},
                {"activeGeneration", std::to_string(activeGeneration_)},
                {"error", storageError_.empty() ? activationError_ : storageError_}}},
              {"runtime",
               {{"platform", "linux"},
                {"sampleRates", JSON::array({48000})},
                {"limits",
                 {{"physicalCaptures", RuntimeLimits::maximumPhysicalCaptures},
                  {"captureChannels", RuntimeLimits::maximumCaptureChannels},
                  {"physicalOutputs", RuntimeLimits::maximumPhysicalOutputs},
                  {"outputChannels", RuntimeLimits::maximumOutputChannels},
                  {"busChannels", RuntimeLimits::maximumBusChannels},
                  {"sourceChannels", RuntimeLimits::maximumSourceChannels},
                  {"connections", RuntimeLimits::maximumConnections}}},
                {"metersFresh", activeGeneration_ == desiredGeneration_},
                {"devices", buses},
                {"monitors", monitors},
                {"sources", sources},
                {"inputs", inputs},
                {"outputs", outputs},
                {"renderedFrames", std::to_string(runtime.renderedFrames)},
                {"graphFailures", std::to_string(runtime.graphFailures)},
                {"missedDeadlines", std::to_string(runtime.missedDeadlines)},
                {"renderLateFrames", std::to_string(runtime.renderLateFrames)},
                {"renderSkippedFrames", std::to_string(runtime.renderSkippedFrames)},
                {"renderReanchors", std::to_string(runtime.renderReanchors)},
                {"editCapabilities", {{"removeOutputPair", true}}}}}}}};
}
JSON Control::request(const JSON& message) {
    try {
        if (!message.is_object() || message.at("version") != 1)
            return reply(false, "unsupported-version");
        const auto operation = message.at("operation").get<std::string>();
        if (operation == "state")
            return snapshot();
        if (operation == "hello")
            return {{"version", 1}, {"ok", true}, {"protocol", "loopz.engine"},
                    {"major", 1},   {"minor", 0}, {"pcm", "native-only"}};
        if (operation == "retry" || operation == "retry-session") {
            if (!storageError_.empty()) {
                auto saved = parseControlJSON(readState(state_));
                if (saved.at("version") != 1)
                    throw std::invalid_argument("Unsupported saved Engine state version");
                auto document = decodeDocument(saved.at("document"));
                auto revision = generation(saved.at("generation"));
                auto parent = state_.parent_path();
                if (parent.empty())
                    parent = ".";
                FD directory{::open(parent.c_str(), O_RDONLY | O_DIRECTORY | O_CLOEXEC)};
                if (directory.value < 0 || ::fsync(directory.value))
                    return reply(false, "storage-unavailable", storageError_);
                desired_ = std::move(document);
                desiredGeneration_ = revision;
                history_ = EditHistory{};
                storageError_.clear();
            }
            activate();
            return reply(true, "ok");
        }
        if (operation != "edit" && operation != "initialize")
            return reply(false, "unsupported-operation");
        if (!storageError_.empty())
            return reply(false, "storage-unavailable", storageError_);
        const auto& request = message.at("request");
        if (request.at("version") != 1)
            return reply(false, "unsupported-version");
        if (request.contains("commands") == request.contains("history"))
            return reply(false, "invalid-request", "Choose exactly one commands or history field");
        const auto& expected = request.at("expected");
        if (expected.at("epoch") != epoch_ ||
            generation(expected.at("generation")) != desiredGeneration_)
            return reply(false, "conflict", "Routing changed; read canonical state before editing");
        if ((operation == "initialize") == desired_.has_value())
            return reply(false, "conflict");
        RouteDocument candidate;
        EditHistory preparedHistory;
        auto before = desired_ ? encodeDocument(*desired_).dump() : encodeDocument({}).dump();
        if (request.contains("history")) {
            const auto direction = request.at("history").get<std::string>();
            if (!desired_ || (direction != "undo" && direction != "redo"))
                throw std::invalid_argument("Invalid history request");
            preparedHistory =
                direction == "undo" ? history_.withUndo(before) : history_.withRedo(before);
            candidate = decodeDocument(parseControlJSON(preparedHistory.current()));
        } else {
            candidate = edit::applyCommands(desired_.value_or(RouteDocument{}),
                                            decodeCommands(request.at("commands")));
            preparedHistory = history_.withEdit(before, encodeDocument(candidate).dump());
        }
        if (operation == "initialize" && candidate.devices.empty())
            throw std::invalid_argument("Create at least one routing bus");
        if (desired_ && before == encodeDocument(candidate).dump())
            return reply(true, "ok");
        if (desiredGeneration_ == UINT64_MAX)
            return reply(false, "generation-exhausted");
        // Prepare before persistence. Unavailable devices retain the prior route
        // while the durable full desired graph remains pending and retryable.
        std::shared_ptr<AudioRuntime::Prepared> prepared;
        std::string pending;
        try {
            prepared = audio_.prepare(candidate);
        } catch (const std::invalid_argument& error) {
            return reply(false, "unsupported-configuration", error.what());
        } catch (const std::exception& error) {
            pending = error.what();
        }
        auto next = desiredGeneration_ + 1;
        // Allocate a definitive acknowledgement before changing durable state.
        auto result = reply(true, "ok");
        result["revision"] = {{"epoch", epoch_}, {"generation", std::to_string(next)}};
        result["activeGeneration"] = std::to_string(prepared ? next : activeGeneration_);
        auto bytes = JSON{{"version", 1},
                          {"generation", std::to_string(next)},
                          {"document", encodeDocument(candidate)}}
                         .dump();
        if (bytes.size() > 1048576)
            throw std::invalid_argument("Saved routing exceeds size limit");
        try {
            saveState(state_, bytes);
        } catch (const DurabilityUncertain& error) {
            desired_ = std::move(candidate);
            desiredGeneration_ = next;
            history_.swap(preparedHistory);
            storageError_ = error.what();
            return reply(false, "uncertain", storageError_);
        } catch (const std::exception& error) {
            return reply(false, "storage-error", error.what());
        }
        desired_ = std::move(candidate);
        desiredGeneration_ = next;
        history_.swap(preparedHistory);
        activationError_ = std::move(pending);
        if (prepared) {
            audio_.commit(std::move(prepared));
            activeGeneration_ = next;
        }
        return result;
    } catch (const std::exception& error) {
        return reply(false, "invalid-request", error.what());
    }
}
} // namespace loopz::runtime
