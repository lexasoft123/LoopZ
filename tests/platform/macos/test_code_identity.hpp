#pragma once
#include <Security/Security.h>
#include <cassert>
#include <string>

inline std::string selfCodeRequirement() {
    SecCodeRef code = nullptr;
    SecStaticCodeRef file = nullptr;
    CFDictionaryRef information = nullptr;
    assert(SecCodeCopySelf(kSecCSDefaultFlags, &code) == errSecSuccess);
    assert(SecCodeCopyStaticCode(code, kSecCSDefaultFlags, &file) == errSecSuccess);
    assert(SecCodeCopySigningInformation(file, kSecCSSigningInformation, &information) ==
           errSecSuccess);
    const auto hash = static_cast<CFDataRef>(CFDictionaryGetValue(information, kSecCodeInfoUnique));
    assert(hash && CFGetTypeID(hash) == CFDataGetTypeID());
    constexpr char digits[] = "0123456789abcdef";
    std::string requirement = "cdhash H\"";
    for (CFIndex index = 0; index < CFDataGetLength(hash); ++index) {
        const auto byte = CFDataGetBytePtr(hash)[index];
        requirement += digits[byte >> 4];
        requirement += digits[byte & 15];
    }
    requirement += '"';
    CFRelease(information);
    CFRelease(file);
    CFRelease(code);
    return requirement;
}
