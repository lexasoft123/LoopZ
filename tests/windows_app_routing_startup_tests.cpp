#include "app_routing_startup.hpp"
#include <windows.h>
#include <objbase.h>
#include <fstream>
#include <iostream>
#include <stdexcept>
using namespace loopz::windows;
namespace {
void require(bool value, const char* message) {
    if (!value)
        throw std::runtime_error(message);
}
template <class F> void rejected(F&& operation, const char* message) {
    bool failed = false;
    try {
        operation();
    } catch (const std::exception&) {
        failed = true;
    }
    require(failed, message);
}
struct Fixture {
    std::filesystem::path root;
    Fixture() {
        GUID id{};
        wchar_t text[40]{};
        require(SUCCEEDED(CoCreateGuid(&id)) && StringFromGUID2(id, text, 40),
                "Startup fixture identity");
        root = std::filesystem::temp_directory_path() / (std::wstring(L"LoopZ-startup-") + text);
        require(std::filesystem::create_directory(root), "Startup fixture directory");
    }
    ~Fixture() {
        std::error_code ignored;
        std::filesystem::remove_all(root, ignored);
    }
};
} // namespace
int main() {
    Fixture fixture;
    const auto missing = fixture.root / L"history";
    unsigned probes = 0;
    auto present = [&] {
        ++probes;
        return true;
    };
    require(!openAppRoutingStore(missing, AppRoutingStorageIntent::RecoverOnly, present) &&
                probes == 0 && !std::filesystem::exists(missing),
            "Passive recovery probed/setup or created missing history");
    rejected(
        [&] {
            openAppRoutingStore(missing, AppRoutingStorageIntent::InitializeForRequest, present);
        },
        "Existing internal device accepted a new baseline");
    require(probes == 1 && !std::filesystem::exists(missing), "Blocked startup mutated storage");
    rejected(
        [&] {
            openAppRoutingStore(missing, AppRoutingStorageIntent::InitializeForRequest,
                                []() -> bool {
                                    throw std::runtime_error("Incomplete device observation");
                                });
        },
        "Incomplete device observation authorized initialization");
    require(!std::filesystem::exists(missing), "Incomplete observation created storage");
    uint64_t revision{};
    {
        auto store =
            openAppRoutingStore(missing, AppRoutingStorageIntent::InitializeForRequest, [&] {
                ++probes;
                return false;
            });
        require(store && store->journal().snapshot().empty(),
                "Fresh saved request did not initialize private history");
        revision = store->journal()
                       .prepare("owned-app", {"console", "", "communications"}, "internal-target")
                       .revision;
    }
    for (const auto intent :
         {AppRoutingStorageIntent::RecoverOnly, AppRoutingStorageIntent::InitializeForRequest}) {
        auto reopened = openAppRoutingStore(missing, intent, []() -> bool {
            throw std::runtime_error("Existing history must not be initialized again");
        });
        const auto all = reopened->journal().snapshot();
        require(all.size() == 1 && all[0].revision == revision &&
                    all[0].original ==
                        std::array<std::string, 3>{"console", "", "communications"} &&
                    all[0].attemptedTargets == std::vector<std::string>{"internal-target"},
                "Reopen replaced durable originals or attempts");
    }
    require(DeleteFileW((missing / L"routing.sqlite3").c_str()) != FALSE,
            "Remove fixture history to model loss");
    rejected(
        [&] {
            openAppRoutingStore(missing, AppRoutingStorageIntent::InitializeForRequest, [] {
                return false;
            });
        },
        "Existing folder with lost history was silently reinitialized");
    require(!std::filesystem::exists(missing / L"routing.sqlite3"), "Lost history was recreated");
    const auto invalid = fixture.root / L"invalid";
    rejected(
        [&] {
            openAppRoutingStore(invalid, static_cast<AppRoutingStorageIntent>(99), present);
        },
        "Invalid startup intent accepted");
    require(!std::filesystem::exists(invalid), "Invalid intent mutated storage");
    std::cout << "First-use history initialization, device/history loss refusal and "
                 "existing-receipt reopening passed\n";
}
