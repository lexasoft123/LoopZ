#include "broker_binary.hpp"
#include <iostream>
#include <stdexcept>
int main() {
    using namespace loopz::windows;
    const auto directory = brokerServiceDirectory();
    if (!directory.is_absolute() || directory.filename() != L"LoopZDeviceBroker")
        throw std::runtime_error("Unexpected service installation directory");
    bool rejected = false;
    try {
        BrokerBinaryGuard guard;
    } catch (const std::runtime_error&) {
        rejected = true;
    }
    if (!rejected)
        throw std::runtime_error("Build-directory fixture was accepted as an installed broker");
    std::cout << "PASS: uninstalled build-directory executable rejected\n";
}
