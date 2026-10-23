// Separate translation unit: match the loaded driver's hardened compiler flags.
#include "windows_packed_samples_benchmark_api.hpp"
#include "../native/platform/windows/driver/packed_samples.hpp"
extern "C" __declspec(noinline) bool
loopzPackedConvert(const unsigned char* source, unsigned sourceBytes, unsigned sourceSize,
                   unsigned char* destination, unsigned destinationBytes, unsigned destinationSize,
                   unsigned count) noexcept {
    return loopz_driver::convertPackedSamples(source, sourceBytes, sourceSize, destination,
                                              destinationBytes, destinationSize, count);
}
