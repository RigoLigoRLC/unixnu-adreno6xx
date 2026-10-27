/* SPDX-License-Identifier: MIT */
#ifndef NATIVE_ADRENO_MEMORY_H
#define NATIVE_ADRENO_MEMORY_H
#include <IOKit/IOTypes.h>
#include <stddef.h>
#include <stdint.h>
class IOBufferMemoryDescriptor;
class IOMemoryDescriptor;
struct NativeAdrenoPhysicalSpan {uint64_t offset,physical,bytes;};
struct NativeAdrenoCpuState {
    uint64_t tcr,ttbr0,ttbr1,sctlr;
    uint64_t kernel_page_bytes,kernel_map_page_bytes;
};
IOReturn NativeAdrenoQueryCpu4K(NativeAdrenoCpuState *);
/* The native-adreno-cache-trace boot argument enables per-BO and per-span cache logs. */
bool NativeAdrenoCacheTraceEnabled();
enum NativeAdrenoCacheOperation {NativeAdrenoClean,NativeAdrenoCleanInvalidate,NativeAdrenoInvalidate};
size_t NativeAdrenoCpuCacheLineBytes();
/* The byte interval is expanded only to intersecting CPU cache lines. The
 * caller excludes conflicting CPU and GPU access to those complete lines. */
IOReturn NativeAdrenoMaintainOwnedPages(void *,size_t,NativeAdrenoCacheOperation);
/* The same over whole physical pages of DRAM, through the kernel's physical aperture: data cache lines are physically tagged, so this
 * reaches lines another task's mapping left (an imported IOSurface). Apple's sys_dcache_flush is only a DSB, so user space cannot. */
IOReturn NativeAdrenoMaintainPhysicalPages(uint64_t physical,size_t bytes,NativeAdrenoCacheOperation);
/* The exact byte interval is maintained across ordered physical spans. Each
 * intersecting cache line is included, with at most 64 pages per preemption hold. */
IOReturn NativeAdrenoMaintainPhysicalSpans(const NativeAdrenoPhysicalSpan *,size_t count,size_t offset,size_t bytes,
    NativeAdrenoCacheOperation);
/* Every CPU mapping of the spans' pages becomes write-combined (true) or takes the default cacheable attribute again (false). */
IOReturn NativeAdrenoSetPagesCacheMode(const NativeAdrenoPhysicalSpan *,size_t count,bool write_combined);
/* int-100 found a write-combined BO's page back at the default (cacheable) attribute with dirty IB lines: XNU's view machinery sets a page's
 * attribute when it populates or tears down a device-pager view. Every page of the spans no longer write-combined is made so again (the pmap
 * then rewrites every mapping it tracks and writes the page's cache lines back); *reset counts them. */
IOReturn NativeAdrenoReassertWriteCombined(const NativeAdrenoPhysicalSpan *,size_t count,uint32_t *reset);

enum : unsigned {
    NativeAdrenoMemorySizeBucketCount=18,
    NativeAdrenoMemorySlowEventCount=64,
    NativeAdrenoBackingMissReasonCount=4,
    NativeAdrenoBackingEvictReasonCount=5,
    NativeAdrenoBackingRejectReasonCount=13
};

enum class NativeAdrenoBackingTrimReason : uint32_t {
    ClientClose=2,
    MemoryPressure=3,
    AllocationRetry=4
};

enum class NativeAdrenoMemorySource : uint32_t {
    Unknown,
    KernelOwned,
    UserOwned,
    Imported
};

enum class NativeAdrenoMemoryCacheResult : uint32_t {
    NotRequested,
    Hit,
    Miss
};

enum class NativeAdrenoMemorySlowOperation : uint32_t {
    Allocation,
    OwnedDescriptorRelease,
    ImportedDescriptorRelease,
    Import
};

enum NativeAdrenoMemoryDiagnosticFlags : uint32_t {
    NativeAdrenoMemoryDiagnosticContiguous=1u<<0,
    NativeAdrenoMemoryDiagnosticWriteCombined=1u<<1,
    NativeAdrenoMemoryDiagnosticImported=1u<<2,
    NativeAdrenoMemoryDiagnosticPooled=1u<<3,
    NativeAdrenoMemoryDiagnosticQuarantined=1u<<4,
    NativeAdrenoMemoryDiagnosticReuseRequested=1u<<5
};

/* NativeAdrenoMemoryBucketStats counts accepted allocation requests whose CPU
 * page configuration was available. A lookup has one hit or one miss reason;
 * bypassed requests did not ask to reuse write-combined backing. */
struct NativeAdrenoMemoryBucketStats {
    uint64_t upper_bound_bytes=0; // Zero denotes sizes above the preceding bucket.
    uint64_t requests=0,bytes=0,lookups=0,hits=0,misses=0,bypassed=0;
    uint64_t miss_reason[NativeAdrenoBackingMissReasonCount]{}; // Empty, size, attributes, quarantined.
};

/* NativeAdrenoMemorySlowEvent describes one completed operation of at least
 * 8000 us. Storage metadata identifies its owner and allocation attributes;
 * the cache and available-page values are sampled when the operation ends. */
struct NativeAdrenoMemorySlowEvent {
    uint64_t sequence=0,sampled_us=0,elapsed_us=0,bytes=0,spans=0;
    uint64_t cached_bytes=0,available_pages=0;
    uint32_t cached_objects=0,output_bits=0,flags=0;
    NativeAdrenoMemorySlowOperation operation=NativeAdrenoMemorySlowOperation::Allocation;
    NativeAdrenoMemorySource source=NativeAdrenoMemorySource::Unknown;
    NativeAdrenoMemoryCacheResult cache_result=NativeAdrenoMemoryCacheResult::NotRequested;
    char process[20]{};
};

/* NativeAdrenoMemoryDiagnostics is a fixed-size copy of backing-cache counters
 * and slow operations. The resource gate serializes producers and the copy;
 * the published service owns the copied value while printing outside its lock. */
struct NativeAdrenoMemoryDiagnostics {
    NativeAdrenoMemoryBucketStats buckets[NativeAdrenoMemorySizeBucketCount]{};
    uint64_t insert_attempts=0,insertions=0;
    // Not owned, not prepared, not WC, quarantine, no spans, zero size,
    // oversized, no evictable entry, no slot, imported, pooled, pressure, busy.
    uint64_t insert_reject[NativeAdrenoBackingRejectReasonCount]{};
    // Object limit, byte limit, client close, memory pressure, allocation retry.
    uint64_t eviction_attempts[NativeAdrenoBackingEvictReasonCount]{};
    uint64_t eviction_failures[NativeAdrenoBackingEvictReasonCount]{};
    uint64_t evictions[NativeAdrenoBackingEvictReasonCount]{};
    uint64_t evicted_bytes[NativeAdrenoBackingEvictReasonCount]{};
    uint64_t cached_objects=0,cached_bytes=0,peak_objects=0,peak_bytes=0;
    uint64_t slow_total=0;
    uint32_t slow_count=0;
    NativeAdrenoMemorySlowEvent slow[NativeAdrenoMemorySlowEventCount]{}; // Oldest first in a copied result.
};

/* NativeAdrenoBackingStorage owns a prepared allocation and the physical spans
 * discovered from its descriptor. The reusable-storage cache keeps these fields
 * after a user BO is unmapped, then transfers them to the next matching BO.
 * Imported pages use the same span metadata while NativeAdrenoMemory owns the
 * importing descriptor and its task lifetime. */
struct NativeAdrenoBackingStorage {
    IOBufferMemoryDescriptor *descriptor=nullptr;
    void *mapping=nullptr;
    NativeAdrenoPhysicalSpan *spans=nullptr;
    size_t bytes=0,span_count=0,span_capacity=0;
    unsigned output_bits=0;
    bool contiguous=false,prepared=false,quarantined=false,write_combined=false;
    bool descriptor_write_combined=false;   /* The persistent memory entry was created with WC. */
    NativeAdrenoMemorySource diagnostic_source=NativeAdrenoMemorySource::Unknown;
    NativeAdrenoMemoryCacheResult diagnostic_cache_result=NativeAdrenoMemoryCacheResult::NotRequested;
};

/* NativeAdrenoMemory owns one BO's backing storage and counts references held
 * by GPU mappings and jobs. Calls run under the queue/resource gate. The VM
 * retires each reference after DMA completion and native TLB unmap, and the
 * user client removes every CPU view before offering storage for reuse.
 * release() keeps failed teardown state owned so the caller can retain it. */
class NativeAdrenoMemory final {
    NativeAdrenoBackingStorage storage_;
    IOMemoryDescriptor *imported_=nullptr;
    bool read_only_=false;
    bool controlled_cpu_views_=true;   /* A legacy physical view requires submit-time page repair. */
    uint32_t device_references_=0;
    bool pooled_=false;
    size_t pool_offset_=0;
    IOReturn maintainImported(size_t offset,size_t bytes,NativeAdrenoCacheOperation);
public:
    NativeAdrenoMemory()=default;
    ~NativeAdrenoMemory()=default;
    NativeAdrenoMemory(const NativeAdrenoMemory &)=delete;
    NativeAdrenoMemory &operator=(const NativeAdrenoMemory &)=delete;
    /* Explicit driver budget; bounded to 256MiB per backing object for now.
     * Error may retain quarantined allocation state if complete() fails.
     * Caller always keeps this object and checks quarantined(), even on error.
     * CPU native4K state and kernel page ABI are checked before allocation.
     * prepare(), raw unmapped physical segments, span/PA bounds and optional
     * physical contiguity are verified before zeroing/PoC publication. */
    /* `pooled`: take the bytes from the BO pool when one is reserved and has room. Only user BOs do: the pool is write-combined for
     * the CPU (writeCombinePool), and the kernel's own objects (SMMU tables, ring, fence and firmware pages) are walked or read by the
     * CPU through their cacheable mapping (int-153: page tables from the write-combined pool made each map's table update 2.6x slower). */
    IOReturn allocate(size_t bytes,unsigned output_bits,bool contiguous,size_t byte_budget,bool pooled=false,
        bool reuse_write_combined=false);
    /* The cache owns at most 64 allocations and 64 MiB. Matching allocations
     * retain their prepared descriptors, spans and write-combined attributes;
     * allocate() reasserts the attributes and clears every byte before reuse.
     * Allocation failure and low available memory drain it. Client close also
     * drains it; failed teardown stays owned and quarantined in the cache. */
    static IOReturn trimBackingCache(NativeAdrenoBackingTrimReason reason=NativeAdrenoBackingTrimReason::ClientClose);
    /* The caller holds the same queue/resource gate as allocate and release.
     * The result contains the retained slow events in increasing sequence order. */
    static void copyDiagnostics(NativeAdrenoMemoryDiagnostics *out);
    /* One physically contiguous block taken from the VM early (while memory is not yet fragmented) and cut into BOs afterwards: a BO from it is
     * one span of the GPU's table instead of one per scattered page (int-137: 59474 spans for 332 MiB, the 65536 span capacity ran out,
     * BoMap failed, the translator answered out of device memory and the compositor fell back to the CPU; each map also walked every span, 167 ms).
     * allocate(..., pooled=true) uses the pool when one is reserved and has room, else the scattered path. Only the board calls this. */
    static IOReturn reservePool(unsigned output_bits,size_t bytes);
    static size_t poolBytes();
    /* The whole pool write-combined once, for good: a pooled BO then takes no page-attribute change on allocation or release.
     * Board only, after reservePool, when user BOs are write-combined. */
    static IOReturn writeCombinePool();
    /* importTask pins the task's pages and retains their existing CPU cache mode.
     * publishForDevice and acquireDeviceWrites maintain their physical ranges
     * around GPU use. Spans and PA bounds are checked as for allocate. */
    IOReturn importTask(void *task,uint64_t address,size_t bytes,unsigned output_bits,size_t byte_budget);
    bool imported() const {return imported_!=nullptr;}
    /* An import of memory the owner maps read-only (the kernel could pin it for reading only): the GPU may read it, never write it. */
    bool readOnly() const {return read_only_;}
    /* After allocate(), before any device or client mapping: cleans and invalidates the pages and makes every CPU mapping of them
     * write-combined. Cached storage already has this attribute. */
    IOReturn makeWriteCombined();
    bool writeCombined() const {return storage_.write_combined;}
    bool descriptorWriteCombined() const {return storage_.descriptor_write_combined;}
    void allowLegacyCpuView() {controlled_cpu_views_=false;}
    /* Legacy physical views can change scattered pages' attributes. Controlled
     * views share the allocation's persistent WC memory entry, so their map and
     * unmap operations preserve the cache mode without walking physical pages. */
    IOReturn reassertWriteCombined(uint32_t *reset) const
    {
        return storage_.write_combined && storage_.span_count && !pooled_ &&
            (!storage_.descriptor_write_combined || !controlled_cpu_views_)?
            NativeAdrenoReassertWriteCombined(storage_.spans,storage_.span_count,reset):kIOReturnSuccess;
    }
    bool backed() const {return storage_.descriptor || imported_ || pooled_;}
    /* cache_backing requires the caller to have removed every CPU mapping.
     * A prepared, write-combined owned allocation can then enter the cache if
     * device references are zero. Imports and quarantined storage cannot enter. */
    IOReturn release(bool cache_backing=false);
    IOReturn holdForDevice();
    IOReturn retireAfterQuiescence();
    void quarantine() {storage_.quarantined=true;}
    /* Requires real CPU/device exclusion and retained ownership. Before GPU
     * writes, clean+invalidate; after genuine completion, invalidate only.
     * CPU must not dirty intersecting cache lines while the device owns access. */
    /* A write-combined object is skipped unless include_write_combined: then its pages are maintained through the physical aperture,
     * for a client whose view turned out to be cacheable after all (NativeAdrenoVm::distrustWriteCombined). */
    IOReturn publishForDevice(size_t offset,size_t bytes,bool device_writes,bool include_write_combined=false);
    IOReturn acquireDeviceWrites(size_t offset,size_t bytes,bool submitted_range=false);
    bool quarantined() const {return storage_.quarantined;}
    uint32_t deviceReferences() const {return device_references_;}
    size_t bytes() const {return storage_.bytes;}
    size_t spanCount() const {return storage_.span_count;}
    const NativeAdrenoPhysicalSpan *spans() const {return storage_.spans;}
    void *mapping() const {return storage_.mapping;}
    /* Borrowed descriptor, never release independently or expose table pools
     * through clientMemoryForType. Sharing requires a separate owner/task ABI. */
    IOBufferMemoryDescriptor *descriptor() const {return storage_.descriptor;}
};
#endif
