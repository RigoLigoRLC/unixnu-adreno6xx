/* SPDX-License-Identifier: MIT */
#ifndef NATIVE_ADRENO_PERFORMANCE_HPP
#define NATIVE_ADRENO_PERFORMANCE_HPP

#include <stdint.h>

enum NativeAdrenoCounterIndex : unsigned {
    NativeAdrenoCoreCycles,
    NativeAdrenoSpBusyCycles,
    NativeAdrenoSpAluCycles,
    NativeAdrenoVsFullAluInstructions,
    NativeAdrenoFsFullAluInstructions,
    NativeAdrenoTpBusyCycles,
    NativeAdrenoUcheBusyCycles,
    NativeAdrenoPerformanceCounterCount
};

/* NativeAdrenoPerformanceSnapshot is copied under the service's sampling lock.
 * Values and deltas come from hardware counters; acknowledgedHz is the startup
 * frequency request acknowledged by the GMU. coreCyclesRateHz
 * is the measured cycle delta per wall-clock second, including clock gating.
 * The SP instruction counters aggregate the GPU, without per-shader attribution. */
struct NativeAdrenoPerformanceSnapshot {
    uint64_t sampledUs;
    uint64_t intervalUs;
    uint64_t busyTicks;
    uint64_t totalTicks;
    uint64_t busyCounter;
    uint64_t ifpcCounter;
    uint64_t samples;
    uint64_t counterDiscontinuities;
    uint64_t counterValues[NativeAdrenoPerformanceCounterCount];
    uint64_t counterDeltas[NativeAdrenoPerformanceCounterCount];
    uint32_t counterSelectors[NativeAdrenoPerformanceCounterCount];
    uint32_t counterValidMask;
    uint32_t counterPopulatedMask; // Counters with at least one observed nonzero delta.
    uint32_t running;
    uint32_t busyValid;
    uint32_t busyPermille;
    uint32_t acknowledgedHz;
    uint32_t coreCyclesRateHz;
};

extern "C" bool native_adreno_performance_snapshot(NativeAdrenoPerformanceSnapshot *);

#endif
