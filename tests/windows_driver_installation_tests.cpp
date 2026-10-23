// Executes unmodified production method bodies with deterministic PortCls faults.
// This covers stage ordering, rollback and COM references, not kernel/PnP behavior.
#include <array>
#include <cstdint>
#include <cstdlib>
#include <iostream>
#include <tuple>
#include <vector>

using NTSTATUS = int32_t;
constexpr NTSTATUS STATUS_SUCCESS = 0;
constexpr NTSTATUS failure = -101;
constexpr NTSTATUS notFound = -102;
constexpr NTSTATUS optionalUnavailable = -103;
#ifndef _In_
#define _In_
#endif
#ifndef _In_opt_
#define _In_opt_
#endif
#ifndef _Out_opt_
#define _Out_opt_
#endif
#define PAGED_CODE() ((void)0)
#define DPF_ENTER(...) ((void)0)
#define UNREFERENCED_PARAMETER(value) ((void)(value))
#define NT_SUCCESS(status) ((status) >= 0)
#define IF_FAILED_JUMP(status, label)                                                              \
    do {                                                                                           \
        if (!NT_SUCCESS(status))                                                                   \
            goto label;                                                                            \
    } while (false)
#define SAFE_RELEASE(pointer)                                                                      \
    do {                                                                                           \
        if (pointer) {                                                                             \
            (pointer)->Release();                                                                  \
            (pointer) = nullptr;                                                                   \
        }                                                                                          \
    } while (false)
using BOOL = bool;
constexpr BOOL TRUE = true, FALSE = false;
constexpr int CLSID_PortTopology = 1, CLSID_PortWaveRT = 2;
constexpr int IID_IPortTopology = 3, IID_IPortWaveRT = 4, IID_IPortClsEtwHelper = 5;
void require(bool condition, const char* message) {
    if (!condition) {
        std::cerr << message << '\n';
        std::exit(1);
    }
}
struct Unknown {
    int references{};
    Unknown* helper{};
    bool optionalAvailable{};
    void AddRef() {
        ++references;
    }
    void Release() {
        require(references > 0, "COM reference underflow");
        --references;
    }
    NTSTATUS QueryInterface(int iid, void** output) {
        require(iid == IID_IPortClsEtwHelper, "Unexpected optional interface query");
        *output = nullptr;
        if (!optionalAvailable)
            return optionalUnavailable;
        helper->AddRef();
        *output = helper;
        return STATUS_SUCCESS;
    }
};
using PUNKNOWN = Unknown*;
using PPORTCLSETWHELPER = Unknown*;
using PVOID = void*;
using PIRP = void*;
using PDEVICE_OBJECT = void*;
struct MiniPair {
    int TopoName{0}, WaveName{1};
    int TemplateTopoName{}, TemplateWaveName{};
    int TopoCreateCallback{}, WaveCreateCallback{};
    int TopoInterfacePropertyCount{}, WaveInterfacePropertyCount{};
    void* TopoInterfaceProperties{};
    void* WaveInterfaceProperties{};
    void* PhysicalConnections{};
    int PhysicalConnectionCount{};
};
using PENDPOINT_MINIPAIR = MiniPair*;
enum class Stage { None, TopologyInstall, TopologyCache, WaveInstall, WaveCache, Connect };
struct CAdapterCommon {
    Stage fault{Stage::None};
    bool partialFailure{};
    std::array<Unknown, 2> ports{}, minis{};
    Unknown helper;
    std::array<bool, 2> cached{}, registered{};
    std::vector<Stage> calls;
    int helperRegistrations{};
    CAdapterCommon() {
        ports[1].helper = &helper;
    }
    void seed(int index) {
        cached[index] = registered[index] = true;
        ports[index].AddRef();
        minis[index].AddRef();
    }
    NTSTATUS GetCachedSubdevice(int index, PUNKNOWN* port, PUNKNOWN* mini) {
        if (!cached[index])
            return notFound;
        (*port = &ports[index])->AddRef();
        (*mini = &minis[index])->AddRef();
        return STATUS_SUCCESS;
    }
    template <typename... Args> NTSTATUS InstallSubdevice(Args... arguments) {
        auto values = std::make_tuple(arguments...);
        const int index = std::get<1>(values);
        const Stage stage = index ? Stage::WaveInstall : Stage::TopologyInstall;
        calls.push_back(stage);
        if (fault == stage && !partialFailure)
            return failure;
        auto outputPort = std::get<sizeof...(Args) - 2>(values);
        auto outputMini = std::get<sizeof...(Args) - 1>(values);
        (*outputPort = &ports[index])->AddRef();
        registered[index] = true;
        if (fault == stage)
            return failure;
        (*outputMini = &minis[index])->AddRef();
        return STATUS_SUCCESS;
    }
    NTSTATUS CacheSubdevice(int index, PUNKNOWN port, PUNKNOWN mini) {
        const Stage stage = index ? Stage::WaveCache : Stage::TopologyCache;
        calls.push_back(stage);
        if (fault == stage)
            return failure;
        require(port == &ports[index] && mini == &minis[index], "Wrong cache objects");
        cached[index] = true;
        port->AddRef();
        mini->AddRef();
        return STATUS_SUCCESS;
    }
    NTSTATUS ConnectTopologies(PUNKNOWN topology, PUNKNOWN wave, void*, int) {
        require(topology == &ports[0] && wave == &ports[1], "Wrong physical connection");
        calls.push_back(Stage::Connect);
        return fault == Stage::Connect ? failure : STATUS_SUCCESS;
    }
    void UnregisterSubdevice(PUNKNOWN port) {
        const int index = port == &ports[1];
        require(port == &ports[index] && registered[index], "Wrong rollback object");
        registered[index] = false;
    }
    void RemoveCachedSubdevice(int index) {
        if (cached[index]) {
            ports[index].Release();
            minis[index].Release();
            cached[index] = false;
        }
    }
    void SetEtwHelper(PPORTCLSETWHELPER value) {
        require(value == &helper, "Wrong ETW helper");
        ++helperRegistrations;
    }
    NTSTATUS InstallEndpointFilters(PIRP, PENDPOINT_MINIPAIR, PVOID, PUNKNOWN*, PUNKNOWN*,
                                    PUNKNOWN*, PUNKNOWN*);
};
using PADAPTERCOMMON = CAdapterCommon*;
#include "windows_driver_installation_methods.inc"

int main() {
    MiniPair pair;
    for (const Stage fault : {Stage::TopologyInstall, Stage::TopologyCache, Stage::WaveInstall,
                              Stage::WaveCache, Stage::Connect}) {
        for (int seed = 0; seed < 4; ++seed) {
            // A stage already satisfied by the cache cannot be fault-injected.
            if (((seed & 1) &&
                 (fault == Stage::TopologyInstall || fault == Stage::TopologyCache)) ||
                ((seed & 2) && (fault == Stage::WaveInstall || fault == Stage::WaveCache)))
                continue;
            for (bool partial : {false, true}) {
                CAdapterCommon adapter;
                adapter.fault = fault;
                adapter.partialFailure = partial;
                for (int index = 0; index < 2; ++index)
                    if (seed & (1 << index))
                        adapter.seed(index);
                std::array<PUNKNOWN, 4> output{};
                const auto status = adapter.InstallEndpointFilters(
                    nullptr, &pair, nullptr, &output[0], &output[1], &output[2], &output[3]);
                require(status == failure, "Installation failure was masked");
                for (auto value : output)
                    require(!value, "Failure published an endpoint interface");
                require(!adapter.calls.empty() && adapter.calls.back() == fault,
                        "Installation continued after a failed stage");
                for (int index = 0; index < 2; ++index) {
                    const bool existing = (seed & (1 << index)) != 0;
                    require(adapter.cached[index] == existing &&
                                adapter.registered[index] == existing,
                            "Rollback removed a cached endpoint or leaked a new one");
                    require(adapter.ports[index].references == int(existing) &&
                                adapter.minis[index].references == int(existing),
                            "Rollback leaked endpoint interfaces");
                }
            }
        }
    }
    for (int seed = 0; seed < 4; ++seed) {
        CAdapterCommon adapter;
        for (int index = 0; index < 2; ++index)
            if (seed & (1 << index))
                adapter.seed(index);
        std::array<PUNKNOWN, 4> output{};
        require(adapter.InstallEndpointFilters(nullptr, &pair, nullptr, &output[0], &output[1],
                                               &output[2], &output[3]) == STATUS_SUCCESS,
                "Healthy installation failed");
        require(output[0] == &adapter.ports[0] && output[1] == &adapter.ports[1] &&
                    output[2] == &adapter.minis[0] && output[3] == &adapter.minis[1],
                "Healthy installation returned wrong interfaces");
        for (auto value : output)
            value->Release();
        for (int index = 0; index < 2; ++index)
            require(adapter.ports[index].references == 1 && adapter.minis[index].references == 1,
                    "Healthy installation leaked interface references");
    }
    for (const Stage fault : {Stage::None, Stage::TopologyInstall, Stage::TopologyCache,
                              Stage::WaveInstall, Stage::WaveCache, Stage::Connect}) {
        for (bool optional : {false, true}) {
            CAdapterCommon adapter;
            adapter.fault = fault;
            adapter.ports[1].optionalAvailable = optional;
            require(InstallEndpointRenderFilters(nullptr, nullptr, &adapter, &pair) ==
                        (fault == Stage::None ? STATUS_SUCCESS : failure),
                    "Optional PortCls query changed installation result");
            require(adapter.helperRegistrations == int(fault == Stage::None && optional) &&
                        adapter.helper.references == 0,
                    "ETW helper ownership or failure handling is wrong");
            for (int index = 0; index < 2; ++index) {
                const int expected = fault == Stage::None ? 1 : 0;
                require(adapter.ports[index].references == expected &&
                            adapter.minis[index].references == expected,
                        "Render startup leaked endpoint interfaces");
            }
        }
    }
    std::cout << "Driver installation faults, rollback, cached ownership and optional ETW passed\n";
}
