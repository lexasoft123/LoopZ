#include "broker_runtime.hpp"
#include "broker_handler.hpp"
#include <condition_variable>
#include <exception>
#include <mutex>
#include <thread>
#include <array>
#include <stdexcept>

namespace loopz::windows {
namespace {
struct Event {
    HANDLE value{CreateEventW(nullptr, TRUE, TRUE, nullptr)};
    Event() {
        if (!value)
            throw std::runtime_error("Cannot create setup reconciliation event");
    }
    ~Event() {
        CloseHandle(value);
    }
};
} // namespace
void runBrokerRuntime(const std::filesystem::path& root, std::wstring_view pipeName,
                      BrokerDeviceAdapter& adapter, HANDLE stop,
                      BrokerReconciler::Progress progress, std::function<void()> started) {
    const auto stopped = WaitForSingleObject(stop, 0);
    if (stopped == WAIT_FAILED)
        throw std::invalid_argument("Invalid setup runtime stop event");
    if (stopped == WAIT_OBJECT_0)
        return;
    DeviceIntentStore requests(root);
    Event changed;
    BrokerPipe pipe(stop, pipeName);
    std::mutex mutex;
    std::condition_variable ready;
    bool initialized = false;
    std::exception_ptr workerError;
    std::map<std::pair<std::wstring, std::string>, BrokerDeviceProgress> states;
    std::jthread worker([&] {
        try {
            DeviceIntentStore store(root);
            BrokerReconciler reconcile(store, adapter, stop, [&](const auto& update) {
                {
                    std::lock_guard guard(mutex);
                    const auto key = std::make_pair(
                        update.owner, deviceIntentStorageKey(update.kind, update.mixId));
                    if (update.phase == BrokerDevicePhase::Removed)
                        states.erase(key);
                    else
                        states[key] = update;
                }
                if (progress)
                    progress(update);
            });
            {
                std::lock_guard guard(mutex);
                initialized = true;
            }
            ready.notify_one();
            const std::array<HANDLE, 2> events{stop, changed.value};
            while (WaitForSingleObject(stop, 0) != WAIT_OBJECT_0) {
                // Reset before reading desired state. A commit during this pass
                // keeps the event set, so the following wait cannot lose it.
                ResetEvent(changed.value);
                const auto retry = reconcile.pass();
                const auto result =
                    WaitForMultipleObjects(static_cast<DWORD>(events.size()), events.data(), FALSE,
                                           retry ? 5000 : INFINITE);
                if (result == WAIT_OBJECT_0)
                    break;
                if (result != WAIT_OBJECT_0 + 1 && result != WAIT_TIMEOUT)
                    throw std::runtime_error("Setup reconciliation wait failed");
            }
        } catch (...) {
            {
                std::lock_guard guard(mutex);
                workerError = std::current_exception();
            }
            SetEvent(stop);
            ready.notify_one();
        }
    });
    // Always signal before joining, including an exception during pipe binding
    // or startup. The worker never outlives captured stores, callbacks or events.
    struct Join {
        HANDLE stop;
        std::jthread& worker;
        ~Join() {
            SetEvent(stop);
            if (worker.joinable())
                worker.join();
        }
    } join{stop, worker};
    {
        std::unique_lock guard(mutex);
        ready.wait(guard, [&] {
            return initialized || workerError;
        });
        if (workerError)
            std::rethrow_exception(workerError);
    }
    const BrokerProgressQuery query =
        [&](std::wstring_view owner, std::string_view mix,
            DeviceIntentKind kind) -> std::optional<BrokerDeviceProgress> {
        std::lock_guard guard(mutex);
        const auto found = states.find({std::wstring(owner), deviceIntentStorageKey(kind, mix)});
        if (found == states.end())
            return {};
        return found->second;
    };
    if (started)
        started();
    runBrokerRequests(pipe, requests, changed.value, query);
    SetEvent(stop);
    worker.join();
    std::lock_guard guard(mutex);
    if (workerError)
        std::rethrow_exception(workerError);
}
} // namespace loopz::windows
