#pragma once
#include <loopz/callback_timing.hpp>

#include <loopz/macos/transport_message.hpp>
#include <loopz/macos/driver_configuration.hpp>
#include <loopz/macos/driver_timing.hpp>
#include <CoreAudio/AudioServerPlugIn.h>
#include <mach/mach_time.h>

#include <array>
#include <atomic>
#include <chrono>
#include <memory>
#include <mutex>
#include <unordered_map>

namespace loopz::macos::hal {

inline constexpr uint32_t kClockPeriodFrames = 16384;
inline constexpr size_t kMaximumRetiredDevices = 128;
inline constexpr size_t kMaximumObjects = kMaximumVirtualDevices + kMaximumRetiredDevices;

class Text {
  public:
    explicit Text(const std::string& value);
    ~Text();
    Text(const Text&) = delete;
    Text& operator=(const Text&) = delete;
    CFStringRef get() const noexcept;

  private:
    CFStringRef value_;
};

struct ClockSnapshot {
    uint64_t anchorTicks;
    uint64_t seed;
};

struct DeviceLifecycle {
    std::atomic<bool> alive{true};
    std::atomic<uint32_t> running{0};
    // High 32 bits identify the observed IO session, low 32 bits retain its
    // largest WriteMix period. One serial playback callback writer; control
    // resets it only when the first client starts a new session.
    std::atomic<uint64_t> writeTiming{0};
    CallbackTimingObservation callbackTiming;
    // Accessed only while State::mutex_ is held; callbacks use running instead.
    // A registered HAL client can hold multiple IO/timing references. An idle
    // registration retains the device identity but does not keep IO running.
    std::unordered_map<UInt32, uint32_t> clients;
    bool removalNoticePending{false};
    uint64_t clockSeed{1};
    uint64_t rateRequestSequence{0};
    uint32_t requestedRate{0};
};

struct Runtime {
    explicit Runtime(RemoteDeviceTransport transport, mach_timebase_info_data_t timebase,
                     std::shared_ptr<DeviceLifecycle> clients = nullptr);
    RemoteDeviceTransport transport;
    const DeviceAudioView audio;
    const mach_timebase_info_data_t timebase;
    const std::shared_ptr<DeviceLifecycle> lifecycle;
    std::atomic<bool> configurationMuted{false};
    std::atomic<uint64_t> clockRevision{0}, anchorTicks{0}, seed{1};
    bool restartClock() noexcept;
    // Control owner holds State::mutex_. Rollback may restore a Runtime
    // retained by an older IO table, so clock readers remain synchronized.
    void inheritClock(const Runtime& previous) noexcept;
    bool clock(ClockSnapshot& result) const noexcept;
    bool frameAt(double sampleTime, uint64_t& frame) const noexcept;
    uint64_t toNs(uint64_t ticks) const noexcept;
    uint64_t toTicks(uint64_t ns) const noexcept;
};

struct Device {
    Device(AudioObjectID object, std::string name, std::shared_ptr<Runtime> runtime,
           bool hasOutput);
    const AudioObjectID object, input, output;
    const std::string label;
    const Text name, uid;
    const std::shared_ptr<Runtime> runtime;
    const bool hasOutput;
};

class State {
  public:
    State();
    OSStatus initialize(AudioServerPlugInHostRef host);
    void setServiceAvailable(bool available);
    void setSampleRateMask(uint32_t mask);
    OSStatus apply(DeviceTransportSnapshot snapshot);
    OSStatus performChange(AudioObjectID device, uint64_t action);
    OSStatus abortChange(AudioObjectID device, uint64_t action);
    OSStatus cancelConfiguration(uint64_t revision);
    DriverConfigurationResult configurationResult(uint64_t revision);
    std::vector<DriverRateRequest> rateRequests();
    OSStatus acknowledgeRates(std::span<const DriverRateRequest> requests);
    OSStatus addClient(AudioObjectID device, const AudioServerPlugInClientInfo* client);
    OSStatus removeClient(AudioObjectID device, const AudioServerPlugInClientInfo* client);
    OSStatus start(AudioObjectID device, UInt32 client);
    OSStatus stop(AudioObjectID device, UInt32 client);

    bool has(AudioObjectID object, const AudioObjectPropertyAddress& address);
    OSStatus settable(AudioObjectID object, const AudioObjectPropertyAddress& address,
                      Boolean& result);
    OSStatus set(AudioObjectID object, const AudioObjectPropertyAddress& address, UInt32 bytes,
                 const void* data);
    OSStatus size(AudioObjectID object, const AudioObjectPropertyAddress& address, UInt32& bytes);
    OSStatus get(AudioObjectID object, const AudioObjectPropertyAddress& address,
                 UInt32 qualifierBytes, const void* qualifier, UInt32 capacity, UInt32& bytes,
                 void* output);

    // All IO calls use a bounded table lookup and borrow raw pointers. They do
    // not acquire mutexes, manipulate shared ownership or reclaim memory.
    OSStatus stamp(AudioObjectID device, Float64* sample, UInt64* host, UInt64* seed) noexcept;
    OSStatus willDo(AudioObjectID device, UInt32 operation, Boolean* willDo,
                    Boolean* inPlace) noexcept;
    OSStatus io(AudioObjectID device, AudioObjectID stream, UInt32 operation, UInt32 frames,
                const AudioServerPlugInIOCycleInfo* cycle, void* buffer) noexcept;
    bool knownDevice(AudioObjectID device) noexcept;

  private:
    struct Table {
        std::vector<std::shared_ptr<Device>> owners;
        std::array<Device*, kMaximumObjects> records{};
        size_t count{0};
    };
    struct Notices {
        std::vector<std::shared_ptr<Device>> removed, renamed;
        bool deviceList{false};
    };
    struct CatalogPlan {
        std::vector<std::shared_ptr<Device>> devices, dead;
        std::unique_ptr<Table> table;
        Notices notices;
    };
    struct Change {
        std::shared_ptr<Device> before, after;
        uint64_t forwardAction, rollbackAction;
    };
    struct Rebinding {
        std::shared_ptr<Device> before, after;
    };
    struct Transaction {
        uint64_t revision;
        AudioObjectID nextObject;
        DriverConfigurationPhase phase{DriverConfigurationPhase::Pending};
        OSStatus error{noErr};
        std::vector<Change> changes;
        std::vector<Rebinding> rebindings;
        std::vector<CatalogPlan> forward, rollback, quarantine;
        CatalogPlan final;
        Notices aborted;
        size_t applied{0};
        uint64_t waitingAction{0};
        std::chrono::steady_clock::time_point deadline;
    };
    struct Control;
    static void destroyControl(Control* control) noexcept;
    using ControlPtr = std::unique_ptr<Control, decltype(&destroyControl)>;
    class Borrow {
      public:
        explicit Borrow(State& state) noexcept;
        ~Borrow();
        Borrow(const Borrow&) = delete;
        Borrow& operator=(const Borrow&) = delete;
        Device* device(AudioObjectID object) const noexcept;

      private:
        State& state_;
        const Table* table_;
    };
    Device* findLocked(AudioObjectID object) const noexcept;
    std::unique_ptr<Table> prepareTable(const std::vector<std::shared_ptr<Device>>& active,
                                        const std::vector<std::shared_ptr<Device>>& dead);
    void publish(std::unique_ptr<Table> table) noexcept;
    void collectDeadLocked() noexcept;
    ControlPtr makeControl();
    void queueRequest(AudioObjectID device, uint64_t action);
    OSStatus requestFailed(AudioObjectID device, uint64_t action, OSStatus error);
    void expireConfiguration();
    uint32_t outstandingRequests() const noexcept;
    CatalogPlan plan(std::vector<std::shared_ptr<Device>> devices,
                     std::vector<std::shared_ptr<Device>> dead);
    void install(CatalogPlan& plan) noexcept;
    void broadcast(Notices notices);
    void remember(DriverConfigurationResult result) noexcept;
    void finishLocked(DriverConfigurationPhase phase, Notices& notices) noexcept;
    void failLocked(OSStatus error, Notices& notices, AudioObjectID& device,
                    uint64_t& action) noexcept;
    void notify(AudioServerPlugInHostRef host, AudioObjectID object,
                AudioObjectPropertySelector selector,
                AudioObjectPropertyScope scope = kAudioObjectPropertyScopeGlobal) const noexcept;

    std::mutex mutex_;
    std::mutex publicationMutex_;
    AudioServerPlugInHostRef host_{nullptr};
    mach_timebase_info_data_t timebase_{};
    std::vector<std::shared_ptr<Device>> devices_, dead_;
    std::unique_ptr<Table> current_;
    std::vector<std::unique_ptr<Table>> retired_;
    std::atomic<Table*> table_{nullptr};
    std::atomic<uint32_t> readers_{0};
    std::atomic<bool> serviceAvailable_{true};
    uint32_t sampleRateMask_{kAllVirtualSampleRates};
    ControlPtr control_{nullptr, destroyControl};
    std::unique_ptr<Transaction> pending_;
    std::array<DriverConfigurationResult, 32> outcomes_{};
    size_t nextOutcome_{0};
    bool haveOutcome_{false};
    AudioObjectID nextObject_{16};
    uint64_t revision_{0};
    uint64_t highestRequestedRevision_{0}, nextAction_{1};
    uint64_t nextRateRequest_{1};
    std::string rateOrigin_;
};

State& state();
void startService();

} // namespace loopz::macos::hal
