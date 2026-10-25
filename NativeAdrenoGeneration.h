/* SPDX-License-Identifier: MIT */
#ifndef NATIVE_ADRENO_GENERATION_H
#define NATIVE_ADRENO_GENERATION_H
#include <stdint.h>

/* The generation selects CP packets, RBBM interrupt bits and register setup. */
enum NativeAdrenoGeneration : uint32_t {
    NativeAdrenoA740 = 0,
    NativeAdrenoA642L = 1
};

#endif
