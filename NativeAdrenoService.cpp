/* SPDX-License-Identifier: MIT */
#include "NativeAdrenoA6xxService.h"
#include "NativeAdrenoA6xxGmu.h"
#include "NativeAdrenoA6xxHwInit.h"
#include "NativeAdrenoA6xxZap.h"
#include "NativeAdrenoGpuContext.h"
#include "NativeAdrenoCpInit.h"
#include "NativeAdrenoIrq.h"
#include "NativeAdrenoKernelTime.h"
#include "NativeAdrenoUserClient.h"
#include "ArmSmmu4KMmio.h"
#include "AdrenoPM4.h"
#include <IOKit/IODeviceMemory.h>
#include <IOKit/IOLib.h>
#include <IOKit/IOMemoryDescriptor.h>
#include <IOKit/IOService.h>
#include <kern/thread.h>
#include <pexpert/pexpert.h>
#include <string.h>

extern "C" const uint8_t native_adreno_firmware_image[];
extern "C" const uint64_t native_adreno_firmware_image_size;
extern "C" const uint8_t native_adreno_sqe_firmware_image[];
extern "C" const uint64_t native_adreno_sqe_firmware_image_size;
extern "C" uint64_t (*native_adreno_vm_now_us)(void);
extern "C" volatile uint32_t native_adreno_pool_mib;
extern "C" IOReturn native_adreno_publish(NativeAdrenoVm *,NativeAdrenoSubmit *,NativeAdrenoRing *,
    const NativeAdrenoRingClock *,const NativeAdrenoTimer *,const NativeAdrenoAbiDeviceInfo *,uint32_t,uint32_t);
extern "C" void native_adreno_fault(void);
extern "C" int native_adreno_progress(uint32_t *,uint32_t *);
extern "C" IOReturn native_adreno_initialize_classes(void);
extern "C" {
volatile uint64_t native_adreno_fence_phys=0;
volatile uint32_t native_adreno_a6xx_stats[24];
}

namespace {
/* A642LState owns all mappings, contexts and DMA allocations for this boot.
 * A failed attempt retains them while the GMU and CP are stopped. The CPU
 * framebuffer continues independently of whether this object publishes a GPU. */
class A642LState final {
    OSSharedPtr<IOMemoryMap> maps_[NativeAdrenoA6xxDomains];
    OSSharedPtr<IOMemoryMap> zapMap_;
    OSSharedPtr<IOMemoryMap> smmuMap_;
    NativeAdrenoA6xxResources resources_{};
    NativeAdrenoA6xxWindow windows_[NativeAdrenoA6xxDomains]{};
    NativeAdrenoA6xxPower power_;
    NativeAdrenoA6xxGmu gmu_;
    NativeAdrenoA6xxZap zap_;
    NativeAdrenoGmuContext gmuContext_;
    NativeAdrenoGpuContext gpuContext_;
    NativeAdrenoRing ring_;
    NativeAdrenoIrq irq_;
    NativeAdrenoSubmit submit_;
    NativeAdrenoTimer timer_;
    NativeAdrenoAbiDeviceInfo info_{};
    ArmSmmu4KMmio smmuMmio_{};
    ArmSmmu4KIo smmuIo_{};
    NativeAdrenoRingIo ringIo_{};
    NativeAdrenoRingClock clock_{};
    bool cpTouched_=false;
    bool ready_=false;
    uint32_t watchedFence_=0;
    uint64_t watchedSince_=0;

    static int read(void *,uint32_t,uint32_t,uint32_t *);
    static int write(void *,uint32_t,uint32_t,uint32_t);
    static void delay(void *,uint32_t);
    static int gpuRead(void *,uint32_t,uint32_t *);
    static int gpuWrite(void *,uint32_t,uint32_t);
    static int barrier(void *);
    static int publishRing(void *,const volatile uint32_t *,size_t);
    static int acquireFence(void *,volatile uint32_t *,size_t);
    static uint64_t now(void *);
    static int idle(void *);
    IOReturn mapWindows();
    IOReturn contexts();
    IOReturn startCp();
    IOReturn fail(IOReturn);
public:
    IOReturn start();
    void poll();
    void dump() const;
};

A642LState *state=nullptr;
uint32_t launched=0;

OSSharedPtr<IOMemoryMap> mapWindow(const NativeAdrenoA6xxWindow &window)
{
    if(!window.physical || !window.bytes || window.bytes>SIZE_MAX ||
        window.physical>UINT64_MAX-window.bytes) {
        return {};
    }
    IODeviceMemory *memory=IODeviceMemory::withRange(window.physical,window.bytes);
    if(!memory) {
        return {};
    }
    OSSharedPtr<IOMemoryMap> map(memory->map(kIOMapAnywhere|kIOMapInhibitCache),OSNoRetain);
    memory->release();
    return map;
}

int A642LState::read(void *cookie,uint32_t domain,uint32_t offset,uint32_t *value)
{
    A642LState *self=static_cast<A642LState *>(cookie);
    if(!self || !value || domain>=NativeAdrenoA6xxDomains || (offset&3) ||
        !self->maps_[domain] || uint64_t(offset)+4>self->windows_[domain].bytes) {
        return 1;
    }
    const uintptr_t base=self->maps_[domain]->getVirtualAddress();
    *value=*reinterpret_cast<volatile uint32_t *>(base+offset);
    return 0;
}

int A642LState::write(void *cookie,uint32_t domain,uint32_t offset,uint32_t value)
{
    A642LState *self=static_cast<A642LState *>(cookie);
    if(!self || domain>=NativeAdrenoA6xxDomains || (offset&3) ||
        !self->maps_[domain] || uint64_t(offset)+4>self->windows_[domain].bytes) {
        return 1;
    }
    const uintptr_t base=self->maps_[domain]->getVirtualAddress();
    *reinterpret_cast<volatile uint32_t *>(base+offset)=value;
    __asm__ volatile("dsb sy" ::: "memory");
    return 0;
}

void A642LState::delay(void *,uint32_t microseconds)
{
    IODelay(microseconds);
}

int A642LState::gpuRead(void *cookie,uint32_t reg,uint32_t *value)
{
    return reg>UINT32_MAX/4?1:read(cookie,NativeAdrenoA6xxGpu,reg*4,value);
}

int A642LState::gpuWrite(void *cookie,uint32_t reg,uint32_t value)
{
    return reg>UINT32_MAX/4?1:write(cookie,NativeAdrenoA6xxGpu,reg*4,value);
}

int A642LState::barrier(void *)
{
    __asm__ volatile("dsb sy" ::: "memory");
    return 0;
}

int A642LState::publishRing(void *cookie,const volatile uint32_t *,size_t bytes)
{
    A642LState *self=static_cast<A642LState *>(cookie);
    return self->gpuContext_.buffer(1).publishForDevice(0,bytes,false)?1:0;
}

int A642LState::acquireFence(void *cookie,volatile uint32_t *,size_t bytes)
{
    A642LState *self=static_cast<A642LState *>(cookie);
    return self->gpuContext_.buffer(2).acquireDeviceWrites(0,bytes)?1:0;
}

uint64_t A642LState::now(void *)
{
    uint64_t value=0;
    NativeAdrenoKernelTime::now_us(nullptr,&value);
    return value;
}

int A642LState::idle(void *)
{
    IODelay(5);
    return 0;
}

IOReturn A642LState::mapWindows()
{
    if(native_adreno_a6xx_resources(&resources_) || resources_.chipId!=0x06030500 ||
        resources_.gmemBytes!=0x80000 || resources_.frequencyHz!=315000000 ||
        resources_.gpuBank==resources_.gmuBank || resources_.gpuSidMask!=0x400 ||
        resources_.gmuSidMask!=0x400 || resources_.gpuSlot>=127 || resources_.gmuSlot>=127) {
        return kIOReturnBadArgument;
    }
    const NativeAdrenoA6xxWindow supplied[]={
        resources_.gpu,resources_.gmu,resources_.gpucc,resources_.gcc,
        resources_.pdc,resources_.rscc,resources_.commandDb,resources_.rpmh,
        resources_.aop,resources_.ipcc,resources_.cxMisc
    };
    for(unsigned index=0;index<NativeAdrenoA6xxDomains;++index) {
        windows_[index]=supplied[index];
        maps_[index]=mapWindow(windows_[index]);
        if(!maps_[index]) {
            return kIOReturnNoMemory;
        }
    }
    zapMap_=mapWindow(resources_.zap);
    if(!zapMap_) {
        return kIOReturnNoMemory;
    }
    return kIOReturnSuccess;
}

IOReturn A642LState::contexts()
{
    smmuMap_=mapWindow(resources_.smmu);
    if(!smmuMap_) {
        return kIOReturnNoMemory;
    }
    /* The context callbacks retain the mapped register pointer for BO updates. */
    smmuMmio_={reinterpret_cast<volatile unsigned char *>(smmuMap_->getVirtualAddress()),
        size_t(resources_.smmu.bytes)};
    if(ArmSmmu4KBindMmio(&smmuMmio_,&smmuIo_)!=ArmSmmu4KSuccess) {
        return kIOReturnBadArgument;
    }
    NativeAdrenoGmuContextReport report{};
    IOReturn status=gmuContext_.bringUp(&smmuIo_,resources_.gmuBank,resources_.gmuAsid,
        resources_.gmuSlot,uint16_t(resources_.gmuSid),nullptr,&report,
        uint16_t(resources_.gmuSid|resources_.gmuSidMask));
    native_adreno_a6xx_stats[4]=report.step;
    native_adreno_a6xx_stats[5]=report.mismatch;
    if(status) {
        return status;
    }
    NativeAdrenoGmuAllocator &memory=gmuContext_.allocator();
    NativeAdrenoMemory *buffers[]={&memory.icache(),&memory.dummy(),&memory.debug(),&memory.log(),&memory.hfi()};
    for(NativeAdrenoMemory *buffer:buffers) {
        status=buffer->makeWriteCombined();
        if(status) {
            return status;
        }
    }

    const unsigned slots[2]={resources_.gpuSlot,resources_.gpuSlot+1};
    const uint16_t sids[2]={uint16_t(resources_.gpuSid),uint16_t(resources_.gpuSid|resources_.gpuSidMask)};
    status=gpuContext_.bringUp(&smmuIo_,resources_.gpuBank,resources_.gpuAsid,slots,sids,nullptr,&report);
    native_adreno_a6xx_stats[6]=report.step;
    native_adreno_a6xx_stats[7]=report.program_status;
    if(status) {
        return status;
    }
    for(unsigned index=0;index<GpuBufferCount;++index) {
        status=gpuContext_.buffer(index).makeWriteCombined();
        if(status) {
            return status;
        }
    }
    return kIOReturnSuccess;
}

IOReturn A642LState::startCp()
{
    ringIo_={this,gpuWrite,gpuRead,barrier};
    cpTouched_=true;
    const uint32_t clockRegisters[][2]={
        {0x23b09,0x20000},{0x23b0a,0x10111},{0x23b0b,0x5555},{0x1f802,0}
    };
    for(const auto &entry:clockRegisters) {
        if(write(this,NativeAdrenoA6xxGmuRegisters,entry[0]*4-0x6a000,entry[1])) {
            return kIOReturnIOError;
        }
    }
    uint32_t spClock=0;
    if(read(this,NativeAdrenoA6xxGmuRegisters,0x1a880*4-0x6a000,&spClock) ||
        write(this,NativeAdrenoA6xxGmuRegisters,0x1a880*4-0x6a000,spClock&~1u)) {
        return kIOReturnIOError;
    }
    IOReturn status=NativeAdrenoA6xxHwInit::program(&ringIo_,resources_.highestBankBit);
    if(status) {
        return status;
    }
    uint32_t gbif=0;
    if(gpuRead(this,0xe3a,&gbif) || gpuWrite(this,0xe3a,(gbif&~0x70000u)|2) ||
        write(this,NativeAdrenoA6xxGmuRegisters,0x1a880*4-0x6a000,spClock|1) ||
        gpuWrite(this,0xae,0x8aa8aa82)) {
        return kIOReturnIOError;
    }
    if(native_adreno_sqe_firmware_image_size<=4 ||
        native_adreno_sqe_firmware_image_size-4>gpuContext_.buffer(0).bytes()) {
        return kIOReturnBadArgument;
    }
    memcpy(gpuContext_.buffer(0).mapping(),native_adreno_sqe_firmware_image+4,
        size_t(native_adreno_sqe_firmware_image_size-4));
    /* The empty register-list header is valid with IFPC held disabled. */
    memset(gpuContext_.buffer(3).mapping(),0,GpuPageBytes);
    memset(gpuContext_.buffer(4).mapping(),0,GpuPageBytes);
    __asm__ volatile("dsb sy" ::: "memory");

    NativeAdrenoRingCache cache{this,publishRing,acquireFence};
    status=ring_.attach(&ringIo_,&cache,gpuContext_.buffer(1).mapping(),GpuRingBytes,GpuRingIova,
        gpuContext_.buffer(2).mapping(),GpuPageBytes,GpuMemptrsIova,NativeAdrenoA642L);
    if(status) {
        return status;
    }
    native_adreno_fence_phys=gpuContext_.buffer(2).spans()[0].physical;
    NativeAdrenoCpInit cp;
    NativeAdrenoCpInitConfig config{GpuSqeIova,GpuPwrupIova,0,NativeAdrenoA642L};
    uint32_t fence=0;
    status=cp.run(&ringIo_,&ring_,&config,&fence);
    clock_={nullptr,now,idle};
    if(!status && ring_.wait(fence,&clock_,20000,nullptr)!=NativeAdrenoRingFenceComplete) {
        status=kIOReturnTimeout;
    }
    native_adreno_a6xx_stats[8]=fence;
    native_adreno_a6xx_stats[9]=ring_.completedFence();
    if(status) {
        return status;
    }

    IOLog("NativeAdrenoA6xx: CP init completed fence=%u, authenticating PAS 13 shader\n",fence);
    status=zap_.load(resources_.zap,reinterpret_cast<void *>(zapMap_->getVirtualAddress()));
    native_adreno_a6xx_stats[16]=zap_.phase();
    native_adreno_a6xx_stats[17]=uint32_t(zap_.transport());
    native_adreno_a6xx_stats[18]=uint32_t(zap_.result());
    if(status) {
        return status;
    }

    uint32_t words[6]={0};
    if(AdrenoPM4Type7(0x66,1,&words[0])!=AdrenoPM4Success ||
        AdrenoPM4Type7(0x3d,3,&words[2])!=AdrenoPM4Success) {
        return kIOReturnBadArgument;
    }
    words[1]=0;
    words[3]=uint32_t(GpuScratchIova);
    words[4]=uint32_t(GpuScratchIova>>32);
    words[5]=0x6421cafe;
    status=ring_.submit(words,6,&fence);
    if(!status && ring_.wait(fence,&clock_,20000,nullptr)!=NativeAdrenoRingFenceComplete) {
        status=kIOReturnTimeout;
    }
    if(!status) {
        status=gpuContext_.buffer(4).acquireDeviceWrites(0,GpuPageBytes);
    }
    native_adreno_a6xx_stats[10]=*static_cast<volatile uint32_t *>(gpuContext_.buffer(4).mapping());
    if(!status && native_adreno_a6xx_stats[10]!=0x6421cafe) {
        status=kIOReturnIOError;
    }
    if(status) {
        return status;
    }

    status=irq_.attach(&ringIo_,NativeAdrenoA642L);
    if(!status) {
        status=irq_.programMask(nullptr);
    }
    if(!status) {
        status=submit_.attach(gpuContext_.vm(),&ring_);
    }
    if(status) {
        return status;
    }
    NativeAdrenoTimeIo time{this,gpuRead};
    status=timer_.attach(&time,NativeAdrenoTimeCp);
    if(status) {
        return status;
    }
    info_={resources_.chipId,resources_.gmemBytes,0x01000000,0x1ffffffff000,
        0x20000000,0x60000000,resources_.highestBankBit,0,0,3,0,0};
    return native_adreno_publish(gpuContext_.vm(),&submit_,&ring_,&clock_,&timer_,&info_,32,40);
}

IOReturn A642LState::fail(IOReturn status)
{
    native_adreno_a6xx_stats[1]=uint32_t(status);
    native_adreno_fault();
    bool stopped=true;
    if(cpTouched_) {
        stopped=NativeAdrenoA6xxHwInit::stop(&ringIo_,1000)==kIOReturnSuccess;
    }
    const IOReturn gmuStopped=gmu_.stop();
    native_adreno_a6xx_stats[20]=stopped;
    native_adreno_a6xx_stats[21]=uint32_t(gmuStopped);
    if(stopped && !gmuStopped) {
        power_.stop(true);
    }
    dump();
    return status;
}

IOReturn A642LState::start()
{
    native_adreno_a6xx_stats[0]=1;
    IOLog("NativeAdrenoA6xx: mapping SM7325 board resources\n");
    IOReturn status=mapWindows();
    if(status) {
        return fail(status);
    }
    native_adreno_a6xx_stats[0]=2;
    IOLog("NativeAdrenoA6xx: enabling CX clocks and parent votes\n");
    const NativeAdrenoA6xxIo io{this,read,write,delay};
    status=power_.enableCx(io);
    if(status) {
        return fail(status);
    }
    native_adreno_a6xx_stats[0]=3;
    IOLog("NativeAdrenoA6xx: programming GMU and GPU SMMU contexts\n");
    status=contexts();
    if(status) {
        return fail(status);
    }
    NativeAdrenoGmuAllocator &memory=gmuContext_.allocator();
    const auto &layout=memory.layout();
    NativeAdrenoA6xxGmuConfig config{io,native_adreno_firmware_image,
        size_t(native_adreno_firmware_image_size),{
            {memory.icache().mapping(),layout.icache_iova,memory.icache().bytes()},
            {memory.dummy().mapping(),layout.dummy_iova,memory.dummy().bytes()},
            {memory.debug().mapping(),layout.debug_iova,memory.debug().bytes()},
            {memory.log().mapping(),layout.log_iova,memory.log().bytes()},
            {memory.hfi().mapping(),layout.hfi_iova,memory.hfi().bytes()}},&power_.tables()};
    native_adreno_a6xx_stats[0]=4;
    IOLog("NativeAdrenoA6xx: loading A660 GMU and starting HFI\n");
    status=gmu_.start(config);
    if(status) {
        return fail(status);
    }
    native_adreno_a6xx_stats[0]=5;
    IOLog("NativeAdrenoA6xx: requesting GX at 315 MHz\n");
    status=power_.enableGx();
    if(!status) {
        status=gmu_.keepAlive(true);
    }
    if(status) {
        return fail(status);
    }
    native_adreno_a6xx_stats[0]=6;
    IOLog("NativeAdrenoA6xx: starting A642L CP and GPU memory write\n");
    status=startCp();
    if(status) {
        return fail(status);
    }
    ready_=true;
    native_adreno_a6xx_stats[0]=7;
    dump();
    return kIOReturnSuccess;
}

void A642LState::poll()
{
    if(!ready_) {
        return;
    }
    NativeAdrenoIrqResult result{};
    const IOReturn status=irq_.handle(&result);
    native_adreno_a6xx_stats[11]=result.status;
    native_adreno_a6xx_stats[12]=result.cp_status;
    native_adreno_a6xx_stats[13]=result.protect_status;
    uint32_t submitted=0,completed=0;
    const int progress=native_adreno_progress(&submitted,&completed);
    const uint64_t current=now(nullptr);
    if(completed!=watchedFence_ || submitted==completed) {
        watchedFence_=completed;
        watchedSince_=current;
    }
    const bool timeout=submitted!=completed && current-watchedSince_>500000;
    if(status || progress || (result.flags&NativeAdrenoIrqFatal) || timeout) {
        ready_=false;
        native_adreno_a6xx_stats[0]=8;
        fail(status?status:(timeout?kIOReturnTimeout:kIOReturnIOError));
    }
}

void A642LState::dump() const
{
    const auto &power=power_.report();
    const auto &gmu=gmu_.report();
    IOLog("NativeAdrenoA6xx: step=%u status=%08x cp_fence=%u/%u readback=%08x irq=%08x cp=%08x protect=%08x\n",
        native_adreno_a6xx_stats[0],native_adreno_a6xx_stats[1],native_adreno_a6xx_stats[8],
        native_adreno_a6xx_stats[9],native_adreno_a6xx_stats[10],native_adreno_a6xx_stats[11],
        native_adreno_a6xx_stats[12],native_adreno_a6xx_stats[13]);
    IOLog("NativeAdrenoA6xx: power=%u/%08x domain=%u offset=%x value=%x expected=%x mask=%x\n",
        power.phase,power.status,power.domain,power.offset,power.value,power.expected,power.mask);
    IOLog("NativeAdrenoA6xx: gmu=%u/%08x domain=%u offset=%x value=%x hfi=%x/%x ack=%u zap=%u scm=%lld/%lld\n",
        gmu.phase,gmu.status,gmu.domain,gmu.offset,gmu.value,gmu.hfiMessage,gmu.hfiError,
        gmu.acknowledged,zap_.phase(),zap_.transport(),zap_.result());
}

uint64_t vmNow()
{
    uint64_t value=0;
    NativeAdrenoKernelTime::now_us(nullptr,&value);
    return value;
}

void worker(void *,wait_result_t)
{
    thread_set_thread_name(current_thread(),"adreno-a642l");
    OSDictionary *matching=IOService::resourceMatching("IOBSD");
    IOService *provider=matching?IOService::waitForMatchingService(matching,60000000000ULL):nullptr;
    OSSafeReleaseNULL(matching);
    if(!provider) {
        IOLog("NativeAdrenoA6xx: IOBSD resource unavailable\n");
        native_adreno_a6xx_stats[1]=uint32_t(kIOReturnNotReady);
        return;
    }
    provider->release();
    const IOReturn registered=native_adreno_initialize_classes();
    if(registered) {
        IOLog("NativeAdrenoA6xx: runtime class registration failed %08x\n",registered);
        native_adreno_a6xx_stats[1]=uint32_t(registered);
        return;
    }
    native_adreno_vm_now_us=vmNow;
    native_adreno_pool_mib=0;
    state=new A642LState;
    if(!state) {
        native_adreno_a6xx_stats[1]=uint32_t(kIOReturnNoMemory);
        return;
    }
    if(state->start()!=kIOReturnSuccess) {
        return;
    }
    for(;;) {
        IOSleep(10);
        state->poll();
    }
}
}

extern "C" void native_adreno_a6xx_start(void)
{
    unsigned enabled=0;
    if(!PE_parse_boot_argn("native-adreno-a6xx",&enabled,sizeof(enabled)) || !enabled) {
        return;
    }

    uint32_t expected=0;
    if(!__atomic_compare_exchange_n(&launched,&expected,1,false,__ATOMIC_ACQ_REL,__ATOMIC_ACQUIRE)) {
        return;
    }
    thread_t thread=nullptr;
    const kern_return_t status=kernel_thread_start(worker,nullptr,&thread);
    if(status==KERN_SUCCESS) {
        thread_deallocate(thread);
    } else {
        native_adreno_a6xx_stats[1]=uint32_t(status);
    }
}

extern "C" void native_adreno_a6xx_dump_stats(void)
{
    if(state) {
        state->dump();
    }
}
