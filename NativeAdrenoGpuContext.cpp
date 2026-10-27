/* SPDX-License-Identifier: MIT */
#include "NativeAdrenoGpuContext.h"
#include "ArmSmmu4KMmio.h"
#include <IOKit/IOLib.h>

extern "C" volatile uint32_t native_adreno_pool_mib;
extern "C" volatile uint32_t native_adreno_pool_stats[8],native_adreno_pool_status;
static const uint64_t kIovas[GpuBufferCount]={GpuSqeIova,GpuRingIova,GpuMemptrsIova,GpuPwrupIova,GpuScratchIova};
static const size_t kBytes[GpuBufferCount]={GpuSqeBytes,GpuRingBytes,GpuPageBytes,GpuPageBytes,GpuPageBytes};
IOReturn NativeAdrenoGpuContext::bringUp(const ArmSmmu4KIo *io,unsigned bank,unsigned asid,const unsigned slots[2],
    const uint16_t sids[2],void (*progress)(uint32_t),NativeAdrenoGmuContextReport *report)
{
    return initialize(io,bank,asid,slots,sids,progress,report,nullptr);
}

IOReturn NativeAdrenoGpuContext::bringUpShared(const ArmSmmu4KIo *io,unsigned bank,unsigned asid,
    const unsigned slots[2],const uint16_t sids[2],NativeAdrenoGpuContext *source,
    NativeAdrenoGmuContextReport *report)
{
    if(!source || source==this || !source->ready()) {
        return kIOReturnBadArgument;
    }
    return initialize(io,bank,asid,slots,sids,nullptr,report,source);
}

IOReturn NativeAdrenoGpuContext::initialize(const ArmSmmu4KIo *io,unsigned bank,unsigned asid,
    const unsigned slots[2],const uint16_t sids[2],void (*progress)(uint32_t),
    NativeAdrenoGmuContextReport *report,NativeAdrenoGpuContext *shared)
{
    if(!report)return kIOReturnBadArgument;
    *report={};
    if(used_)return kIOReturnBusy;
    if(!io || !slots || !sids || bank>63 || !asid || asid>0xffff || slots[0]>127 || slots[1]>127 ||
        slots[0]==slots[1])return kIOReturnBadArgument;
    used_=true;
    shared_=shared;
    report->bank=bank;
    report->asid=asid;
    report->stream_count=2;
    report->streams[0]={slots[0],sids[0]};
    report->streams[1]={slots[1],sids[1]};
    IOReturn status=kIOReturnSuccess;
    auto step=[&](Step s) {report->step=s;if(progress)progress(s);};
    step(Allocate);
    static const uint8_t oas_bits[16]={32,36,40,42,44,48};
    if(io->read32(io->cookie,0x28,&report->id2))status=kIOReturnIOError;
    else {
        const unsigned code=(report->id2>>4)&15;
        report->output_bits=code<6?oas_bits[code]:0;
        if(report->output_bits>40)report->output_bits=40;
        if(!report->output_bits)status=kIOReturnUnsupported;
    }
    /* The BO pool is taken first, while the VM still has long contiguous runs (the board sets the size from sheng-gpu-pool; 0 = scattered BOs only).
     * A pool that cannot be had at one size is tried at the next smaller; none is not an error. */
    if(!status && !shared_ && native_adreno_pool_mib) {
        for(uint32_t mib=native_adreno_pool_mib;mib>=16;mib>>=1) {
            const IOReturn pooled=NativeAdrenoMemory::reservePool(report->output_bits,(size_t)mib<<20);
            native_adreno_pool_stats[0]=pooled==kIOReturnSuccess?mib:0;native_adreno_pool_status=(uint32_t)pooled;
            if(pooled==kIOReturnSuccess)break;
        }
    }
    if(!status) {
        report->allocation_bytes=TablePages*0x1000;
        status=tables_.allocate(TablePages*0x1000,report->output_bits,true,TablePages*0x1000);
        report->allocation_status=status;
    }
    for(unsigned i=0;!status && !shared_ && i<GpuBufferCount;++i) {
        report->allocation_index=i+1;
        report->allocation_bytes=kBytes[i];
        IOLog("NativeAdrenoGpuMemory: allocate index=%u iova=%llx bytes=%lx\n",i,kIovas[i],kBytes[i]);
        status=buffers_[i].allocate(kBytes[i],report->output_bits,true,kBytes[i]);
        report->allocation_status=status;
    }
    if(!status) {
        step(Prepare);
        status=smmu_.prepare(&tables_,0,0,GpuApertureBytes,NativeAdrenoSmmuContext::MaxLeaves,
            bank,asid,1000,report->output_bits);
        if(!status)smmu_.setAliasScratch(alias_scratch_,sizeof alias_scratch_/sizeof *alias_scratch_);
    }
    if(!status) {
        step(Ownership);
        report->ownership_status=ArmSmmu4KCheckFaultedStreams(io,report->streams,
            report->stream_count,bank,smmu_.configuration()->stream_shareability,&report->ownership);
        if(report->ownership_status!=ArmSmmu4KSuccess) {
            status=kIOReturnIOError;
        }
    }
    if(!status) {
        step(Program);
        status=smmu_.program(io);
        const ArmSmmu4KProgramResult *p=smmu_.programResult();
        report->program_phase=p->phase;report->program_writes=p->writes;
        report->program_readback_valid=p->readback_valid;report->program_enabled=p->enabled;
        report->id1=p->id1;report->program_status=smmu_.programStatus();
        report->image=p->readback;
    }
    if(!status) {
        step(Attach);
        const ArmSmmu4KStream routes[2]={{slots[0],sids[0]},{slots[1],sids[1]}};
        status=smmu_.attach(routes,2);
        const ArmSmmu4KStreamResult *s=smmu_.streamResult();
        report->route_status=smmu_.attachStatus();
        report->route_phase=s->phase;report->route_writes=s->writes;report->routes_verified=s->streams_verified;
    }
    if(!status) {
        step(VmAttach);
        NativeAdrenoVmRegion lower={&tables_,smmu_.arenaHandle(),smmu_.stateHandle()};
        status=vm_.attach(io,smmu_.configHandle(),smmu_.streamsHandle(),&lower,nullptr,NativeAdrenoVmMaxObjects,262144);
        if(!status) {
            status=vm_.enableCachedCoherentImports(0x06030500);
        }
    }
    if(!status) {
        step(Map);
        const unsigned permissions[GpuBufferCount]={
            ArmSmmu4KRead|ArmSmmu4KExecute,
            ArmSmmu4KRead|ArmSmmu4KPrivileged,
            ArmSmmu4KRead|ArmSmmu4KWrite|ArmSmmu4KPrivileged,
            ArmSmmu4KRead|ArmSmmu4KWrite|ArmSmmu4KPrivileged,
            ArmSmmu4KRead|ArmSmmu4KWrite
        };

        /* APRIV permits the CP to access its ring, memptrs and power-up list.
         * Indirect user commands retain access only to the scratch buffer. */
        for(unsigned i=0;!status && i<GpuBufferCount;++i) {
            status=vm_.map(&buffer(i),0,kIovas[i],permissions[i],0,2,&handles_[i]);
        }

        report->spans=uint32_t(vm_.spanCount());
    }
    if(!status) {
        step(Ready);
        ready_=true;
    }
    report->status=status;
    NativeAdrenoCaptureSmmuReport(io,&smmu_,report);
    return status;
}
static NativeAdrenoGpuContext *board_gpu_context;
extern "C" IOReturn NativeAdrenoGpuContextBringup(volatile uint8_t *smmu,size_t bytes,unsigned bank,unsigned asid,
    unsigned slot0,unsigned sid0,unsigned slot1,unsigned sid1,void (*progress)(uint32_t),
    NativeAdrenoGmuContextReport *report)
{
    static ArmSmmu4KMmio mmio;
    static ArmSmmu4KIo io;
    if(!report || board_gpu_context || !smmu || !bytes || sid0>0xffff || sid1>0xffff)return kIOReturnBadArgument;
    mmio={reinterpret_cast<volatile unsigned char *>(smmu),bytes};
    if(ArmSmmu4KBindMmio(&mmio,&io)!=ArmSmmu4KSuccess)return kIOReturnBadArgument;
    board_gpu_context=new NativeAdrenoGpuContext;
    if(!board_gpu_context)return kIOReturnNoMemory;
    const unsigned slots[2]={slot0,slot1};
    const uint16_t sids[2]={uint16_t(sid0),uint16_t(sid1)};
    return board_gpu_context->bringUp(&io,bank,asid,slots,sids,progress,report);
}
extern "C" IOReturn NativeAdrenoGpuContextBuffers(NativeAdrenoGpuBuffer out[5])
{
    if(!out)return kIOReturnBadArgument;
    for(unsigned i=0;i<GpuBufferCount;++i)out[i]={};
    if(!board_gpu_context || !board_gpu_context->ready())return kIOReturnNotReady;
    for(unsigned i=0;i<GpuBufferCount;++i) {
        const NativeAdrenoMemory &m=board_gpu_context->buffer(i);
        if(m.spanCount()!=1 || m.spans()[0].bytes!=m.bytes())return kIOReturnUnsupported;
        out[i]={m.spans()[0].physical,m.bytes(),kIovas[i]};
    }
    return kIOReturnSuccess;
}
extern "C" NativeAdrenoVm *NativeAdrenoGpuContextVm()
{
    return board_gpu_context?board_gpu_context->vm():nullptr;
}
