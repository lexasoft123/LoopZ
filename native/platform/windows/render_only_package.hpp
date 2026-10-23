#pragma once
#include <string_view>
namespace loopz::windows {
// Admit only reviewed packages implementing the render-only adapter record.
// Future versions need an explicit compatibility review before registration.
constexpr bool supportsRenderOnlyPackage(std::wstring_view version) noexcept {
    return version == L"0.4.0.18" || version == L"0.4.0.19" || version == L"0.4.0.20" ||
           version == L"0.4.0.21" || version == L"0.4.0.22" || version == L"0.4.0.23" ||
           version == L"0.4.0.24" || version == L"0.4.0.25";
}
} // namespace loopz::windows
