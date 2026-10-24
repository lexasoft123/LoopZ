#include "../../native/platform/macos/desktop/catalog_channel.hpp"
#include <cassert>
#include <chrono>
#include <csignal>
#include <functional>
#include <iostream>
#include <string>
#include <thread>
#include <unistd.h>

using namespace std::chrono_literals;
using loopz::macos::CatalogChannel;
void rejects(const std::function<void()>& operation) {
    bool rejected = false;
    try {
        operation();
    } catch (const std::exception&) {
        rejected = true;
    }
    assert(rejected);
}
int fixture(std::string mode) {
    if (mode == "exit")
        return 0;
    std::string request;
    size_t count = 0;
    while (std::getline(std::cin, request)) {
        if (mode == "hang") {
            std::this_thread::sleep_for(60s);
            return 0;
        }
        if (mode == "partial") {
            std::cout << "incomplete" << std::flush;
            return 0;
        }
        if (mode == "extra") {
            assert(write(STDOUT_FILENO, "one\ntwo\n", 8) == 8);
            return 0;
        }
        if (mode == "large") {
            const std::string block(4096, 'x');
            for (size_t i = 0; i < 8193; ++i)
                if (write(STDOUT_FILENO, block.data(), block.size()) <= 0)
                    return 0;
            continue;
        }
        std::cerr << "fixture stderr is separate\n";
        std::cout << getpid() << ':' << ++count << ':' << request << '\n' << std::flush;
    }
    return 0;
}
int main(int argc, char** argv) {
    if (argc == 3 && std::string(argv[1]) == "--fixture")
        return fixture(argv[2]);
    assert(argc == 1);
    const auto launch = [&](const std::string& mode) {
        return std::vector<std::string>{argv[0], "--fixture", mode};
    };
    CatalogChannel healthy(launch("echo"), 2s);
    const auto first = healthy.exchange("first\n");
    const auto pid = first.substr(0, first.find(':'));
    assert(first == pid + ":1:first");
    assert(healthy.exchange("second\n") == pid + ":2:second");
    rejects([&] {
        healthy.exchange("missing newline");
    });
    rejects([&] {
        healthy.exchange("two\nrequests\n");
    });
    rejects([&] {
        healthy.exchange(std::string(65, 'x') + "\n");
    });
    for (const auto* mode : {"exit", "partial", "extra", "hang", "large"}) {
        const auto begun = std::chrono::steady_clock::now();
        {
            CatalogChannel broken(launch(mode), mode == std::string("large") ? 5s : 250ms);
            rejects([&] {
                broken.exchange("request\n");
            });
            rejects([&] {
                broken.exchange("another\n");
            });
        }
        assert(std::chrono::steady_clock::now() - begun < 7s);
        // A failed channel must never terminate another child owned by its caller.
        assert(healthy.exchange("still alive\n").starts_with(pid + ":"));
    }
    std::puts("Persistent catalog framing, reuse, EOF, failure bounds and child isolation passed");
}
