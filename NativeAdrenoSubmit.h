/* SPDX-License-Identifier: MIT */
#ifndef NATIVE_ADRENO_SUBMIT_H
#define NATIVE_ADRENO_SUBMIT_H
#include "NativeAdrenoRing.h"
#include "NativeAdrenoVm.h"
#include "NativeAdrenoABI.h"

struct NativeAdrenoSubmitBuffer {
    uint64_t handle;
    uint64_t iova;
    size_t bytes;      /* Complete indirect-buffer range in bytes. */
};
/* NativeAdrenoSubmitResource names one exact BO range used by a job. Separate
 * ranges of one mapping remain separate; one of them owns the mapping's job
 * reference until every range has completed its cache acquisition. */
struct NativeAdrenoSubmitResource {
    uint64_t handle=0;
    size_t offset=0,bytes=0;
    unsigned access=0;
    bool maintenance=false;
    bool owns_reference=false;
};
extern "C" void native_adreno_submit_failure(uint32_t stage,uint32_t status,uint64_t handle,
    uint64_t iova,uint32_t index,uint32_t ibs,uint32_t resources,uint64_t bytes=0);
/* Coherence samples (int-070): [0] submissions sampled; [1..3] the last one's first IB word through the kernel's cacheable alias,
 * through DRAM before and through DRAM after the CPU cache maintenance of every mapped BO (~0 when unreadable); [4] samples whose
 * three values differ and [5..7] the first such sample. */
extern "C" volatile uint32_t native_adreno_coherence[8];
/* The write-combine audit (int-100), see NativeAdrenoSubmit.cpp, and the submission (1-based) that ended the trust; 0 = never. */
extern "C" volatile uint32_t native_adreno_wc_audit[6];
extern "C" volatile uint32_t native_adreno_wc_distrusted;
struct NativeAdrenoSubmitStats {
    uint32_t submissions,completed,timeouts,poisoned_jobs;
    size_t references_held,references_retired,peak_jobs;
};
/* NativeAdrenoSubmit owns eight jobs on the shared ring. A resource submission
 * retains every supplied mapping, publishes its used CPU ranges, and emits all
 * IB packets with one final fence. Cached aliases remain unavailable to a
 * conflicting job until the preceding GPU job completes and its CPU cache
 * acquisition finishes. WC memory shares one barrier for the whole submission.
 *
 * The IOKit service serializes calls and retries Busy outside its device lock.
 * A poisoned ring retains every job reference until hardware recovery proves
 * that GPU access stopped. The small submit method serves kernel diagnostics;
 * user connections use submitResources for both ABI versions. */
class NativeAdrenoSubmit final {
    struct Job {
        uint32_t fence=0;
        size_t count=0;
        uint64_t handles[16]{};
        uint32_t queue=0;
        bool active=false;
        NativeAdrenoSubmitResource *resources=nullptr;
        size_t resource_count=0;
        size_t maintenance_count=0; /* Resources needing CPU maintenance occupy the first entries. */
    } jobs_[8];
    NativeAdrenoSubmitResource *resource_storage_=nullptr;
    uint32_t *command_words_=nullptr;
    NativeAdrenoVm *vm_=nullptr;
    NativeAdrenoRing *ring_=nullptr;
    NativeAdrenoSubmitStats stats_{};
    bool attached_=false,poisoned_=false;
    uint32_t audit_tick_=0;   /* submissions since attach; every eighth one audits its IB lines */
    Job *find(uint32_t fence);
    Job *free();
    IOReturn release(Job &, bool acquire_imported = true);
    IOReturn poison();
public:
    NativeAdrenoSubmit()=default;
    ~NativeAdrenoSubmit()=default;
    NativeAdrenoSubmit(const NativeAdrenoSubmit &)=delete;
    NativeAdrenoSubmit &operator=(const NativeAdrenoSubmit &)=delete;
    IOReturn attach(NativeAdrenoVm *,NativeAdrenoRing *);
    /* Refuses while jobs are outstanding or poisoned; the caller must have
     * stopped the GPU and observed every fence before calling this. */
    IOReturn detach();
    /* queue is bookkeeping for one shared ring; it never reorders or isolates
     * submissions and is validated by the caller's open queue set. */
    IOReturn submit(const NativeAdrenoSubmitBuffer *,size_t,uint32_t queue,
        uint32_t *fence);
    IOReturn submitResources(const NativeAdrenoSubmitBuffer *,size_t,
        const NativeAdrenoSubmitResource *,size_t,uint32_t queue,uint32_t fallback_reason,uint32_t *fence);
    bool queueOutstanding(uint32_t queue) const;
    /* Queues [first, first+count): whether any of their jobs is outstanding, and a wait for every one of them (oldest first) that
     * leaves other queues' jobs alone. Each client of the shared ring owns one such range. */
    bool rangeOutstanding(uint32_t first,uint32_t count) const;
    IOReturn waitRange(uint32_t first,uint32_t count,const NativeAdrenoRingClock *,uint32_t timeout_us);
    /* Completed jobs are collected without waiting. Each job's written ranges are acquired before
     * its mapping references and slot are retired. The caller serializes submissions, VM changes and retirement. */
    IOReturn retireCompleted();
    /* Waits for every outstanding job in submission order and retires its references. */
    IOReturn waitAll(const NativeAdrenoRingClock *,uint32_t timeout_us);
    /* Polls the CP timestamp; on completion retires every buffer reference. A fence whose job was already retired (by
     * wait or retireCompleted) is checked against the ring and succeeds once the CP has passed it. */
    IOReturn wait(uint32_t fence,const NativeAdrenoRingClock *,uint32_t timeout_us);
    /* Wait for an earlier fence without touching job references; a timeout
     * leaves every job and reference exactly as they were. */
    IOReturn observeFence(uint32_t fence,const NativeAdrenoRingClock *,uint32_t timeout_us);
    bool poisoned() const {return poisoned_;}
    size_t outstanding() const;
    NativeAdrenoSubmitStats stats() const {return stats_;}
};
#endif
