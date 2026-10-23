#pragma once
#include "broker_reconcile.hpp"
#include <filesystem>
#include <string_view>

namespace loopz::windows {
// Blocking service runtime. All settings are supplied by trusted installation
// code, never IPC. The borrowed stop event must be manual-reset and outlive this
// call. Each control owner opens its own protected SQLite connection. The
// adapter belongs exclusively to the background worker until this call returns.
void runBrokerRuntime(const std::filesystem::path& storeRoot, std::wstring_view pipeName,
                      BrokerDeviceAdapter& adapter, HANDLE stopEvent,
                      BrokerReconciler::Progress progress = {}, std::function<void()> ready = {});
} // namespace loopz::windows
