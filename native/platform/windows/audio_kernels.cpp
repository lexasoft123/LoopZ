#include "audio_kernels.hpp"
#include "cached_fir_kernels.hpp"
#include <windows.h>
#if defined(LOOPZ_WINDOWS_FIR_AVX2)
namespace loopz::detail {
std::array<double, 2> cachedFirDotPairAvx2(const float*, const float*, const double*,
                                           uint32_t) noexcept;
}
#endif
namespace loopz::windows {
bool prepareAudioKernels() noexcept {
    detail::CachedFirPairKernel kernel = nullptr;
#if defined(LOOPZ_WINDOWS_FIR_AVX2)
    // Unsupported/older systems keep the baseline implementation. SDK feature
    // query documentation: https://learn.microsoft.com/windows/win32/api/processthreadsapi/nf-processthreadsapi-isprocessorfeaturepresent
    static const auto available =
        IsProcessorFeaturePresent(PF_AVX2_INSTRUCTIONS_AVAILABLE) != FALSE;
    if (available)
        kernel = &detail::cachedFirDotPairAvx2;
#endif
    detail::setCachedFirPairKernel(kernel);
    return kernel != nullptr;
}
} // namespace loopz::windows
