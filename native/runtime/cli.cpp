#include <loopz/runtime/control_socket.hpp>
#include <loopz/document_json.hpp>
#include <iostream>
int main(int argc, char** argv) {
    try {
        std::filesystem::path socket;
        std::string request = R"({"version":1,"operation":"state"})";
        for (int i = 1; i < argc; ++i) {
            std::string option = argv[i];
            if (option == "--socket" && i + 1 < argc)
                socket = argv[++i];
            else if (option == "--request" && i + 1 < argc)
                request = argv[++i];
            else if (option == "--stdin")
                std::getline(std::cin, request);
            else if (option == "--help") {
                std::cout << "loopzctl [--socket PATH] [--request JSON | --stdin]\n";
                return 0;
            } else
                throw std::runtime_error("Unknown or incomplete CLI option");
        }
        if (socket.empty())
            socket = loopz::runtime::defaultSocketPath();
        const auto response =
            loopz::runtime::requestSocket(socket, loopz::parseControlJSON(request).dump());
        std::cout << response << '\n';
        const auto parsed =
            loopz::parseControlJSON(response, loopz::runtime::kMaximumControlResponseBytes);
        return parsed.value("ok", true) ? 0 : 2;
    } catch (const std::exception& error) {
        std::cerr << error.what() << '\n';
        return 1;
    }
}
