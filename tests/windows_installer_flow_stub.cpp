// Isolated installer-flow fixture; never installs services or operates audio.
#include <windows.h>
#include <filesystem>
#include <fstream>
#include <array>
#include <string_view>
int wmain(int argc, wchar_t** argv) {
    std::array<wchar_t, 32768> module{};
    const auto count =
        GetModuleFileNameW(nullptr, module.data(), static_cast<DWORD>(module.size()));
    if (!count || count >= module.size())
        return 77;
    const std::filesystem::path file(std::wstring(module.data(), count));
    if (file.filename() == L"WebView2RuntimeInstaller.exe") {
        if (argc != 3 || std::wstring_view(argv[1]) != L"/silent" ||
            std::wstring_view(argv[2]) != L"/install")
            return 81;
        std::array<wchar_t, 32768> fixture{};
        const auto size = GetEnvironmentVariableW(
            L"LOOPZ_INSTALLER_RUNTIME_FIXTURE", fixture.data(), static_cast<DWORD>(fixture.size()));
        if (!size || size >= fixture.size())
            return 82;
        const std::filesystem::path root(std::wstring(fixture.data(), size));
        std::ofstream(root / L"runtime-called.txt") << "/silent /install";
        int result = 83;
        std::ifstream(root / L"runtime-result-code.txt") >> result;
        if (result == 0 && std::filesystem::exists(root / L"publish-runtime.txt"))
            std::ofstream(root / L"runtime-version.txt") << "140.0.0.0";
        return result;
    }
    if (file.filename() == L"LoopZ.exe") {
        std::ofstream(file.parent_path() / L"app-launched.txt") << "fixture";
        return 0;
    }
    if (argc != 2)
        return 78;
    const auto command = std::wstring_view(argv[1]);
    const bool uninstall = command == L"--uninstall";
    if (!uninstall && command != L"--install-service")
        return 78;
    std::ofstream marker(file.parent_path() /
                         (uninstall ? L"uninstall-called.txt" : L"helper-called.txt"));
    marker << (uninstall ? "--uninstall" : "--install-service");
    if (!marker)
        return 79;
    int result = 80;
    std::ifstream(file.parent_path() /
                  (uninstall ? L"uninstall-result-code.txt" : L"result-code.txt")) >>
        result;
    return result;
}
