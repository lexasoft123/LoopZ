// LoopZ contributors. MIT license. Non-overlapping resident buffers only.
#pragma once
#if defined(_MSC_VER) && (defined(_M_X64) || defined(_M_IX86))
#include <intrin.h>
#pragma intrinsic(__movsb, __stosb)
#endif
namespace loopz_driver {
inline void copyBytes(unsigned char* destination, const unsigned char* source,
                      unsigned bytes) noexcept {
    if (!bytes)
        return;
#if defined(_MSC_VER) && (defined(_M_X64) || defined(_M_IX86))
    __movsb(destination, source, bytes);
#elif defined(__clang__) || defined(__GNUC__)
    __builtin_memcpy(destination, source, bytes);
#else
    for (unsigned index = 0; index < bytes; ++index)
        destination[index] = source[index];
#endif
}
inline void zeroBytes(unsigned char* destination, unsigned bytes) noexcept {
    if (!bytes)
        return;
#if defined(_MSC_VER) && (defined(_M_X64) || defined(_M_IX86))
    __stosb(destination, 0, bytes);
#elif defined(__clang__) || defined(__GNUC__)
    __builtin_memset(destination, 0, bytes);
#else
    for (unsigned index = 0; index < bytes; ++index)
        destination[index] = 0;
#endif
}
} // namespace loopz_driver
