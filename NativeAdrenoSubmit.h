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
 * reference until the GPU fence completes. */
struct NativeAdrenoSubmitResource {
    uint64_t handle=0;
    size_t offset=0,bytes=0;
    unsigned access=0;
    bool check_overlap=false;
    bool owns_reference=false;
    uint16_t next_for_mapping=0; /* One-based index of another interval of this mapping. */
};
enum class NativeAdrenoResourceReject : uint32_t {
    None,HandleMissing,HandleUnmapped,HandleUnreleasable,AccessFormat,VmDescribe,Permissions,Range
};
/* NativeAdrenoSubmitResourceFailure retains the request and mapping state at
 * a rejected resource. The profile keeps only the first stage 2 snapshot. */
struct NativeAdrenoSubmitResourceFailure {
    NativeAdrenoResourceReject reason=NativeAdrenoResourceReject::None;
    uint32_t access=0,reserved=0;
    uint32_t slot_present=0,used=0,mapped=0,unreleasable=0;
    uint64_t entry_handle=0,mapped_handle=0,memory_bytes=0;
    uint32_t read_only=0,vm_queried=0,vm_status=0,permissions=0;
    uint64_t vm_iova=0,vm_bytes=0;
};
extern "C" void native_adreno_submit_failure(uint32_t stage,uint32_t status,uint64_t handle,
    uint64_t iova,uint32_t index,uint32_t ibs,uint32_t resources,uint64_t bytes=0,
    const NativeAdrenoSubmitResourceFailure *resource=nullptr);
/* The board diagnostic ABI retains these zero-valued counters. */
extern "C" volatile uint32_t native_adreno_coherence[8];
extern "C" volatile uint32_t native_adreno_wc_audit[6];
extern "C" volatile uint32_t native_adreno_wc_distrusted;
struct NativeAdrenoSubmitStats {
    uint32_t submissions,completed,timeouts,poisoned_jobs;
    size_t references_held,references_retired,peak_jobs;
};
/* NativeAdrenoSubmit owns eight jobs on the shared ring. A resource submission
 * retains every supplied mapping and emits all IB packets with one final
 * fence. Conflicting aliases wait for the preceding GPU fence. CPU stores and
 * completed GPU writes use ordering barriers and hardware coherence.
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
        size_t overlap_count=0; // Imported ranges checked for conflicting active jobs occupy the first entries.
        uint16_t *mapping_resources=nullptr;
        size_t cache_line=0;
    } jobs_[8];
    struct AliasQuery {
        const NativeAdrenoSubmitResource *resources;
        const uint16_t *mapping_resources;
        size_t cache_line;
        bool writes,skip_checked;
    };
    NativeAdrenoSubmitResource *resource_storage_=nullptr;
    uint16_t *mapping_resource_storage_=nullptr;
    uint32_t *command_words_=nullptr;
    NativeAdrenoVm *vm_=nullptr;
    NativeAdrenoRing *ring_=nullptr;
    NativeAdrenoSubmitStats stats_{};
    bool attached_=false,poisoned_=false;
    Job *find(uint32_t fence);
    Job *free();
    IOReturn release(Job &);
    IOReturn poison();
    static bool resourceConflict(void *,uint64_t,size_t,size_t);
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
    /* Completed jobs release their mapping references after the fence read and
     * CPU barrier. The caller serializes submissions, VM changes and retirement. */
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
