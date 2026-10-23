#include "cached_fir_kernels.hpp"
#include <immintrin.h>
namespace loopz::detail {
// Compiled separately with AVX2 and strict floating point. Four independent
// tap lanes retain the SSE2 accumulator and final reduction order; no FMA.
std::array<double, 2> cachedFirDotPairAvx2(const float* a, const float* b, const double* weights,
                                           uint32_t count) noexcept {
    auto first = _mm256_setzero_pd(), second = _mm256_setzero_pd();
    uint32_t tap = 0;
    for (; tap + 4 <= count; tap += 4) {
        const auto w = _mm256_loadu_pd(weights + tap);
        first = _mm256_add_pd(first, _mm256_mul_pd(w, _mm256_cvtps_pd(_mm_loadu_ps(a + tap))));
        second = _mm256_add_pd(second, _mm256_mul_pd(w, _mm256_cvtps_pd(_mm_loadu_ps(b + tap))));
    }
    std::array<double, 4> x{}, y{};
    _mm256_storeu_pd(x.data(), first);
    _mm256_storeu_pd(y.data(), second);
    std::array<double, 2> result{(x[0] + x[1]) + (x[2] + x[3]), (y[0] + y[1]) + (y[2] + y[3])};
    for (; tap < count; ++tap) {
        result[0] += weights[tap] * a[tap];
        result[1] += weights[tap] * b[tap];
    }
    return result;
}
} // namespace loopz::detail
