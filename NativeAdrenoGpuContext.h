/* SPDX-License-Identifier: MIT */
#ifndef NATIVE_ADRENO_GPU_CONTEXT_H
#define NATIVE_ADRENO_GPU_CONTEXT_H
#include "NativeAdrenoGmuContext.h"

enum : uint64_t {
    GpuSqeIova=0x100000, GpuRingIova=0x200000, GpuMemptrsIova=0x300000, GpuPwrupIova=0x301000,
    GpuScratchIova=0x302000, GpuApertureBytes=0x80000000
};
enum : size_t {GpuSqeBytes=0x13000, GpuRingBytes=0x8000, GpuPageBytes=0x1000, GpuBufferCount=5};
struct NativeAdrenoGpuBuffer {uint64_t physical,bytes,iova;};
/* NativeAdrenoGpuContext owns the GPU's 4 KiB page tables, one context bank and
 * two stream routes. bringUp receives the live SMMU callbacks and allocated
 * bank, ASID, SMR slots and SIDs, then allocates buffers, programs the context
 * and maps the fixed GPU addresses in a 0..2 GiB aperture. bringUpShared
 * creates independent page tables and routes over a ready context's buffers;
 * the source context owns those buffers for the entire boot.
 *
 * The SQE buffer contains a660_sqe.fw without its first dword and is read-only
 * executable. The 32 KiB ring is privileged read-only; memptrs and the power-up
 * register list are privileged read/write. The separate scratch page is normal
 * read/write memory for the first CP write. Memptrs stores the RPTR at +0 and
 * the completion fence at +4. Data buffers use matching Normal non-cacheable,
 * outer-shareable CPU and SMMU mappings. The service owns power, firmware loading
 * and command submission; these allocations remain retained after failure. */
class NativeAdrenoGpuContext final {
    NativeAdrenoMemory tables_,buffers_[GpuBufferCount];
    uint64_t handles_[GpuBufferCount]{};
    NativeAdrenoSmmuContext smmu_;
    NativeAdrenoVm vm_;
    NativeAdrenoGpuContext *shared_=nullptr;
    /* The alias checks' sort scratch: one entry per range of the smaller compared side, at most half the aperture's leaves. */
    uint32_t alias_scratch_[NativeAdrenoSmmuContext::MaxLeaves/2];
    bool ready_=false,used_=false;
    IOReturn initialize(const ArmSmmu4KIo *,unsigned,unsigned,const unsigned[2],
        const uint16_t[2],void (*)(uint32_t),NativeAdrenoGmuContextReport *,NativeAdrenoGpuContext *);
public:
    enum Step : uint32_t {Allocate=1,Prepare,Program,Attach,VmAttach,Map,Ready,Ownership};
    enum : size_t {TablePages=1027};
    NativeAdrenoGpuContext()=default;
    NativeAdrenoGpuContext(const NativeAdrenoGpuContext &)=delete;
    NativeAdrenoGpuContext &operator=(const NativeAdrenoGpuContext &)=delete;
    /* The report is the GMU context's: route counts cover both streams. */
    IOReturn bringUp(const ArmSmmu4KIo *io,unsigned bank,unsigned asid,const unsigned slots[2],
        const uint16_t sids[2],void (*progress)(uint32_t step),NativeAdrenoGmuContextReport *report);
    IOReturn bringUpShared(const ArmSmmu4KIo *io,unsigned bank,unsigned asid,const unsigned slots[2],
        const uint16_t sids[2],NativeAdrenoGpuContext *source,NativeAdrenoGmuContextReport *report);
    bool ready() const {return ready_;}
    const NativeAdrenoMemory &buffer(unsigned index) const {return shared_?shared_->buffer(index):buffers_[index];}
    NativeAdrenoMemory &buffer(unsigned index) {return shared_?shared_->buffer(index):buffers_[index];}
    NativeAdrenoVm *vm() {return ready_?&vm_:nullptr;}
};
extern "C" IOReturn NativeAdrenoGpuContextBringup(volatile uint8_t *smmu,size_t bytes,unsigned bank,unsigned asid,
    unsigned slot0,unsigned sid0,unsigned slot1,unsigned sid1,void (*progress)(uint32_t),
    NativeAdrenoGmuContextReport *report);
/* sqe, ring, memptrs, pwrup, scratch: physical base, size and GPU address, only after Ready. */
extern "C" IOReturn NativeAdrenoGpuContextBuffers(NativeAdrenoGpuBuffer out[5]);
/* The live VM of the ready GPU context (null before Ready), for the user-client core. */
extern "C" NativeAdrenoVm *NativeAdrenoGpuContextVm();
#endif
