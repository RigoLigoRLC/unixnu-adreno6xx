/* SPDX-License-Identifier: MIT */
#ifndef NATIVE_ADRENO_GPU_PROFILE_H
#define NATIVE_ADRENO_GPU_PROFILE_H

#include "NativeAdrenoRing.h"

/* NativeAdrenoGpuProfile retains GPU register samples and completed job
 * timestamps for one bounded USB-requested window. The service configures its
 * counters while CP is stopped and calls poll from its existing worker. The
 * ring records CP arrival and RB completion in its coherent fence allocation;
 * the submission owner delivers those values after observing the normal fence.
 * USB exports the fixed arrays after sampling and pending jobs have finished. */
class NativeAdrenoGpuProfile final {
public:
    static IOReturn initialize(const NativeAdrenoRingIo *,bool (*accessible)(void *));
    static void poll();
    static void fault();
    static bool active();
    static uint32_t beginJob();
    static void finishJob(uint32_t window,uint32_t fence,uint32_t queue,
        uint32_t issue,uint32_t cp,uint32_t rb,bool valid);
};

extern "C" uint32_t native_adreno_gpu_profile_start(uint32_t duration_ms);
extern "C" void native_adreno_gpu_profile_stop(void);
extern "C" void native_adreno_gpu_profile_dump(void);

#endif
