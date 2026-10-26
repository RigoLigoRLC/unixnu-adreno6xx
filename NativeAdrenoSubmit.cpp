/* SPDX-License-Identifier: MIT */
#include "NativeAdrenoSubmit.h"
#include "AdrenoPM4.h"
#include <IOKit/IOLib.h>

/* CP_INDIRECT_BUFFER is opcode 0x3f with a 3-word payload: 64-bit GPU address
 * and IB_SIZE in dwords (bits 0..19), exactly as the pinned adreno_pm4.xml
 * domain states and Linux msm_gem_submit.c converts the userspace byte size. */
enum : uint32_t {SubmitIndirectBuffer=0x3f,SubmitIbWords=3,SubmitMaxJobs=8,
    SubmitMaxBuffers=16,SubmitMaxIbBytes=0xfffffu*4u,
    SubmitAuditBytes=256u*1024u};   /* IB bytes the write-combine audit compares per submission */
extern "C" {volatile uint32_t native_adreno_coherence[8];}
/* The submission (1-based count) at which the write-combine audit found a dirty IB line or failed; 0 = never. */
extern "C" {volatile uint32_t native_adreno_wc_distrusted;}
/* The write-combine audit before each submission while trusted: [0] submissions audited, [1] IB lines compared, [2] dirty lines,
 * [3] stale clean lines, [4] the last audit failure (any failure also ends the trust), [5] pages found back at the default attribute
 * and write-combined again. */
extern "C" {volatile uint32_t native_adreno_wc_audit[6];}
/* Fence waits: [0] calls, [1] timeouts, [2] the fence of the last timed-out wait, [3] the fence value the GPU had written then, [4] the newest fence handed out,
 * [5] the newest completed, [6] waits for a fence no job holds (observeFence path), [7] the last fence refused as never submitted. */
extern "C" {volatile uint32_t native_adreno_fence_wait[8];}
/* The IBs of the last submissions: [slot] fence, iova low/high, bytes, then the first 28 dwords of the first IB (read as the kernel maps it). */
extern "C" {volatile uint32_t native_adreno_ib_log[4][4+28];volatile uint32_t native_adreno_ib_log_count;}
extern "C" {extern uint64_t (*native_adreno_vm_now_us)(void);extern volatile uint32_t native_adreno_method_stats[20][4];}

/* Bounded render-path profile for the GPU performance investigation.
 *
 * Every successful submission takes one record in a fixed 256-entry ring.
 * The completion path fills the fence latency when the CP timestamp is first
 * observed. The records are copied under a short spinlock by the gpu dump and
 * printed after the lock is released, so the render path never waits for the
 * console. No allocation, no disk I/O and no per-frame logging: a submission
 * only writes one record and updates a few counters. */
enum : unsigned {
    ProfileSamples = 256,
    ProfileBuckets = 8,
    ProfileClientChars = 16,
};
struct ProfileSample {
    uint32_t seq;
    uint32_t fence;
    uint32_t ibCount;
    uint32_t complete;
    char client[ProfileClientChars];
    uint64_t submitUs;
    uint64_t completeUs;
    uint32_t latencyUs;
    uint32_t retireUs;
    uint32_t cacheUs;
    uint32_t publishUs;
    uint32_t tlbUs;
    uint32_t ringUs;
    uint32_t submitCpuUs;
    uint32_t mapCalls;
    uint32_t freeCalls;
    uint32_t allocCalls;
    uint64_t mapUs;
    uint64_t freeUs;
    uint64_t allocUs;
};
static ProfileSample profileSamples[ProfileSamples];
static uint32_t profileHead;
static uint32_t profileSeq;
static uint32_t profileSubmitted;
static uint32_t profileCompleted;
static uint64_t profileLatencyTotal;
static uint64_t profileLatencyMax;
static uint64_t profileLatencyBuckets[ProfileBuckets];
static char profileClient[ProfileClientChars];
static uint32_t profileLock;
/* Per-submission interval deltas of the ABI method totals, so the dump can
 * show the BO work between two submissions without sampling the clock. */
static uint32_t profileLastCalls[6];
static uint32_t profileLastUs[6];
static uint32_t profileMapCalls[4];
static uint64_t profileMapUs[4];
static uint64_t profileMapMax[4];
static uint32_t profileLockCalls[20];
static uint64_t profileLockUs[20];
static uint64_t profileLockMax[20];
static uint64_t profileNow()
{
    return native_adreno_vm_now_us ? native_adreno_vm_now_us() : 0;
}
static void profileEnter()
{
    while (__atomic_test_and_set(&profileLock, __ATOMIC_ACQUIRE)) {
    }
}
static void profileLeave()
{
    __atomic_clear(&profileLock, __ATOMIC_RELEASE);
}
extern "C" void native_adreno_profile_set_client(const char *name)
{
    if (!name) {
        name = "?";
    }
    profileEnter();
    unsigned at = 0;
    for (; at < sizeof(profileClient); ++at) {
        profileClient[at] = 0;
    }
    for (at = 0; at + 1 < sizeof(profileClient) && name[at]; ++at) {
        profileClient[at] = name[at];
    }
    profileLeave();
}
extern "C" void native_adreno_profile_map(uint32_t pages, uint64_t us)
{
    const unsigned bucket = pages < 16 ? 0 : pages < 256 ? 1 : pages < 4096 ? 2 : 3;
    profileEnter();
    profileMapCalls[bucket] = profileMapCalls[bucket] + 1;
    profileMapUs[bucket] += us;
    if (us > profileMapMax[bucket]) {
        profileMapMax[bucket] = us;
    }
    profileLeave();
}
extern "C" void native_adreno_profile_lock_wait(uint32_t method, uint64_t us)
{
    if (method < 20) {
        profileEnter();
        profileLockCalls[method] = profileLockCalls[method] + 1;
        profileLockUs[method] += us;
        if (us > profileLockMax[method]) {
            profileLockMax[method] = us;
        }
        profileLeave();
    }
}
static void profileNoteSubmit(uint32_t fence, uint32_t ibCount,
    uint64_t t0, uint64_t t1, uint64_t t2, uint64_t t3, uint64_t t4, uint64_t t5)
{
    profileEnter();
    uint32_t intervalCalls[6] = {};
    uint32_t intervalUs[6] = {};
    for (unsigned method = 2; method <= 5; ++method) {
        const uint32_t calls = native_adreno_method_stats[method][0];
        const uint32_t us = native_adreno_method_stats[method][3];
        intervalCalls[method] = calls - profileLastCalls[method];
        intervalUs[method] = us - profileLastUs[method];
        profileLastCalls[method] = calls;
        profileLastUs[method] = us;
    }
    ProfileSample &sample = profileSamples[profileHead];
    sample.seq = ++profileSeq;
    sample.fence = fence;
    sample.ibCount = ibCount;
    sample.complete = 0;
    for (unsigned at = 0; at < sizeof(sample.client); ++at) {
        sample.client[at] = profileClient[at];
    }
    sample.submitUs = t0;
    sample.completeUs = 0;
    sample.latencyUs = 0;
    sample.retireUs = uint32_t(t1 - t0);
    sample.cacheUs = uint32_t(t2 - t1);
    sample.publishUs = uint32_t(t3 - t2);
    sample.tlbUs = uint32_t(t4 - t3);
    sample.ringUs = uint32_t(t5 - t4);
    sample.submitCpuUs = uint32_t(t5 - t0);
    sample.mapCalls = intervalCalls[3];
    sample.mapUs = intervalUs[3];
    sample.freeCalls = intervalCalls[4];
    sample.freeUs = intervalUs[4];
    sample.allocCalls = intervalCalls[2];
    sample.allocUs = intervalUs[2];
    profileHead = (profileHead + 1) % ProfileSamples;
    profileSubmitted = profileSubmitted + 1;
    profileLeave();
}
static void profileNoteComplete(uint32_t fence)
{
    if (!fence) {
        return;
    }
    const uint64_t now = profileNow();
    if (!now) {
        return;
    }
    profileEnter();
    for (unsigned at = 0; at < ProfileSamples; ++at) {
        ProfileSample &sample = profileSamples[at];
        if (!sample.seq || sample.complete || !sample.fence) {
            continue;
        }
        if (int32_t(fence - sample.fence) < 0) {
            continue;
        }
        sample.complete = 1;
        sample.completeUs = now;
        const uint64_t latency = now > sample.submitUs ? now - sample.submitUs : 0;
        sample.latencyUs = uint32_t(latency > 0xffffffffu ? 0xffffffffu : latency);
        profileCompleted = profileCompleted + 1;
        profileLatencyTotal += latency;
        if (latency > profileLatencyMax) {
            profileLatencyMax = latency;
        }
        const unsigned bucket = latency < 5000 ? 0 : latency < 10000 ? 1 : latency < 20000 ? 2 :
            latency < 50000 ? 3 : latency < 100000 ? 4 : latency < 200000 ? 5 :
            latency < 500000 ? 6 : 7;
        profileLatencyBuckets[bucket] = profileLatencyBuckets[bucket] + 1;
    }
    profileLeave();
}
/* Called by the gpu dump. Copy under the spinlock, then print without it. */
extern "C" void native_adreno_profile_dump(void)
{
    static ProfileSample samples[ProfileSamples];
    static uint32_t submitted, completed;
    static uint64_t latencyTotal, latencyMax;
    static uint64_t buckets[ProfileBuckets];
    static uint32_t mapCalls[4];
    static uint64_t mapUs[4];
    static uint64_t mapMax[4];
    static uint32_t lockCalls[20];
    static uint64_t lockUs[20];
    static uint64_t lockMax[20];
    profileEnter();
    submitted = profileSubmitted;
    completed = profileCompleted;
    latencyTotal = profileLatencyTotal;
    latencyMax = profileLatencyMax;
    for (unsigned at = 0; at < ProfileSamples; ++at) {
        samples[at] = profileSamples[at];
    }
    for (unsigned at = 0; at < ProfileBuckets; ++at) {
        buckets[at] = profileLatencyBuckets[at];
    }
    for (unsigned at = 0; at < 4; ++at) {
        mapCalls[at] = profileMapCalls[at];
        mapUs[at] = profileMapUs[at];
        mapMax[at] = profileMapMax[at];
    }
    for (unsigned at = 0; at < 20; ++at) {
        lockCalls[at] = profileLockCalls[at];
        lockUs[at] = profileLockUs[at];
        lockMax[at] = profileLockMax[at];
    }
    profileLeave();
    IOLog("NativeAdrenoProfile: summary submitted=%u completed=%u lat_total=%llu lat_max=%llu buckets=%llu,%llu,%llu,%llu,%llu,%llu,%llu,%llu\n",
        submitted, completed, (unsigned long long)latencyTotal, (unsigned long long)latencyMax,
        (unsigned long long)buckets[0], (unsigned long long)buckets[1], (unsigned long long)buckets[2],
        (unsigned long long)buckets[3], (unsigned long long)buckets[4], (unsigned long long)buckets[5],
        (unsigned long long)buckets[6], (unsigned long long)buckets[7]);
    IOLog("NativeAdrenoProfile: lock_wait m2=%u/%llu/%llu m3=%u/%llu/%llu m4=%u/%llu/%llu m5=%u/%llu/%llu\n",
        lockCalls[2], (unsigned long long)lockUs[2], (unsigned long long)lockMax[2],
        lockCalls[3], (unsigned long long)lockUs[3], (unsigned long long)lockMax[3],
        lockCalls[4], (unsigned long long)lockUs[4], (unsigned long long)lockMax[4],
        lockCalls[5], (unsigned long long)lockUs[5], (unsigned long long)lockMax[5]);
    IOLog("NativeAdrenoProfile: map_pages small=%u/%llu/%llu medium=%u/%llu/%llu large=%u/%llu/%llu huge=%u/%llu/%llu\n",
        mapCalls[0], (unsigned long long)mapUs[0], (unsigned long long)mapMax[0],
        mapCalls[1], (unsigned long long)mapUs[1], (unsigned long long)mapMax[1],
        mapCalls[2], (unsigned long long)mapUs[2], (unsigned long long)mapMax[2],
        mapCalls[3], (unsigned long long)mapUs[3], (unsigned long long)mapMax[3]);
    for (unsigned at = 0; at < ProfileSamples; ++at) {
        const ProfileSample &sample = samples[at];
        if (!sample.seq) {
            continue;
        }
        IOLog("NativeAdrenoProfile: S seq=%u fence=%u client=%s ib=%u complete=%u t=%llu c=%llu lat=%u submit_cpu=%u retire=%u cache=%u publish=%u tlb=%u ring=%u map=%u/%llu free=%u/%llu alloc=%u/%llu\n",
            sample.seq, sample.fence, sample.client, sample.ibCount, sample.complete,
            (unsigned long long)sample.submitUs, (unsigned long long)sample.completeUs,
            sample.latencyUs, sample.submitCpuUs, sample.retireUs, sample.cacheUs,
            sample.publishUs, sample.tlbUs, sample.ringUs,
            sample.mapCalls, (unsigned long long)sample.mapUs,
            sample.freeCalls, (unsigned long long)sample.freeUs,
            sample.allocCalls, (unsigned long long)sample.allocUs);
    }
}
IOReturn NativeAdrenoSubmit::poison()
{
    poisoned_=true;return kIOReturnIOError;
}
NativeAdrenoSubmit::Job *NativeAdrenoSubmit::find(uint32_t fence)
{
    if(fence)for(auto &job:jobs_)if(job.active && job.fence==fence)return &job;
    return nullptr;
}
NativeAdrenoSubmit::Job *NativeAdrenoSubmit::free()
{
    for(auto &job:jobs_)if(!job.active)return &job;
    return nullptr;
}
IOReturn NativeAdrenoSubmit::attach(NativeAdrenoVm *vm,NativeAdrenoRing *ring)
{
    if(attached_ || poisoned_ || vm_ || ring_)return kIOReturnBusy;
    if(!vm || !ring || vm->poisoned() || ring->poisoned())return kIOReturnBadArgument;
    vm_=vm;ring_=ring;attached_=true;stats_={};
    return kIOReturnSuccess;
}
IOReturn NativeAdrenoSubmit::release(Job &job, bool acquire_imported)
{
    IOReturn status=kIOReturnSuccess;
    for(size_t i=0;i<job.count;++i) {
        IOReturn retired=vm_->retireJobAfterCompletion(job.handles[i]);
        if(retired)status=retired;else ++stats_.references_retired;
    }
    if(status)return poison();
    if(acquire_imported && vm_->acquireImported())return poison();
    job={};return kIOReturnSuccess;
}
IOReturn NativeAdrenoSubmit::detach()
{
    if(poisoned_ || outstanding())return kIOReturnBusy;
    if(!attached_)return kIOReturnNotReady;
    attached_=false;vm_=nullptr;ring_=nullptr;return kIOReturnSuccess;
}
bool NativeAdrenoSubmit::queueOutstanding(uint32_t queue) const
{
    return rangeOutstanding(queue,1);
}
bool NativeAdrenoSubmit::rangeOutstanding(uint32_t first,uint32_t count) const
{
    for(const auto &job:jobs_)if(job.active && job.queue-first<count)return true;
    return false;
}
size_t NativeAdrenoSubmit::outstanding() const
{
    size_t count=0;for(const auto &job:jobs_)if(job.active)++count;return count;
}
IOReturn NativeAdrenoSubmit::submit(const NativeAdrenoSubmitBuffer *buffers,size_t count,
    uint32_t queue,uint32_t *fence)
{
    const uint64_t profileT0=profileNow();
    uint64_t profileT1=profileT0,profileT2=profileT0,profileT3=profileT0,profileT4=profileT0,profileT5=profileT0;
    if(!fence)return kIOReturnBadArgument;*fence=0;
    if(!attached_ || poisoned_)return kIOReturnNotReady;
    if(!buffers || !count || count>SubmitMaxBuffers)return kIOReturnBadArgument;
    /* Every buffer is validated against the live VM before any reference. */
    uint64_t handles[SubmitMaxBuffers];size_t handle_count=0;
    uint32_t words[SubmitMaxBuffers*(1+SubmitIbWords)];size_t word_count=0;
    for(size_t i=0;i<count;++i) {
        const NativeAdrenoSubmitBuffer &buffer=buffers[i];
        if(!buffer.handle || !buffer.bytes || (buffer.bytes&3) || buffer.bytes>SubmitMaxIbBytes)
            return kIOReturnBadArgument;
        uint64_t iova=0;size_t bytes=0;unsigned permissions=0;
        IOReturn status=vm_->describe(buffer.handle,&iova,&bytes,&permissions);
        if(status)return status;
        if(!(permissions&ArmSmmu4KRead) || buffer.iova<iova ||
            buffer.iova-iova>bytes || buffer.bytes>bytes-(buffer.iova-iova))return kIOReturnBadArgument;
        uint32_t header=0;
        if(AdrenoPM4Type7(SubmitIndirectBuffer,SubmitIbWords,&header)!=AdrenoPM4Success)
            return poison();
        words[word_count++]=header;
        words[word_count++]=(uint32_t)buffer.iova;
        words[word_count++]=(uint32_t)(buffer.iova>>32);
        words[word_count++]=(uint32_t)(buffer.bytes/4);
        bool known=false;
        for(size_t j=0;j<handle_count;++j)if(handles[j]==buffer.handle) {known=true;break;}
        if(!known)handles[handle_count++]=buffer.handle;
    }
    IOReturn swept=retireCompleted();
    profileT1=profileNow();
    if(swept)return swept;
    Job *job=free();
    if(!job)return kIOReturnBusy;
    for(size_t i=0;i<handle_count;++i) {
        IOReturn status=vm_->holdJob(handles[i]);
        if(status) {
            while(i) {vm_->retireJobAfterCompletion(handles[--i]);++stats_.references_retired;}
            return status;
        }
        ++stats_.references_held;
    }
    uint32_t value=0;
    /* int-070: the first IB word as the kernel's cacheable alias and DRAM see it before any maintenance, then DRAM after the CPU
     * caches over every mapped BO are cleaned and invalidated. The board shows the last submission's record on a stall. */
    uint32_t cached=~0u,before=~0u,ignored=0,after=~0u;
    /* Write-combined stores this CPU made before the call (the client's command words) complete before anything below. */
    __asm__ volatile("dsb sy":::"memory");
    /* int-098/099: with write-combined user BOs and no maintenance the CP stopped at the end of the first IB, as in int-065..069 when an
     * IB sat in a CPU cache. While write-combined BOs are trusted, every line of this submission's IBs is compared as a CPU cache and
     * DRAM hold it (bounded). A dirty line means a client view still caches its BOs: maintain every BO from now on, this submission
     * included (the audit already wrote the IB lines back; the rest of the BOs may hold more). */
    if(vm_->writeCombinedTrusted()) {
        /* int-100: the dirty IB page was back at XNU's default (cacheable) attribute. Write-combine every such page again first; the
         * pmap writes its lines back on the way, so the audit below should then find none. */
        uint32_t reset=0;
        IOReturn audited=vm_->reassertWriteCombined(&reset);
        native_adreno_wc_audit[5]=native_adreno_wc_audit[5]+reset;
        uint32_t lines=0,dirty=0,stale=0;size_t budget=SubmitAuditBytes;
        /* The re-assertion above is the correction and runs every time; the audit only detects, and found no dirty line in int-141's
         * 133798, so after the first 16 submissions it compares every eighth one's IBs (it cost about 2 ms each). */
        ++audit_tick_;
        const bool audit_now=audit_tick_<=16 || (audit_tick_&7)==1;
        if(!audit_now)budget=0;
        for(size_t i=0;i<count && budget && !audited;++i) {   /* a failed re-assertion skips the audit and ends the trust */
            const size_t bytes=buffers[i].bytes<budget?(size_t)buffers[i].bytes:budget;
            audited=vm_->auditRange(buffers[i].iova,bytes,&lines,&dirty,&stale);budget-=bytes;
        }
        if(audit_now && audited!=kIOReturnUnsupported) {   /* no board hook (host): nothing to compare */
            native_adreno_wc_audit[0]=native_adreno_wc_audit[0]+1;
            native_adreno_wc_audit[1]=native_adreno_wc_audit[1]+lines;
            native_adreno_wc_audit[2]=native_adreno_wc_audit[2]+dirty;
            native_adreno_wc_audit[3]=native_adreno_wc_audit[3]+stale;
            if(audited)native_adreno_wc_audit[4]=audited;
        }
        if(dirty || (audited && audited!=kIOReturnUnsupported)) {
            vm_->distrustWriteCombined();
            native_adreno_wc_distrusted=native_adreno_coherence[0]+1;
        }
    }
    profileT2=profileNow();
    (void)vm_->sampleWord(buffers[0].iova,&cached,&before);
    IOReturn status=vm_->publishMapped();
    if(!status) {
        (void)vm_->sampleWord(buffers[0].iova,&ignored,&after);
        profileT3=profileNow();
        native_adreno_coherence[0]=native_adreno_coherence[0]+1;
        native_adreno_coherence[1]=cached;native_adreno_coherence[2]=before;native_adreno_coherence[3]=after;
        if(cached!=before || before!=after) {
            if(!native_adreno_coherence[4]) {
                native_adreno_coherence[5]=cached;native_adreno_coherence[6]=before;native_adreno_coherence[7]=after;
            }
            native_adreno_coherence[4]=native_adreno_coherence[4]+1;
        }
        status=vm_->invalidateTlb();
        profileT4=profileNow();
    }
    if(status) {
        for(size_t i=0;i<handle_count;++i) {vm_->retireJobAfterCompletion(handles[i]);++stats_.references_retired;}
        return status;
    }
    {
        const uint32_t slot=native_adreno_ib_log_count&3;uint32_t head[28];
        (void)vm_->peekWords(buffers[0].iova,head,28);
        native_adreno_ib_log[slot][0]=ring_->lastSubmitted()+1;native_adreno_ib_log[slot][1]=(uint32_t)buffers[0].iova;
        native_adreno_ib_log[slot][2]=(uint32_t)(buffers[0].iova>>32);native_adreno_ib_log[slot][3]=(uint32_t)buffers[0].bytes;
        for(unsigned i=0;i<28;++i)native_adreno_ib_log[slot][4+i]=head[i];
        native_adreno_ib_log_count=native_adreno_ib_log_count+1;
    }
    status=ring_->submitFramed(words,word_count,&value);
    profileT5=profileNow();
    if(status) {
        /* A poisoned ring may already have consumed the WPTR write, so the
         * referenced buffers cannot be retired here. */
        if(ring_->poisoned()) {
            *job=Job{};++stats_.poisoned_jobs;
            for(size_t i=0;i<handle_count;++i)job->handles[i]=handles[i];
            job->count=handle_count;job->queue=queue;job->active=true;
            return poison();
        }
        while(handle_count) {vm_->retireJobAfterCompletion(handles[--handle_count]);++stats_.references_retired;}
        return status;
    }
    for(size_t i=0;i<handle_count;++i)job->handles[i]=handles[i];
    job->count=handle_count;job->fence=value;job->queue=queue;job->active=true;
    ++stats_.submissions;
    if(outstanding()>stats_.peak_jobs)stats_.peak_jobs=outstanding();
    profileNoteSubmit(value,(uint32_t)count,profileT0,profileT1,profileT2,profileT3,profileT4,profileT5);
    if(ring_->completedFence()>=value)profileNoteComplete(value);
    *fence=value;
    return kIOReturnSuccess;
}
IOReturn NativeAdrenoSubmit::observeFence(uint32_t fence,const NativeAdrenoRingClock *clock,
    uint32_t timeout_us)
{
    if(!attached_ || poisoned_)return kIOReturnNotReady;
    if(!fence)return kIOReturnBadArgument;
    const NativeAdrenoRingFence result=ring_->observeFence(fence,clock,timeout_us,nullptr);
    if(result==NativeAdrenoRingFencePoisoned)return poison();
    if(result==NativeAdrenoRingFenceArgument)return kIOReturnBadArgument;
    if(result==NativeAdrenoRingFenceTimeout)return kIOReturnTimeout;
    profileNoteComplete(fence);
    return kIOReturnSuccess;
}
IOReturn NativeAdrenoSubmit::wait(uint32_t fence,const NativeAdrenoRingClock *clock,
    uint32_t timeout_us)
{
    if(!attached_ || poisoned_)return kIOReturnNotReady;
    Job *job=find(fence);
    uint32_t observed=0;
    native_adreno_fence_wait[0]=native_adreno_fence_wait[0]+1;
    if(!job) {
        native_adreno_fence_wait[6]=native_adreno_fence_wait[6]+1;
        const NativeAdrenoRingFence passed=ring_->observeFence(fence,clock,timeout_us,&observed);
        if(passed==NativeAdrenoRingFencePoisoned)return poison();
        if(passed==NativeAdrenoRingFenceArgument) {native_adreno_fence_wait[7]=fence;return kIOReturnBadArgument;}
        if(passed==NativeAdrenoRingFenceTimeout) {
            ++stats_.timeouts;
            native_adreno_fence_wait[1]=native_adreno_fence_wait[1]+1;native_adreno_fence_wait[2]=fence;native_adreno_fence_wait[3]=observed;
            native_adreno_fence_wait[4]=ring_->lastSubmitted();native_adreno_fence_wait[5]=ring_->completedFence();
            return kIOReturnTimeout;
        }
        profileNoteComplete(fence);
        return kIOReturnSuccess;
    }
    const NativeAdrenoRingFence result=ring_->wait(fence,clock,timeout_us,&observed);
    if(result==NativeAdrenoRingFencePoisoned)return poison();
    if(result==NativeAdrenoRingFenceArgument) {native_adreno_fence_wait[7]=fence;return kIOReturnBadArgument;}
    if(result==NativeAdrenoRingFenceTimeout) {
        ++stats_.timeouts;
        native_adreno_fence_wait[1]=native_adreno_fence_wait[1]+1;native_adreno_fence_wait[2]=fence;native_adreno_fence_wait[3]=observed;
        native_adreno_fence_wait[4]=ring_->lastSubmitted();native_adreno_fence_wait[5]=ring_->completedFence();
        return kIOReturnTimeout;
    }
    profileNoteComplete(fence);
    IOReturn status=release(*job);
    if(status)return status;
    ++stats_.completed;
    return kIOReturnSuccess;
}
IOReturn NativeAdrenoSubmit::waitAll(const NativeAdrenoRingClock *clock,uint32_t timeout_us)
{
    if(!attached_ || poisoned_)return kIOReturnNotReady;
    while(outstanding()) {
        Job *oldest=nullptr;
        for(auto &job:jobs_)if(job.active && (!oldest || job.fence-oldest->fence>0x80000000u))oldest=&job;
        const IOReturn status=wait(oldest->fence,clock,timeout_us);
        if(status)return status;
    }
    return kIOReturnSuccess;
}
IOReturn NativeAdrenoSubmit::waitRange(uint32_t first,uint32_t count,const NativeAdrenoRingClock *clock,uint32_t timeout_us)
{
    if(!attached_ || poisoned_)return kIOReturnNotReady;
    while(rangeOutstanding(first,count)) {
        Job *oldest=nullptr;
        for(auto &job:jobs_)if(job.active && job.queue-first<count && (!oldest || job.fence-oldest->fence>0x80000000u))oldest=&job;
        const IOReturn status=wait(oldest->fence,clock,timeout_us);
        if(status)return status;
    }
    return kIOReturnSuccess;
}
IOReturn NativeAdrenoSubmit::retireCompleted()
{
    if (!attached_ || poisoned_) {
        return kIOReturnNotReady;
    }

    Job *completed[SubmitMaxJobs];
    size_t count = 0;
    for (auto &job : jobs_) {
        if (!job.active) {
            continue;
        }

        const NativeAdrenoRingFence state = ring_->observeFence(job.fence, nullptr, 0, nullptr);
        if (state == NativeAdrenoRingFencePoisoned) {
            return poison();
        }
        if (state != NativeAdrenoRingFenceComplete) {
            continue;
        }

        profileNoteComplete(job.fence);
        completed[count++] = &job;
    }
    if (!count) {
        return kIOReturnSuccess;
    }

    if (vm_->acquireImported()) {
        return poison();
    }

    for (size_t i = 0; i < count; ++i) {
        const IOReturn status = release(*completed[i], false);
        if (status) {
            return status;
        }

        ++stats_.completed;
    }

    return kIOReturnSuccess;
}
