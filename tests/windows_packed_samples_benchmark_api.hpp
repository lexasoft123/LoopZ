#pragma once
extern "C" bool loopzPackedConvert(const unsigned char* source, unsigned sourceBytes,
                                   unsigned sourceSize, unsigned char* destination,
                                   unsigned destinationBytes, unsigned destinationSize,
                                   unsigned count) noexcept;
