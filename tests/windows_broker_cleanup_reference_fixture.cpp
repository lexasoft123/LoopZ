// Opt-in administrative fixture, called only with the outer harness's new store.
// Adds a durable reference to an existing microphone without operating that device.
#include "device_intent_store.hpp"
#include "driver_identity.hpp"
#include "device_operations.hpp"
#include <devguid.h>
#include <stdexcept>
#include <iostream>
using namespace loopz;
using namespace loopz::windows;
int main(int argc, char** argv) {
    if (argc != 3)
        throw std::runtime_error("Expected fixture mode and existing test reference ID");
    const auto owner = currentOwnerSid();
    const std::string id(argv[2]);
    constexpr auto operation = "12345678-1234-1234-1234-123456789abc";
    const std::string_view mode(argv[1]);
    if (mode == "--seed" || mode == "--seed-missing") {
        const auto devices = SetupDiCreateDeviceInfoList(&GUID_DEVCLASS_MEDIA, nullptr);
        SP_DEVINFO_DATA device{sizeof(device)};
        const bool present =
            devices != INVALID_HANDLE_VALUE &&
            SetupDiOpenDeviceInfoW(devices, driverInstance(driverMixKey(owner, id)).c_str(),
                                   nullptr, 0, &device);
        const auto error = GetLastError();
        if (devices != INVALID_HANDLE_VALUE)
            SetupDiDestroyDeviceInfoList(devices);
        if (mode == "--seed" ? !present : (present || error != ERROR_NO_SUCH_DEVINST))
            throw std::runtime_error("Microphone reference does not match fixture precondition");
        DeviceIntentStore store;
        if (!store.ownersWithDesiredState().empty())
            throw std::runtime_error("Store is not an empty fixture");
        DeviceIntentRequest request;
        request.operationId = operation;
        request.mixId = id;
        request.name = "Cleanup guard fixture only";
        if (store.accept(owner, request) != 1)
            throw std::runtime_error("Cannot seed reference guard fixture");
    } else if (mode == "--clear-fixture") {
        {
            DeviceIntentStore store;
            const auto owners = store.ownersWithDesiredState();
            const auto snapshot = store.snapshot(owner);
            if (owners.size() != 1 || owners.front() != owner || snapshot.revision != 1 ||
                snapshot.records.size() != 1 || snapshot.records.front().mixId != id ||
                snapshot.records.front().operationId != operation ||
                snapshot.records.front().name != "Cleanup guard fixture only")
                throw std::runtime_error("Refusing to clear non-fixture desired state");
        }
        // Only the precise fixture record above is eligible for teardown. The
        // referenced physical microphone is never modified, removed or claimed.
        if (!DeleteFileW((deviceIntentStoreRoot() / L"setup.sqlite3").c_str()))
            throw std::runtime_error("Cannot remove own fixture database");
    } else
        throw std::runtime_error("Unknown reference fixture mode");
    std::cout << "PASS: scoped reference fixture " << mode << '\n';
}
