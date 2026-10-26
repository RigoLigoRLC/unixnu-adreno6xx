/* SPDX-License-Identifier: MIT */
#ifndef NATIVE_ADRENO_SUBMISSION_PROFILE_H
#define NATIVE_ADRENO_SUBMISSION_PROFILE_H

#include "NativeAdrenoMemory.h"

class NativeAdrenoVm;
struct NativeAdrenoRingIo;

enum : unsigned {
    NativeAdrenoProfileCounterCount = 12,
    NativeAdrenoProfileSlots = 32,
    NativeAdrenoProfileSlotBytes = 512,
    NativeAdrenoProfileSnapshotWords = 107
};

/* NativeAdrenoProfileCpuTiming contains host durations for one validated IB list.
 * Submit measures the stages while its caller holds the shared submission lock. */
struct NativeAdrenoProfileCpuTiming {
    uint64_t submitUs;
    uint64_t cpuUs;
    uint64_t validationUs;
    uint64_t cacheUs;
    uint64_t publishUs;
    uint64_t tlbUs;
    uint64_t ringUs;
    uint32_t queue;
    uint32_t depth;
    uint32_t ibs;
};

uint64_t NativeAdrenoProfileNowUs();

/* NativeAdrenoSubmissionProfile owns Normal non-cacheable RAM at GPU address
 * 0x304000. The ring writes begin/end timestamps, counter values, selectors and
 * a fence marker into selected submissions' slots. Every submission retains
 * its CPU durations until its existing completion fence passes. The hardware
 * sampling interval selects which slots also receive register snapshots.
 *
 * The caller serializes every method with ring submission and retirement. The
 * allocation and its privileged GPU mapping remain owned for the entire boot,
 * including failures. A separate short lock protects completed CPU records
 * copied by the USB dump; that lock never covers GPU access or log output. */
class NativeAdrenoSubmissionProfile final {
    struct Slot {
        NativeAdrenoProfileCpuTiming cpu{};
        uint64_t enqueuedTicks = 0;
        uint64_t enqueuedUs = 0;
        uint32_t fence = 0;
        bool active = false;
        bool anchorValid = false;
        bool sampled = false;
    } slots_[NativeAdrenoProfileSlots];
    NativeAdrenoMemory memory_;
    uint64_t handle_ = 0;
    uint32_t sampleEvery_ = 16;
    uint64_t submissions_ = 0;
    bool enabled_ = false;
public:
    IOReturn attach(NativeAdrenoVm *,const NativeAdrenoRingIo *);
    bool enabled() const
    {
        return enabled_;
    }
    bool samples() const
    {
        return enabled_ && submissions_%sampleEvery_==0;
    }
    IOReturn prepare(uint32_t fence,const NativeAdrenoProfileCpuTiming &,bool sampled);
    void cancel(uint32_t fence);
    IOReturn emit(uint32_t fence,bool end,uint32_t *words,size_t capacity);
    void markEnqueued(uint32_t fence,const NativeAdrenoRingIo *);
    void setCpuTiming(uint32_t fence,const NativeAdrenoProfileCpuTiming &);
    void collect(uint32_t fence);
};

extern "C" void native_adreno_profile_dump(void);

#endif
