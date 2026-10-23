#pragma once
namespace loopz_driver {
// Stable, per-root PortCls reference names. Cache keys and INF template names
// stay separate from the registered name. No allocation or kernel dependencies.
inline bool endpointReference(const wchar_t* instance, const wchar_t* filter,
                              wchar_t* output, unsigned capacity) noexcept {
    if (!output || !capacity)
        return false;
    output[0] = 0;
    if (!instance || !filter)
        return false;
    const auto upper = [](wchar_t c) { return c >= L'a' && c <= L'z' ? c - L'a' + L'A' : c; };
    const auto allowed = [](wchar_t c) {
        return (c >= L'A' && c <= L'Z') || (c >= L'a' && c <= L'z') ||
               (c >= L'0' && c <= L'9') || c == L'_';
    };
    constexpr wchar_t prefix[] = L"ROOT\\LOOPZAUDIO\\";
    unsigned begin = 0;
    for (; prefix[begin]; ++begin)
        if (!instance[begin] || upper(instance[begin]) != prefix[begin])
            return false;
    unsigned suffix = 0, name = 0;
    while (suffix < 128 && instance[begin + suffix]) {
        if (!allowed(instance[begin + suffix])) return false;
        ++suffix;
    }
    while (name < 32 && filter[name]) {
        if (!allowed(filter[name])) return false;
        ++name;
    }
    if (!suffix || suffix == 128 || !name || name == 32 || capacity <= 7 + name + suffix)
        return false;
    unsigned offset = 0;
    for (wchar_t c : L"LoopZ-") {
        if (c) output[offset++] = c;
    }
    for (unsigned i = 0; i < name; ++i) output[offset++] = filter[i];
    output[offset++] = L'-';
    for (unsigned i = 0; i < suffix; ++i) output[offset++] = static_cast<wchar_t>(upper(instance[begin + i]));
    output[offset] = 0;
    return true;
}
} // namespace loopz_driver
