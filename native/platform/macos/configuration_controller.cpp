#include <loopz/sample_rates.hpp>
#include <loopz/macos/configuration_controller.hpp>

#include <algorithm>
#include <stdexcept>
#include <utility>

namespace loopz::macos {
namespace {
using Phase = DriverConfigurationPhase;
constexpr int32_t kControlFailure = -1;
uint64_t after(uint64_t now, uint64_t delay) noexcept {
    return now > UINT64_MAX - delay ? UINT64_MAX : now + delay;
}
void validateBackendDocument(const RouteDocument& document) {
    validateRouteDocument(document);
    for (const auto& device : document.devices)
        if (!supportedVirtualSampleRate(device.sampleRate))
            throw std::invalid_argument("Unsupported virtual-device sample rate");
}
} // namespace

ConfigurationController::ConfigurationController(DriverConfigurationLink& link,
                                                 std::filesystem::path documentPath,
                                                 GraphActivation* activation, bool loadDocument,
                                                 uint32_t timingInspectionIntervalMs)
    : link_(link), activation_(activation), path_(std::move(documentPath)),
      timingInspectionIntervalMs_(timingInspectionIntervalMs) {
    if (timingInspectionIntervalMs_ > 1000)
        throw std::invalid_argument("Timing inspection interval exceeds recovery bound");
    submission_.setDesired({{"submission", {}}}, 0);
    if (loadDocument && !path_.empty())
        reloadDesired();
}
ConfigurationController::~ConfigurationController() {
    abandonPending();
}
void ConfigurationController::replaceDesired(RouteDocument document, bool save, bool create) {
    if (create && desired_)
        throw std::invalid_argument("Routing is already configured; refresh before editing");
    validateBackendDocument(document);
    if (save) {
        const auto mask = link_.sampleRateMask();
        for (const auto& device : document.devices) {
            const bool unchanged =
                desired_ &&
                std::any_of(desired_->devices.begin(), desired_->devices.end(),
                            [&](const auto& previous) {
                                return previous.id == device.id &&
                                       previous.sampleRate == device.sampleRate &&
                                       (!device.routing.enabled || previous.routing.enabled);
                            });
            if (!unchanged && !(mask & virtualSampleRateBit(device.sampleRate)))
                throw std::invalid_argument("This sample rate requires a compatible LoopZ update");
        }
    }
    auto canonical = encodeRouteDocumentJSON(document);
    const bool changed = !desired_ || canonical != canonicalDesired_;
    if (changed && desiredGeneration_ == UINT64_MAX)
        throw std::overflow_error("Desired graph generation exhausted");
    if (save) {
        if (path_.empty())
            throw std::invalid_argument("No routing document path configured");
        // Persistence succeeds before intent is changed. Atomic move operations
        // below cannot turn a failed save into an unsaved runtime configuration.
        if (create)
            createRouteDocumentFile(path_, document);
        else
            saveRouteDocumentFile(path_, document);
        recoveredBackup_ = false;
    }
    if (!changed)
        return;
    desired_ = std::move(document);
    canonicalDesired_ = std::move(canonical);
    ++desiredGeneration_;
    if (!pending_)
        submission_.restart(0);
    error_ = 0;
    detail_.clear();
    nextPoll_ = 0;
}
void ConfigurationController::setDesired(RouteDocument document) {
    replaceDesired(std::move(document), false);
}
void ConfigurationController::saveDesired(RouteDocument document) {
    replaceDesired(std::move(document), true);
}
void ConfigurationController::createDesired(RouteDocument document) {
    replaceDesired(std::move(document), true, true);
}
void ConfigurationController::reloadDesired() {
    if (path_.empty())
        throw std::invalid_argument("No routing document path configured");
    auto loaded = loadRouteDocumentFile(path_);
    replaceDesired(std::move(loaded.document), false);
    recoveredBackup_ = loaded.recoveredBackup;
}
const RouteDocument* ConfigurationController::desired() const noexcept {
    return desired_ ? &*desired_ : nullptr;
}
const RouteDocument* ConfigurationController::active() const noexcept {
    return active_ ? &*active_ : nullptr;
}
const std::vector<DeviceTransport>& ConfigurationController::devices() const noexcept {
    return pool_.devices();
}
void ConfigurationController::abandonPending() noexcept {
    if (!pending_)
        return;
    // A lost session may have accepted the request. Retire tentative regions
    // before dropping ownership; never retire reused healthy active regions.
    for (const auto& staged : pending_->transports.devices()) {
        const bool active =
            std::any_of(pool_.devices().begin(), pool_.devices().end(), [&](const auto& item) {
                return item.region == staged.region;
            });
        if (!active)
            staged.region->retire();
    }
    pending_.reset();
}
void ConfigurationController::newSession(uint64_t session, uint64_t now) {
    abandonPending();
    session_ = session;
    synchronized_ = false;
    activeGeneration_ = 0; // A previous peer's acknowledgement is not evidence.
    orphanCancellation_ = 0;
    submission_.disconnected(now);
    submission_.retryNow(now);
    nextPoll_ = now;
}
bool ConfigurationController::currentSession(uint64_t now) {
    const auto current = link_.session();
    if (current == session_)
        return true;
    newSession(current, now);
    return false;
}
void ConfigurationController::retry(uint64_t now, int32_t error, std::string detail) {
    error_ = error;
    detail_ = std::move(detail);
    submission_.disconnected(now);
}
void ConfigurationController::protocolFailure(uint64_t now, std::string detail) {
    link_.invalidate();
    newSession(0, now);
    error_ = kControlFailure;
    detail_ = std::move(detail);
}
void ConfigurationController::checkResult(const DriverConfigurationResult& result,
                                          uint64_t revision) const {
    if ((revision && result.revision != revision) || result.phase < Phase::Unknown ||
        result.phase > Phase::Quarantined)
        throw std::runtime_error("Invalid driver configuration acknowledgement");
}
void ConfigurationController::pendingResult(uint64_t now) {
    const auto revision = pending_->revision;
    if (pending_->desiredGeneration != desiredGeneration_ && !pending_->cancelRequested) {
        // Failed cancellation can race a commit. The authoritative terminal
        // phase, not the cancel return value, decides which mappings stay alive.
        const auto error = link_.cancel(revision);
        if (!currentSession(now))
            return;
        pending_->cancelRequested = error == 0;
        if (error) {
            error_ = error;
            detail_ = "Cancellation rejected; waiting for the driver result and retrying";
        }
    }
    auto result = link_.status(revision);
    if (!currentSession(now))
        return;
    checkResult(result, revision);
    switch (result.phase) {
    case Phase::Pending:
        return;
    case Phase::RollingBack:
        pending_->cancelRequested = true;
        return;
    case Phase::Committed: {
        const auto generation = pending_->desiredGeneration;
        pool_.commit(std::move(pending_->transports));
        if (pending_->activation)
            pending_->activation->commit();
        active_ = std::move(pending_->document);
        activeGeneration_ = generation;
        pending_.reset();
        error_ = 0;
        detail_.clear();
        if (activeGeneration_ != desiredGeneration_) {
            submission_.restart(now);
        }
        return;
    }
    case Phase::Aborted:
    case Phase::Quarantined: {
        // Old active mappings remain owned across rollback. Quarantine may have
        // retired some of them; prepare() replaces those on the next retry.
        const bool superseded = pending_->desiredGeneration != desiredGeneration_;
        abandonPending();
        if (superseded) {
            submission_.restart(now);
            return;
        }
        retry(now, result.error,
              result.phase == Phase::Quarantined
                  ? "Driver isolated an unsafe transition; restoring intent"
                  : "Driver rolled back the transition; restoring intent");
        return;
    }
    case Phase::Unknown: {
        // Ordered status can resolve a submit whose reply was lost. An absent
        // revision below the next accepted request means it was never accepted.
        const auto latest = link_.status(0);
        if (!currentSession(now))
            return;
        checkResult(latest, 0);
        if (latest.revision < revision && latest.phase != Phase::Pending &&
            latest.phase != Phase::RollingBack) {
            abandonPending();
            retry(now, kControlFailure, "Submission was not accepted; restoring intent");
        } else
            protocolFailure(now, "Exclusive driver session lost its configuration history");
        return;
    }
    }
}
void ConfigurationController::poll(uint64_t now) {
    const auto observedSession = link_.session();
    if (observedSession != session_)
        newSession(observedSession, now);
    if (!desired_ || !session_ || now < nextPoll_)
        return;
    nextPoll_ = after(now, 100);
    try {
        if (pending_) {
            pendingResult(now);
            return;
        }
        if (!synchronized_) {
            const auto latest = link_.status(0);
            if (!currentSession(now))
                return;
            checkResult(latest, 0);
            if (latest.phase == Phase::Unknown) {
                protocolFailure(now, "Driver did not report its current configuration");
                return;
            }
            nextRevision_ = std::max(nextRevision_, latest.revision);
            if (latest.phase == Phase::Pending || latest.phase == Phase::RollingBack) {
                // A predecessor may have died mid-approval. Settle its changes
                // before submitting any replacement for the saved full graph.
                if (latest.phase == Phase::Pending && orphanCancellation_ != latest.revision) {
                    const auto error = link_.cancel(latest.revision);
                    if (!currentSession(now))
                        return;
                    if (!error)
                        orphanCancellation_ = latest.revision;
                    else {
                        error_ = error;
                        detail_ = "Predecessor cancellation rejected; retrying before restoration";
                    }
                }
                return;
            }
            synchronized_ = true;
        }
        if (activeGeneration_ == desiredGeneration_) {
            const bool retired =
                std::any_of(pool_.devices().begin(), pool_.devices().end(), [](const auto& device) {
                    return device.region->header().retired.load();
                });
            bool timingChanged = false;
            if (!retired && activation_ && now >= nextTimingInspection_) {
                const auto latencies = activation_->inputLatencyFrames(*desired_);
                nextTimingInspection_ = after(now, timingInspectionIntervalMs_);
                if (!latencies.empty()) {
                    if (latencies.size() != desired_->devices.size())
                        throw std::runtime_error("Renderer returned an incomplete timing plan");
                    for (size_t index = 0; index < desired_->devices.size(); ++index) {
                        const auto& definition = desired_->devices[index];
                        if (!definition.routing.enabled)
                            continue;
                        const auto transport = std::find_if(
                            pool_.devices().begin(), pool_.devices().end(), [&](const auto& value) {
                                return value.id == definition.id;
                            });
                        timingChanged = timingChanged || transport == pool_.devices().end() ||
                                        transport->inputLatencyFrames != latencies[index];
                    }
                }
            }
            if (!retired && !timingChanged)
                return;
            activeGeneration_ = 0;
            if (timingChanged) {
                submission_.restart(now);
                detail_ = "Public IO timing changed; updating the prepared graph";
            } else
                retry(now, kControlFailure, "Active device mapping retired; restoring full intent");
        }
        const auto attempts = submission_.attempts(now);
        if (attempts.empty())
            return;
        const auto& attempt = attempts.front();
        try {
            for (const auto& device : desired_->devices)
                if (device.routing.enabled &&
                    !(link_.sampleRateMask() & virtualSampleRateBit(device.sampleRate)))
                    throw std::runtime_error("Saved sample rate requires a compatible LoopZ update "
                                             "before routing can resume");
            if (nextRevision_ == UINT64_MAX)
                throw std::overflow_error("Driver revision exhausted");
            auto plan = activation_ ? activation_->plan(*desired_) : nullptr;
            if (activation_ && !plan)
                throw std::runtime_error("Renderer did not plan the requested graph");
            auto transports = pool_.prepare(*desired_, plan ? plan->inputLatencyFrames()
                                                            : std::span<const uint32_t>{});
            // Stage every allocation before submit, including the document copy.
            Pending staged{
                *desired_, std::move(transports), desiredGeneration_, ++nextRevision_, {}};
            if (activation_) {
                staged.activation = plan->prepare(staged.transports.devices());
                if (!staged.activation)
                    throw std::runtime_error("Renderer did not prepare the requested graph");
            }
            pending_.emplace(std::move(staged));
            try {
                const auto error = link_.submit(pending_->transports.devices(), pending_->revision);
                if (!currentSession(now))
                    return;
                if (error) {
                    abandonPending();
                    submission_.complete(attempt, false, now);
                    error_ = error;
                    detail_ = "Driver rejected the transition; restoring intent";
                    return;
                }
            } catch (const std::exception& error) {
                if (!currentSession(now))
                    return;
                // Keep tentative mappings pinned. The next ordered query tells
                // us whether the driver accepted before the reply was lost.
                error_ = kControlFailure;
                detail_ = error.what();
            }
            submission_.complete(attempt, true, now);
        } catch (const std::exception& error) {
            abandonPending();
            submission_.complete(attempt, false, now);
            error_ = kControlFailure;
            detail_ = error.what();
        }
    } catch (const std::exception& error) {
        if (!currentSession(now))
            return;
        // A transient status/cancel failure cannot release mappings that the
        // host still borrows, or cause an overlapping replacement submission.
        error_ = kControlFailure;
        detail_ = error.what();
        nextPoll_ = after(now, 1000);
    }
}
void ConfigurationController::retryNow(uint64_t now) noexcept {
    submission_.retryNow(now);
    nextPoll_ = now;
    nextTimingInspection_ = now;
}
RestorationStatus ConfigurationController::status() const {
    auto phase = RestorationPhase::Reconciling;
    if (!desired_)
        phase = RestorationPhase::NoDesiredDocument;
    else if (!session_ || link_.session() != session_)
        phase = RestorationPhase::WaitingForDriver;
    else if (pending_)
        phase =
            pending_->cancelRequested ? RestorationPhase::Cancelling : RestorationPhase::Applying;
    else if (synchronized_ && activeGeneration_ == desiredGeneration_)
        phase = RestorationPhase::Restored;
    else if (error_ || !detail_.empty())
        phase = RestorationPhase::Retry;
    return {
        phase,   desiredGeneration_, activeGeneration_, pending_ ? pending_->revision : 0, error_,
        detail_, recoveredBackup_};
}
} // namespace loopz::macos
