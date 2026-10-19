#pragma once
#include <stdint.h>
#include <CoreFoundation/CoreFoundation.h>
#define LOOPZ_TIMING_PROPERTY 0x4c5a746dU
#define LOOPZ_TIMING_ACTION 0x4c5a000000000000ULL
#ifdef __cplusplus
extern "C" {
#endif
void LoopZBridgeInitialize(void);
uint32_t LoopZClockPeriod(void);
uint32_t LoopZInputLatency(void);
int LoopZValidTiming(uint32_t period, uint32_t queue, uint32_t source);
void LoopZSetTiming(uint32_t period, uint32_t queue, uint32_t source);
CFDictionaryRef LoopZCopyTiming(void);
void LoopZClockStart(double rate);
void LoopZClockStamp(double* sample, uint64_t* host, uint64_t* seed);
void LoopZRead(float* interleaved, uint32_t frames, double sampleTime);
void LoopZWrite(const float* interleaved, uint32_t frames, double sampleTime);
#ifdef __cplusplus
}
#endif
