#include "desktop.hpp"

#include <cstdio>

int loopzPrintRoutingStatus() {
    if (!loopzValidateDesktopBundle())
        return 3;
    try {
        loopz::macos::EditorServiceClient client(loopz::macos::productionEditorEndpoint());
        client.connect();
        const auto snapshot = client.snapshot();
        if (std::fwrite(snapshot.data(), 1, snapshot.size(), stdout) != snapshot.size() ||
            std::putchar('\n') == EOF)
            throw std::runtime_error("Routing status could not be written");
        return 0;
    } catch (const std::exception& error) {
        std::fprintf(stderr, "LoopZ routing status: %s\n", error.what());
        return 1;
    }
}
