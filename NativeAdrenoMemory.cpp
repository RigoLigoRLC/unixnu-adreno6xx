/* SPDX-License-Identifier: MIT */
#include "NativeAdrenoMemory.h"
/* Where a call that never returns is stuck: the free/unmap/release path stores a stage number here, the board page shows it. */
extern "C" {volatile uint32_t native_adreno_trace_stage;}
#include <IOKit/IOBufferMemoryDescriptor.h>
#include <IOKit/IOLib.h>
#include <kern/task.h>

/* Stage rows are allocation, prepare, span discovery, zeroing, initial clean,
 * WC attribute setting, WC reassertion, attribute restoration, complete and
 * descriptor release. Each row records calls, total us, maximum us and bytes. */
extern "C" {
extern uint64_t (*native_adreno_vm_now_us)(void);
extern unsigned int memorystatus_available_pages;
extern volatile uint64_t native_adreno_cache_stats[5];
void proc_selfname(char *,int);
volatile uint64_t native_adreno_memory_stage_stats[10][4];
/* hits, misses, insertions, evictions, rejected insertions, cached objects,
 * cached bytes, peak cached bytes, pressure drains, allocation-failure drains,
 * pages restored to WC on reuse, quarantined cache entries. */
volatile uint64_t native_adreno_backing_cache_stats[12];
}

namespace {
enum MemoryStage : unsigned {
    MemoryAllocate,
    MemoryPrepare,
    MemorySpans,
    MemoryZero,
    MemoryClean,
    MemorySetAttributes,
    MemoryReassertAttributes,
    MemoryRestoreAttributes,
    MemoryComplete,
    MemoryDescriptorRelease
};

/* MemoryStageTime accumulates one operation's duration in the bounded stage
 * table. The caller supplies the number of bytes handled by that operation. */
class MemoryStageTime final {
    unsigned stage_;
    size_t bytes_;
    uint64_t began_;
public:
    MemoryStageTime(MemoryStage stage,size_t bytes):stage_(stage),bytes_(bytes),
        began_(native_adreno_vm_now_us?native_adreno_vm_now_us():0)
    {
    }

    ~MemoryStageTime()
    {
        const uint64_t ended=native_adreno_vm_now_us?native_adreno_vm_now_us():0;
        const uint64_t elapsed=began_ && ended>=began_?ended-began_:0;
        volatile uint64_t *row=native_adreno_memory_stage_stats[stage_];
        row[0]=row[0]+1;
        row[1]=row[1]+elapsed;
        if(elapsed>row[2]) {
            row[2]=elapsed;
        }
        row[3]=row[3]+bytes_;
    }
};

enum : size_t {
    BackingCacheEntries=64,
    BackingCacheBytes=64u*1024u*1024u,
    BackingCacheObjectBytes=16u*1024u*1024u,
    BackingCacheLowPages=32768
};

/* CachedBacking owns storage after all user and GPU mappings are removed.
 * sequence orders successful insertions so eviction releases the oldest one. */
struct CachedBacking {
    NativeAdrenoBackingStorage storage;
    uint64_t sequence=0;
};

CachedBacking backing_cache[BackingCacheEntries];
size_t backing_cache_bytes=0,backing_cache_count=0;
uint64_t backing_cache_sequence=0;
NativeAdrenoMemoryDiagnostics memory_diagnostics;

enum BackingMissReason : unsigned {
    BackingMissEmpty,
    BackingMissSize,
    BackingMissAttributes,
    BackingMissQuarantined
};

enum BackingRejectReason : unsigned {
    BackingRejectNotOwned,
    BackingRejectNotPrepared,
    BackingRejectNotWriteCombined,
    BackingRejectQuarantined,
    BackingRejectNoSpans,
    BackingRejectZeroSize,
    BackingRejectOversized,
    BackingRejectNoEvictableEntry,
    BackingRejectNoSlot,
    BackingRejectImported,
    BackingRejectPooled,
    BackingRejectPressure,
    BackingRejectBusy
};

unsigned backingSizeBucket(size_t bytes)
{
    uint64_t upper=4096;
    for(unsigned index=0;index+1<NativeAdrenoMemorySizeBucketCount;++index) {
        if(bytes<=upper) {
            return index;
        }
        upper*=2;
    }
    return NativeAdrenoMemorySizeBucketCount-1;
}

void noteBackingRequest(size_t bytes,bool reuse)
{
    NativeAdrenoMemoryBucketStats &bucket=memory_diagnostics.buckets[backingSizeBucket(bytes)];
    ++bucket.requests;
    bucket.bytes+=bytes;
    if(reuse) {
        ++bucket.lookups;
    } else {
        ++bucket.bypassed;
    }
}

void noteBackingReject(BackingRejectReason reason)
{
    ++memory_diagnostics.insert_reject[reason];
    native_adreno_backing_cache_stats[4]=native_adreno_backing_cache_stats[4]+1;
}

/* MemorySlowTime retains the storage attributes needed after a descriptor is
 * released. Allocation and import refresh those attributes when they finish,
 * so their records include the physical spans found during the operation. */
class MemorySlowTime final {
    const NativeAdrenoBackingStorage *storage_;
    const bool *pooled_;
    NativeAdrenoMemorySlowEvent event_;
    uint64_t began_;
    bool refresh_;

    void captureStorage()
    {
        event_.spans=storage_->span_count;
        event_.source=storage_->diagnostic_source;
        event_.cache_result=storage_->diagnostic_cache_result;
        if(storage_->contiguous) {
            event_.flags|=NativeAdrenoMemoryDiagnosticContiguous;
        }
        if(storage_->write_combined) {
            event_.flags|=NativeAdrenoMemoryDiagnosticWriteCombined;
        }
        if(storage_->quarantined) {
            event_.flags|=NativeAdrenoMemoryDiagnosticQuarantined;
        }
        if(storage_->diagnostic_source==NativeAdrenoMemorySource::Imported) {
            event_.flags|=NativeAdrenoMemoryDiagnosticImported;
        }
        if(pooled_ && *pooled_) {
            event_.flags|=NativeAdrenoMemoryDiagnosticPooled;
        }
    }

public:
    MemorySlowTime(NativeAdrenoMemorySlowOperation operation,const NativeAdrenoBackingStorage &storage,
        size_t bytes,unsigned bits,uint32_t flags=0,bool refresh=false,const bool *pooled=nullptr):
        storage_(&storage),pooled_(pooled),began_(native_adreno_vm_now_us?native_adreno_vm_now_us():0),refresh_(refresh)
    {
        event_.operation=operation;
        event_.bytes=bytes;
        event_.output_bits=bits;
        event_.flags=flags;
        captureStorage();
    }

    ~MemorySlowTime()
    {
        const uint64_t ended=native_adreno_vm_now_us?native_adreno_vm_now_us():0;
        if(!began_ || ended<began_ || ended-began_<8000) {
            return;
        }
        if(refresh_ && storage_->bytes) {
            captureStorage();
        }
        event_.sequence=++memory_diagnostics.slow_total;
        event_.sampled_us=ended;
        event_.elapsed_us=ended-began_;
        event_.cached_bytes=backing_cache_bytes;
        event_.cached_objects=static_cast<uint32_t>(backing_cache_count);
        event_.available_pages=memorystatus_available_pages;
        proc_selfname(event_.process,sizeof(event_.process));
        memory_diagnostics.slow[(event_.sequence-1)%NativeAdrenoMemorySlowEventCount]=event_;
        if(memory_diagnostics.slow_count<NativeAdrenoMemorySlowEventCount) {
            ++memory_diagnostics.slow_count;
        }
    }
};

bool backingPressure()
{
    return memorystatus_available_pages<BackingCacheLowPages;
}

void noteBackingCacheSize()
{
    native_adreno_backing_cache_stats[5]=backing_cache_count;
    native_adreno_backing_cache_stats[6]=backing_cache_bytes;
    if(backing_cache_bytes>native_adreno_backing_cache_stats[7]) {
        native_adreno_backing_cache_stats[7]=backing_cache_bytes;
    }
    memory_diagnostics.cached_objects=backing_cache_count;
    memory_diagnostics.cached_bytes=backing_cache_bytes;
    if(backing_cache_count>memory_diagnostics.peak_objects) {
        memory_diagnostics.peak_objects=backing_cache_count;
    }
    if(backing_cache_bytes>memory_diagnostics.peak_bytes) {
        memory_diagnostics.peak_bytes=backing_cache_bytes;
    }
}

IOReturn destroyOwnedStorage(NativeAdrenoBackingStorage &storage)
{
    if(storage.quarantined) {
        return kIOReturnBusy;
    }
    const uint32_t release_flags=storage.write_combined?NativeAdrenoMemoryDiagnosticWriteCombined:0;
    if(storage.write_combined) {
        MemoryStageTime elapsed(MemoryRestoreAttributes,storage.bytes);
        const IOReturn status=NativeAdrenoSetPagesCacheMode(storage.spans,storage.span_count,false);
        if(status) {
            storage.quarantined=true;
            return status;
        }
        storage.write_combined=false;
    }
    if(storage.descriptor) {
        if(storage.prepared) {
            MemoryStageTime elapsed(MemoryComplete,storage.bytes);
            native_adreno_trace_stage=31;
            const IOReturn status=storage.descriptor->complete();
            native_adreno_trace_stage=32;
            if(status) {
                storage.quarantined=true;
                return status;
            }
            storage.prepared=false;
        }
        {
            MemoryStageTime elapsed(MemoryDescriptorRelease,storage.bytes);
            MemorySlowTime slow(NativeAdrenoMemorySlowOperation::OwnedDescriptorRelease,storage,
                storage.bytes,storage.output_bits,release_flags);
            native_adreno_trace_stage=33;
            storage.descriptor->release();
            native_adreno_trace_stage=34;
        }
        storage.descriptor=nullptr;
    }
    if(storage.spans) {
        IODeleteData(storage.spans,NativeAdrenoPhysicalSpan,storage.span_capacity);
    }
    storage={};
    return kIOReturnSuccess;
}

IOReturn evictBacking(CachedBacking &entry,unsigned reason)
{
    const size_t bytes=entry.storage.bytes;
    const bool quarantined=entry.storage.quarantined;
    ++memory_diagnostics.eviction_attempts[reason];
    const IOReturn status=destroyOwnedStorage(entry.storage);
    if(status) {
        ++memory_diagnostics.eviction_failures[reason];
        if(!quarantined && entry.storage.quarantined) {
            native_adreno_backing_cache_stats[11]=native_adreno_backing_cache_stats[11]+1;
        }
        return status;
    }
    entry.sequence=0;
    backing_cache_bytes-=bytes;
    --backing_cache_count;
    ++memory_diagnostics.evictions[reason];
    memory_diagnostics.evicted_bytes[reason]+=bytes;
    native_adreno_backing_cache_stats[3]=native_adreno_backing_cache_stats[3]+1;
    noteBackingCacheSize();
    return kIOReturnSuccess;
}

bool takeBacking(size_t bytes,unsigned bits,bool contiguous,NativeAdrenoBackingStorage &storage)
{
    NativeAdrenoMemoryBucketStats &bucket=memory_diagnostics.buckets[backingSizeBucket(bytes)];
    bool any=false,sized=false,quarantined_match=false;
    for(auto &entry:backing_cache) {
        const NativeAdrenoBackingStorage &candidate=entry.storage;
        if(!candidate.descriptor) {
            continue;
        }
        any=true;
        if(candidate.bytes!=bytes) {
            continue;
        }
        sized=true;
        if(candidate.output_bits!=bits || candidate.contiguous!=contiguous) {
            continue;
        }
        if(candidate.quarantined) {
            quarantined_match=true;
            continue;
        }
        storage=candidate;
        storage.diagnostic_cache_result=NativeAdrenoMemoryCacheResult::Hit;
        entry={};
        backing_cache_bytes-=bytes;
        --backing_cache_count;
        native_adreno_backing_cache_stats[0]=native_adreno_backing_cache_stats[0]+1;
        ++bucket.hits;
        noteBackingCacheSize();
        return true;
    }
    native_adreno_backing_cache_stats[1]=native_adreno_backing_cache_stats[1]+1;
    ++bucket.misses;
    const BackingMissReason reason=quarantined_match?BackingMissQuarantined:
        sized?BackingMissAttributes:any?BackingMissSize:BackingMissEmpty;
    ++bucket.miss_reason[reason];
    storage.diagnostic_cache_result=NativeAdrenoMemoryCacheResult::Miss;
    return false;
}

bool cacheBacking(NativeAdrenoBackingStorage &storage)
{
    if(!storage.descriptor || !storage.prepared || !storage.write_combined || storage.quarantined ||
        !storage.spans || !storage.span_count || !storage.bytes || storage.bytes>BackingCacheObjectBytes) {
        const BackingRejectReason reason=!storage.descriptor?BackingRejectNotOwned:
            !storage.prepared?BackingRejectNotPrepared:!storage.write_combined?BackingRejectNotWriteCombined:
            storage.quarantined?BackingRejectQuarantined:!storage.spans || !storage.span_count?BackingRejectNoSpans:
            !storage.bytes?BackingRejectZeroSize:BackingRejectOversized;
        noteBackingReject(reason);
        return false;
    }
    while(backing_cache_count==BackingCacheEntries || backing_cache_bytes>BackingCacheBytes-storage.bytes) {
        CachedBacking *oldest=nullptr;
        for(auto &entry:backing_cache) {
            if(entry.storage.descriptor && !entry.storage.quarantined &&
                (!oldest || entry.sequence<oldest->sequence)) {
                oldest=&entry;
            }
        }
        if(!oldest) {
            noteBackingReject(BackingRejectNoEvictableEntry);
            return false;
        }
        const unsigned reason=backing_cache_count==BackingCacheEntries?0:1;
        (void)evictBacking(*oldest,reason);
    }
    for(auto &entry:backing_cache) {
        if(entry.storage.descriptor) {
            continue;
        }
        entry.storage=storage;
        entry.sequence=++backing_cache_sequence;
        backing_cache_bytes+=storage.bytes;
        ++backing_cache_count;
        storage={};
        native_adreno_backing_cache_stats[2]=native_adreno_backing_cache_stats[2]+1;
        ++memory_diagnostics.insertions;
        noteBackingCacheSize();
        return true;
    }
    noteBackingReject(BackingRejectNoSlot);
    return false;
}
}

IOReturn NativeAdrenoMemory::trimBackingCache(NativeAdrenoBackingTrimReason reason)
{
    IOReturn result=kIOReturnSuccess;
    for(auto &entry:backing_cache) {
        if(!entry.storage.descriptor) {
            continue;
        }
        const IOReturn status=evictBacking(entry,static_cast<unsigned>(reason));
        if(status && !result) {
            result=status;
        }
    }
    return result;
}

void NativeAdrenoMemory::copyDiagnostics(NativeAdrenoMemoryDiagnostics *out)
{
    if(!out) {
        return;
    }
    *out=memory_diagnostics;
    uint64_t upper=4096;
    for(unsigned index=0;index+1<NativeAdrenoMemorySizeBucketCount;++index) {
        out->buckets[index].upper_bound_bytes=upper;
        upper*=2;
    }
    for(unsigned index=0;index<out->slow_count;++index) {
        const uint64_t retained=out->slow_total-out->slow_count+index;
        out->slow[index]=memory_diagnostics.slow[retained%NativeAdrenoMemorySlowEventCount];
    }
}

static bool outputWidth(unsigned bits)
{return bits==32 || bits==36 || bits==40 || bits==42 || bits==44 || bits==48;}
/* Where the last import stopped (10..19, 19 = done), and for a refused page the physical address and offset: the board log shows them. */
enum : IOReturn {kNativeAdrenoProtectionFailure=2};   /* KERN_PROTECTION_FAILURE, as prepare() reports a range that cannot be wired for writing */
extern "C" {volatile uint32_t native_adreno_pool_mib=0,native_adreno_pool_status=0;volatile uint32_t native_adreno_import_stage;volatile uint64_t native_adreno_import_fail_pa,native_adreno_import_fail_offset;}
/* The pool (see reservePool): a free list of page extents over one contiguous block; best fit, neighbours merged on release. */
extern "C" {volatile uint32_t native_adreno_pool_stats[8];}
/* The pool's write-combine (writeCombinePool): bit 31 once tried, the low bits its status. */
extern "C" {volatile uint32_t native_adreno_pool_wc;}   /* [0] pool MiB, [1] BOs from the pool, [2] fell back, [3] MiB in use, [4] peak MiB, [5] scattered BOs, [6] their spans, [7] import spans */
namespace {
struct PoolExtent {size_t offset,bytes;};
struct Pool {
    IOBufferMemoryDescriptor *descriptor=nullptr;
    unsigned char *virt=nullptr;uint64_t physical=0;size_t bytes=0,used=0,peak=0;
    PoolExtent free[512];unsigned count=0;
    bool write_combined=false;
} pool;
bool poolTake(size_t bytes,size_t *offset) {
    int best=-1;
    for(unsigned i=0;i<pool.count;++i)if(pool.free[i].bytes>=bytes && (best<0 || pool.free[i].bytes<pool.free[best].bytes))best=(int)i;
    if(best<0)return false;
    *offset=pool.free[best].offset;
    pool.free[best].offset+=bytes;pool.free[best].bytes-=bytes;
    if(!pool.free[best].bytes) {for(unsigned i=(unsigned)best;i+1<pool.count;++i)pool.free[i]=pool.free[i+1];--pool.count;}
    pool.used+=bytes;if(pool.used>pool.peak)pool.peak=pool.used;
    native_adreno_pool_stats[3]=(uint32_t)(pool.used>>20);native_adreno_pool_stats[4]=(uint32_t)(pool.peak>>20);
    return true;
}
void poolGive(size_t offset,size_t bytes) {
    unsigned at=0;while(at<pool.count && pool.free[at].offset<offset)++at;
    if(pool.count>=512) {pool.used-=bytes;return;}   /* cannot happen with bounded fragmentation; the extent is lost, not reused */
    for(unsigned i=pool.count;i>at;--i)pool.free[i]=pool.free[i-1];
    pool.free[at]={offset,bytes};++pool.count;
    if(at+1<pool.count && pool.free[at].offset+pool.free[at].bytes==pool.free[at+1].offset) {
        pool.free[at].bytes+=pool.free[at+1].bytes;
        for(unsigned i=at+1;i+1<pool.count;++i)pool.free[i]=pool.free[i+1];
        --pool.count;
    }
    if(at && pool.free[at-1].offset+pool.free[at-1].bytes==pool.free[at].offset) {
        pool.free[at-1].bytes+=pool.free[at].bytes;
        for(unsigned i=at;i+1<pool.count;++i)pool.free[i]=pool.free[i+1];
        --pool.count;
    }
    pool.used-=bytes;native_adreno_pool_stats[3]=(uint32_t)(pool.used>>20);
}
}
size_t NativeAdrenoMemory::poolBytes() {return pool.bytes;}
/* int-152: a user BO's pages changed attribute one pmap_set_cache_attributes call at a time (each walks the page's mappings, flushes the
 * TLB and cleans the page), on allocation and again on release, under the connection lock: 3 ms a BO on average and 90-130 ms for a
 * surface-sized one, while every other GPU call of every client waited. The pool is the BOs' own block and nothing else maps it, so
 * its pages take the attribute once here and keep it while the pool lives. */
IOReturn NativeAdrenoMemory::writeCombinePool()
{
    if(!pool.descriptor || !pool.virt || !pool.bytes)return kIOReturnNotReady;
    if(pool.write_combined)return kIOReturnSuccess;
    IOReturn status=NativeAdrenoMaintainOwnedPages(pool.virt,pool.bytes,NativeAdrenoCleanInvalidate);
    if(status)return status;
    const NativeAdrenoPhysicalSpan span={0,pool.physical,pool.bytes};
    status=NativeAdrenoSetPagesCacheMode(&span,1,true);
    if(status)return status;
    pool.write_combined=true;return kIOReturnSuccess;
}
IOReturn NativeAdrenoMemory::reservePool(unsigned bits,size_t bytes)
{
    if(pool.descriptor)return kIOReturnBusy;
    if(!bytes || (bytes&4095) || !outputWidth(bits))return kIOReturnBadArgument;
    NativeAdrenoCpuState cpu{};
    IOReturn status=NativeAdrenoQueryCpu4K(&cpu);if(status!=kIOReturnSuccess)return status;
    const uint64_t limit=UINT64_C(1)<<bits;
    IOBufferMemoryDescriptor *descriptor=IOBufferMemoryDescriptor::inTaskWithPhysicalMask(kernel_task,
        static_cast<IOOptionBits>(kIODirectionInOut)|static_cast<IOOptionBits>(kIOMemoryMapperNone)|kIOMemoryPhysicallyContiguous,
        bytes,(limit-1)&~UINT64_C(4095));
    if(!descriptor)return kIOReturnNoMemory;
    status=descriptor->prepare();
    if(status!=kIOReturnSuccess) {descriptor->release();return status;}
    IOByteCount length=0,tail=0;
    const addr64_t first=descriptor->getPhysicalSegment(0,&length,kIOMemoryMapperNone);
    const addr64_t last=descriptor->getPhysicalSegment(bytes-4096,&tail,kIOMemoryMapperNone);
    unsigned char *virt=static_cast<unsigned char *>(descriptor->getBytesNoCopy());
    if(!first || (first&4095) || !virt || ((uintptr_t)virt&4095) || last!=first+bytes-4096 || first>=limit || bytes>limit-first ||
        descriptor->getLength()!=bytes) {
        (void)descriptor->complete();descriptor->release();return kIOReturnUnsupported;
    }
    pool.descriptor=descriptor;pool.virt=virt;pool.physical=first;pool.bytes=bytes;pool.used=pool.peak=0;
    pool.free[0]={0,bytes};pool.count=1;
    native_adreno_pool_stats[0]=(uint32_t)(bytes>>20);
    return kIOReturnSuccess;
}
IOReturn NativeAdrenoMemory::allocate(size_t bytes,unsigned bits,bool contiguous,size_t budget,bool pooled,
    bool reuse_write_combined)
{
    if(backed() || storage_.spans || storage_.quarantined) {
        return kIOReturnBusy;
    }
    if(!bytes || (bytes&4095) || !outputWidth(bits) || !budget || budget>256u*1024u*1024u) {
        return kIOReturnBadArgument;
    }
    if(bytes>budget) {
        return kIOReturnNoResources;
    }
    const NativeAdrenoMemorySource source=pooled?NativeAdrenoMemorySource::UserOwned:NativeAdrenoMemorySource::KernelOwned;
    controlled_cpu_views_=true;
    storage_.diagnostic_source=source;
    storage_.diagnostic_cache_result=NativeAdrenoMemoryCacheResult::NotRequested;
    storage_.output_bits=bits;
    storage_.contiguous=contiguous;
    MemorySlowTime slow(NativeAdrenoMemorySlowOperation::Allocation,storage_,bytes,bits,
        reuse_write_combined?NativeAdrenoMemoryDiagnosticReuseRequested:0,true,&pooled_);

    NativeAdrenoCpuState cpu{};
    IOReturn status=NativeAdrenoQueryCpu4K(&cpu);
    if(status) {
        return status;
    }
    noteBackingRequest(bytes,reuse_write_combined);
    if(backing_cache_count && backingPressure()) {
        native_adreno_backing_cache_stats[8]=native_adreno_backing_cache_stats[8]+1;
        (void)trimBackingCache(NativeAdrenoBackingTrimReason::MemoryPressure);
    }

    if(reuse_write_combined && takeBacking(bytes,bits,contiguous,storage_)) {
        storage_.diagnostic_source=source;
        uint32_t reset=0;
        {
            MemoryStageTime elapsed(MemoryReassertAttributes,bytes);
            status=NativeAdrenoReassertWriteCombined(storage_.spans,storage_.span_count,&reset);
        }
        native_adreno_backing_cache_stats[10]=native_adreno_backing_cache_stats[10]+reset;
        if(status) {
            storage_.quarantined=true;
            return status;
        }
        {
            MemoryStageTime elapsed(MemoryZero,bytes);
            bzero(storage_.mapping,bytes);
            __asm__ volatile("dsb sy":::"memory");
        }
        return kIOReturnSuccess;
    }

    const uint64_t limit=UINT64_C(1)<<bits;
    if(pooled && pool.descriptor && bytes<=pool.bytes) {
        size_t offset=0;
        if(poolTake(bytes,&offset)) {
            const uint64_t physical=pool.physical+offset;
            if(physical>=limit || bytes>limit-physical) {
                poolGive(offset,bytes);
            } else {
                storage_.spans=IONewData(NativeAdrenoPhysicalSpan,1);
                if(!storage_.spans) {
                    poolGive(offset,bytes);
                    return kIOReturnNoMemory;
                }
                storage_.span_capacity=1;
                storage_.span_count=1;
                storage_.spans[0]={0,physical,bytes};
                storage_.mapping=pool.virt+offset;
                storage_.bytes=bytes;
                storage_.prepared=true;
                pooled_=true;
                pool_offset_=offset;
                {
                    MemoryStageTime elapsed(MemoryZero,bytes);
                    bzero(storage_.mapping,bytes);
                    if(pool.write_combined) {
                        __asm__ volatile("dsb sy":::"memory");
                    }
                }
                if(!pool.write_combined) {
                    MemoryStageTime elapsed(MemoryClean,bytes);
                    status=NativeAdrenoMaintainOwnedPages(storage_.mapping,bytes,NativeAdrenoClean);
                }
                if(status) {
                    (void)release();
                    return status;
                }
                native_adreno_pool_stats[1]=native_adreno_pool_stats[1]+1;
                return kIOReturnSuccess;
            }
        }
        native_adreno_pool_stats[2]=native_adreno_pool_stats[2]+1;
    }

    IOOptionBits options=static_cast<IOOptionBits>(kIODirectionInOut)|static_cast<IOOptionBits>(kIOMemoryMapperNone);
    if(reuse_write_combined) {
        options|=kIOMapWriteCombineCache;
    }
    if(contiguous) {
        options|=kIOMemoryPhysicallyContiguous;
    }
    {
        MemoryStageTime elapsed(MemoryAllocate,bytes);
        storage_.descriptor=IOBufferMemoryDescriptor::inTaskWithPhysicalMask(kernel_task,options,bytes,(limit-1)&~UINT64_C(4095));
    }
    if(!storage_.descriptor && backing_cache_count) {
        native_adreno_backing_cache_stats[9]=native_adreno_backing_cache_stats[9]+1;
        (void)trimBackingCache(NativeAdrenoBackingTrimReason::AllocationRetry);
        MemoryStageTime elapsed(MemoryAllocate,bytes);
        storage_.descriptor=IOBufferMemoryDescriptor::inTaskWithPhysicalMask(kernel_task,options,bytes,(limit-1)&~UINT64_C(4095));
    }
    if(!storage_.descriptor) {
        return kIOReturnNoMemory;
    }
    storage_.descriptor_write_combined=reuse_write_combined;
    {
        MemoryStageTime elapsed(MemoryPrepare,bytes);
        status=storage_.descriptor->prepare();
    }
    if(status) {
        MemoryStageTime elapsed(MemoryDescriptorRelease,bytes);
        MemorySlowTime slowRelease(NativeAdrenoMemorySlowOperation::OwnedDescriptorRelease,storage_,bytes,bits);
        storage_.descriptor->release();
        storage_.descriptor=nullptr;
        return status;
    }
    storage_.prepared=true;
    storage_.bytes=bytes;
    storage_.output_bits=bits;
    storage_.contiguous=contiguous;
    storage_.mapping=storage_.descriptor->getBytesNoCopy();
    status=kIOReturnUnsupported;
    if(!storage_.mapping || ((uintptr_t)storage_.mapping&4095) || bytes>UINTPTR_MAX-(uintptr_t)storage_.mapping ||
        storage_.descriptor->getLength()!=bytes) {
        goto failed;
    }
    storage_.span_capacity=bytes/4096;
    storage_.spans=IONewData(NativeAdrenoPhysicalSpan,storage_.span_capacity);
    if(!storage_.spans) {
        status=kIOReturnNoMemory;
        goto failed;
    }
    {
        MemoryStageTime elapsed(MemorySpans,bytes);
        for(size_t offset=0;offset<bytes;offset+=4096) {
            IOByteCount length=0;
            const addr64_t physical=storage_.descriptor->getPhysicalSegment(offset,&length,kIOMemoryMapperNone);
            if(!physical || (physical&4095) || length<4096 || physical>=limit || 4096>limit-physical) {
                status=kIOReturnUnsupported;
                goto failed;
            }
            if(contiguous && offset && physical!=storage_.spans[0].physical+offset) {
                status=kIOReturnUnsupported;
                goto failed;
            }
            if(storage_.span_count &&
                physical==storage_.spans[storage_.span_count-1].physical+storage_.spans[storage_.span_count-1].bytes) {
                storage_.spans[storage_.span_count-1].bytes+=4096;
            } else {
                storage_.spans[storage_.span_count++]={offset,physical,4096};
            }
        }
    }
    storage_.write_combined=storage_.descriptor_write_combined;
    if(storage_.write_combined) {
        __atomic_fetch_add(&native_adreno_cache_stats[4],bytes/4096,__ATOMIC_RELAXED);
    }
    {
        MemoryStageTime elapsed(MemoryZero,bytes);
        bzero(storage_.mapping,bytes);
    }
    {
        MemoryStageTime elapsed(MemoryClean,bytes);
        status=NativeAdrenoMaintainOwnedPages(storage_.mapping,bytes,NativeAdrenoClean);
    }
    if(status) {
        goto failed;
    }
    native_adreno_pool_stats[5]=native_adreno_pool_stats[5]+1;
    native_adreno_pool_stats[6]=native_adreno_pool_stats[6]+(uint32_t)storage_.span_count;
    return kIOReturnSuccess;

failed:
    (void)release();
    return status;
}
/* An import holds the pages through a memory entry of its own (kIOMemoryPersistent), which references the VM object: the owner may unmap
 * its range while the GPU still uses it (Core Animation frees an image's Shmem once its no-copy buffer is released, before the runtime frees
 * the import), and the object then lives on until the import completes instead of being torn down under a wired UPL (int-233..235: the render
 * thread stuck in vm_deallocate). A range no memory entry can be made of is imported as before; [0] counts the persistent imports, [1] those. */
extern "C" {volatile uint32_t native_adreno_import_persistent[2];}
static IOMemoryDescriptor *importRange(uint64_t address,size_t bytes,IODirection direction,void *task)
{
    const IOOptionBits options=static_cast<IOOptionBits>(direction)|static_cast<IOOptionBits>(kIOMemoryMapperNone);
    IOMemoryDescriptor *range=IOMemoryDescriptor::withAddressRange(address,bytes,options|kIOMemoryPersistent,static_cast<task_t>(task));
    if(range) {native_adreno_import_persistent[0]=native_adreno_import_persistent[0]+1;return range;}
    range=IOMemoryDescriptor::withAddressRange(address,bytes,options,static_cast<task_t>(task));
    if(range)native_adreno_import_persistent[1]=native_adreno_import_persistent[1]+1;
    return range;
}
IOReturn NativeAdrenoMemory::importTask(void *task,uint64_t address,size_t bytes,unsigned bits,size_t budget)
{
    native_adreno_import_stage=10;
    if(backed() || storage_.spans || storage_.quarantined)return kIOReturnBusy;
    native_adreno_import_stage=11;
    if(!task || !address || (address&4095) || !bytes || (bytes&4095) || address>UINT64_MAX-bytes || !outputWidth(bits) ||
        !budget || budget>256u*1024u*1024u)return kIOReturnBadArgument;
    native_adreno_import_stage=12;
    if(bytes>budget)return kIOReturnNoResources;
    storage_.diagnostic_source=NativeAdrenoMemorySource::Imported;
    storage_.diagnostic_cache_result=NativeAdrenoMemoryCacheResult::NotRequested;
    storage_.output_bits=bits;
    MemorySlowTime slow(NativeAdrenoMemorySlowOperation::Import,storage_,bytes,bits,
        NativeAdrenoMemoryDiagnosticImported,true);
    NativeAdrenoCpuState cpu{};
    native_adreno_import_stage=13;
    IOReturn status=NativeAdrenoQueryCpu4K(&cpu);if(status!=kIOReturnSuccess)return status;
    const uint64_t limit=UINT64_C(1)<<bits;
    imported_=importRange(address,bytes,kIODirectionInOut,task);
    native_adreno_import_stage=14;
    if(!imported_)return kIOReturnNoMemory;
    native_adreno_import_stage=15;
    status=imported_->prepare();
    if(status==kNativeAdrenoProtectionFailure) {
        /* Read-only memory of the owner (a mapped file, constant data): cannot be pinned for writing. Pin it for reading only; the GPU then maps it
         * read-only. Metal's no-copy buffers over such memory are only ever read by shaders. */
        imported_->release();imported_=nullptr;
        imported_=importRange(address,bytes,kIODirectionOut,task);
        if(!imported_)return kIOReturnNoMemory;
        status=imported_->prepare();
        if(status==kIOReturnSuccess)read_only_=true;
    }
    if(status!=kIOReturnSuccess) {imported_->release();imported_=nullptr;return status;}
    storage_.prepared=true;
    storage_.bytes=bytes;
    status=kIOReturnUnsupported;
    native_adreno_import_stage=16;
    if(imported_->getLength()!=bytes)goto failed;
    native_adreno_import_stage=17;
    storage_.span_capacity=bytes/4096;
    storage_.spans=IONewData(NativeAdrenoPhysicalSpan,storage_.span_capacity);
    if(!storage_.spans) {
        status=kIOReturnNoMemory;
        goto failed;
    }
    for(size_t offset=0;offset<bytes;offset+=4096) {
        IOByteCount length=0;
        addr64_t physical=imported_->getPhysicalSegment(offset,&length,kIOMemoryMapperNone);
        if(!physical || (physical&4095) || length<4096 || physical>=limit || 4096>limit-physical)
            {native_adreno_import_stage=18;native_adreno_import_fail_pa=physical;native_adreno_import_fail_offset=offset;
             status=kIOReturnUnsupported;goto failed;}
        if(storage_.span_count &&
            physical==storage_.spans[storage_.span_count-1].physical+storage_.spans[storage_.span_count-1].bytes) {
            storage_.spans[storage_.span_count-1].bytes+=4096;
        } else {
            storage_.spans[storage_.span_count++]={offset,physical,4096};
        }
    }
    cpu_cache_mode_=NativeAdrenoQueryPagesCacheMode(storage_.spans,storage_.span_count);
    native_adreno_import_stage=19;
    native_adreno_pool_stats[7]=native_adreno_pool_stats[7]+(uint32_t)storage_.span_count;
    return kIOReturnSuccess;
failed:
    (void)release();return status;
}
IOReturn NativeAdrenoMemory::makeWriteCombined()
{
    if(!(storage_.descriptor || pooled_) || imported_ || !storage_.prepared || storage_.quarantined ||
        device_references_ || !storage_.mapping || !storage_.span_count) {
        return kIOReturnNotReady;
    }
    if(storage_.write_combined) {
        return kIOReturnSuccess;
    }
    if(pooled_ && pool.write_combined) {
        storage_.write_combined=true;
        return kIOReturnSuccess;
    }
    const bool trace=NativeAdrenoCacheTraceEnabled();
    if(trace) {
        IOLog("NativeAdrenoMemory: clean/invalidate begin cpu=%p bytes=%lx spans=%lx\n",
            storage_.mapping,storage_.bytes,storage_.span_count);
    }
    IOReturn status;
    {
        MemoryStageTime elapsed(MemoryClean,storage_.bytes);
        status=NativeAdrenoMaintainOwnedPages(storage_.mapping,storage_.bytes,NativeAdrenoCleanInvalidate);
    }
    if(trace || status) {
        IOLog("NativeAdrenoMemory: clean/invalidate end status=%08x\n",status);
    }
    if(status)return status;
    if(trace) {
        IOLog("NativeAdrenoMemory: pmap cache attributes begin physical=%llx bytes=%lx\n",
            storage_.spans[0].physical,storage_.bytes);
    }
    {
        MemoryStageTime elapsed(MemorySetAttributes,storage_.bytes);
        status=NativeAdrenoSetPagesCacheMode(storage_.spans,storage_.span_count,true);
    }
    if(trace || status) {
        IOLog("NativeAdrenoMemory: pmap cache attributes end status=%08x\n",status);
    }
    if(status) {
        storage_.quarantined=true;
        return status;
    }
    storage_.write_combined=true;
    return kIOReturnSuccess;
}
IOReturn NativeAdrenoMemory::release(bool cache_backing)
{
    native_adreno_trace_stage=30;
    if(cache_backing) {
        ++memory_diagnostics.insert_attempts;
    }
    if(device_references_ || storage_.quarantined) {
        if(cache_backing) {
            noteBackingReject(storage_.quarantined?BackingRejectQuarantined:BackingRejectBusy);
        }
        return kIOReturnBusy;
    }
    const bool pressure=backingPressure();
    if(pressure && backing_cache_count) {
        native_adreno_backing_cache_stats[8]=native_adreno_backing_cache_stats[8]+1;
        (void)trimBackingCache(NativeAdrenoBackingTrimReason::MemoryPressure);
    }
    if(cache_backing) {
        if(pressure) {
            noteBackingReject(BackingRejectPressure);
        } else if(imported_) {
            noteBackingReject(BackingRejectImported);
        } else if(pooled_) {
            noteBackingReject(BackingRejectPooled);
        } else if(cacheBacking(storage_)) {
            read_only_=false;
            return kIOReturnSuccess;
        }
    }
    if(storage_.write_combined && pooled_ && pool.write_combined) {
        storage_.write_combined=false;
    }
    if(storage_.write_combined && !storage_.descriptor) {
        MemoryStageTime elapsed(MemoryRestoreAttributes,storage_.bytes);
        const IOReturn status=NativeAdrenoSetPagesCacheMode(storage_.spans,storage_.span_count,false);
        if(status) {
            storage_.quarantined=true;
            return status;
        }
        storage_.write_combined=false;
    }
    if(imported_) {
        if(storage_.prepared) {
            MemoryStageTime elapsed(MemoryComplete,storage_.bytes);
            const IOReturn status=imported_->complete();
            if(status) {
                storage_.quarantined=true;
                return status;
            }
            storage_.prepared=false;
        }
        {
            MemoryStageTime elapsed(MemoryDescriptorRelease,storage_.bytes);
            MemorySlowTime slow(NativeAdrenoMemorySlowOperation::ImportedDescriptorRelease,storage_,
                storage_.bytes,storage_.output_bits);
            imported_->release();
        }
        imported_=nullptr;
        cpu_cache_mode_=NativeAdrenoCpuCacheMode::Unknown;
    }
    if(pooled_) {
        poolGive(pool_offset_,storage_.bytes);
        pooled_=false;
        storage_.prepared=false;
        pool_offset_=0;
    }
    const IOReturn status=destroyOwnedStorage(storage_);
    if(!status) {
        read_only_=false;
    }
    return status;
}
IOReturn NativeAdrenoMemory::holdForDevice()
{
    if(!backed() || !storage_.prepared || !storage_.span_count || storage_.quarantined)return kIOReturnNotReady;
    if(device_references_==UINT32_MAX)return kIOReturnNoResources;
    ++device_references_;return kIOReturnSuccess;
}
IOReturn NativeAdrenoMemory::retireAfterQuiescence()
{
    if(!device_references_)return kIOReturnBadArgument;
    if(storage_.quarantined)return kIOReturnNotReady;
    --device_references_;return kIOReturnSuccess;
}
IOReturn NativeAdrenoMemory::publishForDevice(size_t offset,size_t bytes,bool include_write_combined)
{
    if(!backed() || !storage_.prepared || storage_.quarantined)return kIOReturnNotReady;
    if(!bytes || offset>storage_.bytes || bytes>storage_.bytes-offset)return kIOReturnBadArgument;
    if(imported_)return maintainImported(offset,bytes,NativeAdrenoClean);
    if(storage_.write_combined) {
        if(include_write_combined) {
            return maintainImported(offset,bytes,NativeAdrenoClean);
        }

        __asm__ volatile("dsb st" ::: "memory");
        return kIOReturnSuccess;
    }
    return NativeAdrenoMaintainOwnedPages(static_cast<unsigned char *>(storage_.mapping)+offset,bytes,NativeAdrenoClean);
}
IOReturn NativeAdrenoMemory::acquireDeviceWrites(size_t offset,size_t bytes,bool submitted_range)
{
    if(!backed() || !storage_.prepared || storage_.quarantined)return kIOReturnNotReady;
    if(!bytes || offset>storage_.bytes || bytes>storage_.bytes-offset)return kIOReturnBadArgument;
    if(imported_) {
        return maintainImported(offset,bytes,submitted_range?NativeAdrenoInvalidate:NativeAdrenoCleanInvalidate);
    }
    if(storage_.write_combined) {
        __asm__ volatile("dsb ld" ::: "memory");
        return kIOReturnSuccess;
    }
    return NativeAdrenoMaintainOwnedPages(static_cast<unsigned char *>(storage_.mapping)+offset,bytes,NativeAdrenoInvalidate);
}
/* Imported pages have no kernel mapping here (and write-combined ones only a non-cacheable one); maintain each span's part of
 * [offset, offset+bytes) by physical address. */
IOReturn NativeAdrenoMemory::maintainImported(size_t offset,size_t bytes,NativeAdrenoCacheOperation op)
{
    return NativeAdrenoMaintainPhysicalSpans(storage_.spans,storage_.span_count,offset,bytes,op);
}
