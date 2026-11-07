#include <loopz/runtime/control_socket.hpp>
#include <cstdlib>
#include <iostream>
int main(int argc, char** argv) {
    try {
        std::filesystem::path socket, state;
        for (int i = 1; i < argc; ++i) {
            std::string option = argv[i];
            if ((option == "--socket" || option == "--state") && i + 1 < argc) {
                (option == "--socket" ? socket : state) = argv[++i];
            } else if (option == "--help") {
                std::cout << "loopz-engine [--socket PATH] [--state PATH]\n";
                return 0;
            } else
                throw std::runtime_error("Unknown or incomplete Engine option");
        }
        if (socket.empty())
            socket = loopz::runtime::defaultSocketPath();
        if (state.empty()) {
            const auto* config = std::getenv("XDG_CONFIG_HOME");
            const auto* home = std::getenv("HOME");
            if ((!config || !*config) && (!home || !*home))
                throw std::runtime_error("HOME or XDG_CONFIG_HOME is required");
            state = (config && *config ? std::filesystem::path(config)
                                       : std::filesystem::path(home) / ".config") /
                    "loopz/engine.json";
        }
        return loopz::runtime::serveSocket(socket, state);
    } catch (const std::exception& error) {
        std::cerr << error.what() << '\n';
        return 1;
    }
}
