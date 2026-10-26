/* SPDX-License-Identifier: MIT */
#ifndef NATIVE_ADRENO_VM_H
#define NATIVE_ADRENO_VM_H
#include "NativeAdrenoSmmuMemory.h"
#include "ArmSmmu4KStreams.h"
#include <libkern/tree.h>
struct NativeAdrenoVmRegion {
    NativeAdrenoMemory *tables;
    ArmSmmu4KLiveArena *arena;
    ArmSmmu4KLiveState *state;
};
struct NativeAdrenoVmResult {
    ArmSmmu4KStatus operation,quarantine;
    size_t completed_spans;
    bool complete;
};
/* NativeAdrenoVm owns the CPU records for BOs mapped into the GPU context.
 * Each record retains its backing memory, physical ranges and a physical-address
 * index. A red-black tree locates neighboring GPU address ranges;
 * generation handles locate records directly. Mapping validates the BO against
 * its neighbors, conflicting physical attributes and both page-table pools,
 * then asks ArmSmmu4K to change only that BO's leaves. Unmapping completes the
 * context's TLB synchronization before releasing the memory reference.
 *
 * The service serializes these operations and owns the powered context, streams
 * and page-table allocations. NativeAdrenoSubmit retains job references until
 * the GPU fence completes. A failed page-table transaction keeps the records
 * and memory, faults the owned streams and requires service recovery. */
enum : size_t {NativeAdrenoVmMaxObjects=4096};
/* Board hook for sampleWord(): reads one aligned word of physical memory through a fresh uncached mapping; 0 on success. */
extern "C" int (*native_adreno_dram_read32)(uint64_t physical,uint32_t *value);
/* The board's reader through the kernel's cacheable physical aperture (null on the host: the BO's own mapping is read). */
extern "C" int (*native_adreno_cached_read32)(uint64_t physical,uint32_t *value);
/* Board hook for auditRange(): [physical, physical+bytes), whole 64-byte lines inside one page, as the cacheable physical aperture and
 * DRAM (a fresh uncached mapping) read it. A line that differs is cleaned and invalidated and DRAM read again: when DRAM then holds the
 * cached copy the line was dirty (*dirty), when DRAM is unchanged it was a stale clean copy (*stale). 0 on success. */
extern "C" int (*native_adreno_line_audit)(uint64_t physical,size_t bytes,uint32_t *dirty,uint32_t *stale);
class NativeAdrenoVm final {
    struct Mapping {
        NativeAdrenoMemory *memory=nullptr;
        ArmSmmu4KRange *ranges=nullptr;
        uint32_t *physical_order=nullptr;
        size_t physical_count=0;
        uint64_t handle=0,iova=0;
        size_t spans=0,mapped_spans=0;
        unsigned permissions=0,writeback=0,shareability=0,jobs=0;
        size_t next_free=0;
        RB_ENTRY(Mapping) address_link;
        Mapping *previous=nullptr,*next=nullptr;
    };
    struct AddressTree {
        RB_HEAD(Root,Mapping) root{};
        static int compare(Mapping *left,Mapping *right) {
            if(left->iova<right->iova) {
                return -1;
            }
            return left->iova>right->iova?1:0;
        }
        RB_GENERATE(Root,Mapping,address_link,compare)
    } addresses_;
    Mapping *mappings_=nullptr;
    Mapping *first_=nullptr,*last_=nullptr;
    NativeAdrenoVmRegion regions_[2]{};
    ArmSmmu4KLiveVisibility visibility_[2]{};
    ArmSmmu4KIo io_{};
    ArmSmmu4KContextConfig *context_=nullptr;
    ArmSmmu4KStreamState *streams_=nullptr;
    size_t lower_count_=0,upper_count_=0,objects_=0,free_head_=0,mapping_capacity_=0;
    size_t class_objects_[16]{};
    unsigned budget_=0;
    uint64_t next_handle_=1;
    bool attached_=false,poisoned_=false,distrust_write_combined_=false;
    NativeAdrenoVmResult result_{};
    ArmSmmu4KLiveRegion region(unsigned) const;
    ArmSmmu4KStatus observe() const;
    IOReturn fail(ArmSmmu4KStatus);
    Mapping *find(uint64_t) const;
    const Mapping *findAddressRecord(uint64_t) const;
    Mapping *lowerBound(uint64_t) const;
    void insert(Mapping *,unsigned);
    void erase(Mapping *,unsigned);
    IOReturn preflight(const NativeAdrenoMemory *,unsigned,uint64_t,unsigned,unsigned,unsigned) const;
public:
    NativeAdrenoVm()=default;
    ~NativeAdrenoVm()=default;
    NativeAdrenoVm(const NativeAdrenoVm &)=delete;
    NativeAdrenoVm &operator=(const NativeAdrenoVm &)=delete;
    IOReturn attach(const ArmSmmu4KIo *,ArmSmmu4KContextConfig *,ArmSmmu4KStreamState *,
        const NativeAdrenoVmRegion *lower,const NativeAdrenoVmRegion *upper,
        size_t max_objects,unsigned poll_budget);
    /* Frees CPU metadata only, with no BOs remaining. Never releases roots,
     * disables a bank or returns the platform lease; board lifecycle owns it. */
    IOReturn detach();
    IOReturn findAddress(unsigned upper,size_t bytes,uint64_t alignment,uint64_t *iova) const;
    IOReturn map(NativeAdrenoMemory *,unsigned upper,uint64_t iova,
        unsigned permissions,unsigned writeback,unsigned shareability,uint64_t *handle);
    IOReturn unmap(uint64_t handle);
    IOReturn holdJob(uint64_t handle);
    /* TLBIALL for the context bank plus TLBSYNC: drops every cached translation of the bank whatever its ASID. Unmap already
     * synchronizes by ASID; this is the stronger form the submit path uses before each submission (int-070: on sheng the CP read
     * zeros from an IB whose table entry and DRAM were both correct, after the address had been mapped to other pages before). */
    IOReturn invalidateTlb();
    /* Cleans and invalidates the CPU caches over every mapped BO (whole objects; imported ones through their physical pages), so
     * no CPU cache line can hold data the GPU should see or hide what it wrote. Lines are physically tagged, so this also reaches
     * lines a user mapping of the same pages left behind. int-070 runs it before every submission. */
    IOReturn publishMapped();
    /* The submit path saw a CPU-cached word that DRAM did not hold yet in a client's BO: from now on write-combined BOs are maintained
     * like any other (sticky; the safe behaviour of int-070..097). */
    void distrustWriteCombined() {distrust_write_combined_=true;}
    bool writeCombinedTrusted() const {return !distrust_write_combined_;}
    /* After a submission completed: cleans and invalidates the CPU caches over every GPU-writable imported BO, so the owner's mapping
     * (an IOSurface) reads what the GPU wrote, not a line prefetched while the GPU ran. Jobs name only their IBs, so every such BO
     * is covered. int-071: an imported render target read back 672 of 4096 texels stale. */
    IOReturn acquireImported();
    /* Diagnostic, never a proof of coherence: the 32-bit word at a mapped GPU address as the kernel's cacheable alias reads it
     * and, when the board installed native_adreno_dram_read32, as DRAM reads it through an uncached mapping (else ~0). */
    IOReturn sampleWord(uint64_t iova,uint32_t *cached,uint32_t *dram) const;
    /* Every line of [iova, iova+bytes) that lies in a write-combined BO, through native_adreno_line_audit: *dirty counts lines a CPU
     * cache held newer than DRAM (a client view that caches after all; each such line is written back by the audit), *stale clean
     * copies, *lines the lines compared. kIOReturnUnsupported without a board hook. int-099 sampled one word and wrote its line back
     * before reading DRAM, so it could never see a dirty line. */
    /* Before a submission while write-combined BOs are trusted: every page of every mapped write-combined BO that XNU set back to the
     * default attribute is write-combined again (and its lines written back); *reset counts the pages. */
    IOReturn reassertWriteCombined(uint32_t *reset) const;
    IOReturn auditRange(uint64_t iova,size_t bytes,uint32_t *lines,uint32_t *dirty,uint32_t *stale) const;
    /* Read-only description of a live mapping; never pins or extends it. */
    IOReturn describe(uint64_t handle,uint64_t *iova,size_t *bytes,unsigned *permissions) const;
    /* Up to `count` dwords of a mapped kernel-owned BO at a GPU address (through the kernel's own mapping), for the board log. */
    IOReturn peekWords(uint64_t iova,uint32_t *out,size_t count) const;
    /* Only a scheduler's actual successful GPU completion may call this.
     * A timeout, interrupt bit alone or CPU-written timestamp is insufficient. */
    IOReturn retireJobAfterCompletion(uint64_t handle);
    bool poisoned() const {return poisoned_;}
    size_t objectCount() const {return objects_;}
    size_t spanCount() const {return lower_count_+upper_count_;}
    NativeAdrenoVmResult lastResult() const {return result_;}
};
#endif
