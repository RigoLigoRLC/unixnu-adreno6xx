/* SPDX-License-Identifier: MIT */
#include "NativeAdrenoA6xxService.h"
#include "NativeAdrenoA6xxGmu.h"
#include "NativeAdrenoA6xxHwInit.h"
#include "NativeAdrenoA6xxZap.h"
#include "NativeAdrenoA6xxSmmu.h"
#include "NativeAdrenoGpuContext.h"
#include "NativeAdrenoGpuProfile.h"
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
#include <kern/clock.h>
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
uint32_t performanceWake = 0;

uint32_t performanceLevelFor(uint64_t frequencyHz)
{
    uint32_t level = 1;
    while (level < NativeAdrenoA6xxOperatingPointCount &&
        NativeAdrenoA6xxOperatingPoints[level - 1].frequencyHz < frequencyHz) {
        ++level;
    }

    return level;
}

/* A642LState owns all mappings, contexts and DMA allocations for this boot.
 * A failed attempt retains them while the GMU and CP are stopped. The CPU
 * framebuffer continues independently of whether this object publishes a GPU.
 *
 * Its hardware worker reads the GMU AO busy counter and selects an operating
 * point every 16 ms. Ring publication only records submission time and wakes
 * that worker after a 50 ms gap. The worker applies the board's temperature
 * cap, waits for the paired frequency/DDR HFI acknowledgement, and sends the
 * resulting state to the bounded capture recorder. The same worker owns fault
 * shutdown and performance-counter OOB acquisition and release. */
class A642LState final {
    OSSharedPtr<IOMemoryMap> maps_[NativeAdrenoA6xxDomains];
    OSSharedPtr<IOMemoryMap> zapMap_;
    OSSharedPtr<IOMemoryMap> smmuMap_;
    NativeAdrenoA6xxResources resources_{};
    NativeAdrenoA6xxWindow windows_[NativeAdrenoA6xxDomains]{};
    NativeAdrenoA6xxPower power_;
    NativeAdrenoA6xxGmu gmu_;
    NativeAdrenoA6xxReport failedPowerReport_{},failedGmuReport_{};
    bool failureCaptured_=false;
    NativeAdrenoA6xxZap zap_;
    NativeAdrenoA6xxSmmu smmuProvider_;
    NativeAdrenoGmuContext gmuContext_;
    NativeAdrenoGpuContext gpuContext_;
    NativeAdrenoGpuContext lpacContext_;
    NativeAdrenoGmuContextReport gmuContextReport_{};
    NativeAdrenoGmuContextReport gpuContextReport_{};
    NativeAdrenoGmuContextReport lpacContextReport_{};
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
    bool cpTrace_=false;
    bool ready_=false;
    bool irqAttached_=false;
    bool hardwareIrqEnabled_=false;
    uint32_t hardwareIrqFailed_=0;
    uint32_t watchedFence_=0;
    uint64_t watchedSince_=0;
    uint64_t nextHealthPollUs_ = 0;
    uint64_t nextPerformancePollUs_ = 0;
    uint64_t lastPerformanceUs_ = 0;
    uint64_t lastBusyTicks_ = 0;
    uint64_t lastActiveUs_ = 0;
    uint64_t continuousBusyUs_ = 0;
    uint64_t boostUntilUs_ = 0;
    uint64_t lastSubmissionUs_ = 0; // Ring publisher writes; hardware worker reads atomically.
    uint32_t activityKick_ = 0;
    uint32_t boostLevel_ = 1;
    uint32_t fixedPerformanceLevel_ = 0;
    bool capturePerformanceEnabled_ = false;
    uint32_t capturePerformanceHz_ = 0;
    uint32_t downWindows_ = 0;
    uint32_t downHold_ = 0;
    NativeAdrenoTracePerformance performance_{};
    unsigned gpuSlots_[2]{},gmuSlots_[2]{},lpacSlots_[2]{};

    static int read(void *,uint32_t,uint32_t,uint32_t *);
    static int write(void *,uint32_t,uint32_t,uint32_t);
    static void delay(void *,uint32_t);
    static int gpuRead(void *,uint32_t,uint32_t *);
    static int gpuWrite(void *,uint32_t,uint32_t);
    static int writeWptr(void *,uint32_t);
    static int readGpuBusy(void *,uint32_t *);
    static int readAlwaysOn(void *, uint64_t *);
    IOReturn readBusyTicks(uint64_t *);
    IOReturn pollPerformance(bool pending);
    static IOReturn acquireProfileAccess(void *);
    static IOReturn releaseProfileAccess(void *);
    static bool profileAccessible(void *);
    static int barrier(void *);
    static int publishRing(void *,const volatile uint32_t *,size_t);
    static int acquireFence(void *,volatile uint32_t *,size_t);
    static uint64_t now(void *);
    static int idle(void *);
    static void gpuInterrupt(void *);
    IOReturn observeInitialization();
    IOReturn checkGpuTranslations();
    IOReturn mapWindows();
    IOReturn contexts();
    IOReturn reserveSmmuResources();
    IOReturn startCp();
    IOReturn fail(IOReturn);
public:
    IOReturn start();
    void poll();
    uint32_t waitMilliseconds() const;
    bool activityPending() const;
    void dump() const;
};

A642LState *state=nullptr;
uint32_t launched=0;
uint32_t hardwareLaunched=0;

OSSharedPtr<IOMemoryMap> mapWindow(const NativeAdrenoA6xxWindow &window,
    IOOptionBits cacheMode=kIOMapInhibitCache)
{
    if(!window.physical || !window.bytes || window.bytes>SIZE_MAX ||
        window.physical>UINT64_MAX-window.bytes) {
        return {};
    }
    IODeviceMemory *memory=IODeviceMemory::withRange(window.physical,window.bytes);
    if(!memory) {
        return {};
    }
    OSSharedPtr<IOMemoryMap> map(memory->map(kIOMapAnywhere|cacheMode),OSNoRetain);
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
    if(self->cpTrace_) {
        IOLog("NativeAdrenoCpMmio: read begin domain=%u offset=%x reg=%x\n",domain,offset,offset/4);
    }
    *value=*reinterpret_cast<volatile uint32_t *>(base+offset);
    __asm__ volatile("dsb ld" ::: "memory");
    if(self->cpTrace_) {
        IOLog("NativeAdrenoCpMmio: read end domain=%u offset=%x value=%08x\n",domain,offset,*value);
    }
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
    if(self->cpTrace_) {
        IOLog("NativeAdrenoCpMmio: write begin domain=%u offset=%x reg=%x value=%08x\n",domain,offset,offset/4,value);
    }
    __asm__ volatile("dsb st" ::: "memory");
    *reinterpret_cast<volatile uint32_t *>(base+offset)=value;
    if(self->cpTrace_) {
        IOLog("NativeAdrenoCpMmio: write end domain=%u offset=%x\n",domain,offset);
    }
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

int A642LState::writeWptr(void *cookie,uint32_t value)
{
    auto *self=static_cast<A642LState *>(cookie);
    if(self->cpTrace_) {
        IOLog("NativeAdrenoCp: WPTR publication begin value=%x\n",value);
    }

    for(unsigned attempt=0;attempt<200;++attempt) {
        if(gpuWrite(cookie,0x807,value)) {
            return 1;
        }

        if(self->cpTrace_ && !attempt) {
            IOLog("NativeAdrenoCp: WPTR store completed; synchronizing\n");
        }

        __asm__ volatile("dsb sy" ::: "memory");

        uint32_t fence=0;
        if(read(cookie,NativeAdrenoA6xxGmuRegisters,0x23b13*4-0x6a000,&fence)) {
            return 1;
        }

        if(!(fence&1)) {
            if(self->cpTrace_) {
                IOLog("NativeAdrenoCp: WPTR acknowledged attempts=%u fence=%08x\n",attempt+1,fence);
            }

            const uint64_t submittedUs = now(nullptr);
            const uint64_t previousUs = __atomic_exchange_n(&self->lastSubmissionUs_,
                submittedUs, __ATOMIC_RELAXED);
            if (!previousUs || submittedUs - previousUs >= 50000) {
                __atomic_store_n(&self->activityKick_, 1, __ATOMIC_RELEASE);
                thread_wakeup(&performanceWake);
            }

            return 0;
        }

        IODelay(10);
    }

    IOLog("NativeAdrenoCp: WPTR acknowledgement timed out value=%x\n",value);
    return 1;
}

int A642LState::readGpuBusy(void *cookie,uint32_t *value)
{
    return read(cookie,NativeAdrenoA6xxGmuRegisters,0x23b0c*4-0x6a000,value);
}

IOReturn A642LState::readBusyTicks(uint64_t *ticks)
{
    // XOCLK0 belongs to GMU AO and counts busy time at 19.2 MHz.
    for (unsigned attempt = 0; attempt < 4; ++attempt) {
        uint32_t before = 0;
        uint32_t low = 0;
        uint32_t after = 0;
        if (read(this, NativeAdrenoA6xxGmuRegisters, 0x1f845 * 4 - 0x6a000, &before) ||
            read(this, NativeAdrenoA6xxGmuRegisters, 0x1f844 * 4 - 0x6a000, &low) ||
            read(this, NativeAdrenoA6xxGmuRegisters, 0x1f845 * 4 - 0x6a000, &after)) {
            return kIOReturnIOError;
        }
        if (before == after) {
            *ticks = uint64_t(after) << 32 | low;
            return kIOReturnSuccess;
        }
    }

    return kIOReturnIOError;
}

/* pollPerformance applies pandora_gpu.c's 16 ms, 50-percent busy policy to
 * SM7325's three stock operating points. It raises to the lowest sufficient
 * frequency with ten-percent headroom, delays a one-level decrease for four
 * low-load windows, and preserves the activity boost for 50 ms. */
IOReturn A642LState::pollPerformance(bool pending)
{
    uint64_t busyTicks = 0;
    IOReturn status = readBusyTicks(&busyTicks);
    if (status) {
        return status;
    }

    const uint64_t currentUs = now(nullptr);
    const uint64_t intervalNs = (currentUs - lastPerformanceUs_) * 1000;
    const bool busyValid = lastPerformanceUs_ && intervalNs && busyTicks >= lastBusyTicks_;
    const uint64_t deltaTicks = busyValid ? busyTicks - lastBusyTicks_ : 0;
    const uint64_t busyNs = deltaTicks > intervalNs * 12 / 625 ?
        intervalNs : deltaTicks * 625 / 12;
    const uint32_t busyPermille = busyValid ? uint32_t(busyNs * 1000 / intervalNs) : 0;
    const uint32_t currentLevel = gmu_.acknowledgedPerformance();
    const uint32_t currentHz = NativeAdrenoA6xxOperatingPoints[currentLevel - 1].frequencyHz;
    uint32_t wantedLevel = currentLevel;

    const bool kicked = __atomic_exchange_n(&activityKick_, 0, __ATOMIC_ACQ_REL) != 0;
    if (kicked) {
        boostLevel_ = performanceLevelFor(uint64_t(currentHz) * 2);
        boostUntilUs_ = currentUs + 50000;
    }
    const bool boosted = currentUs < boostUntilUs_;
    const bool active = busyNs || pending;
    if (busyValid) {
        const uint64_t neededHz = uint64_t(busyPermille) * currentHz * 2 / 1000;
        const uint32_t sufficientLevel = performanceLevelFor(neededHz * 11 / 10);
        continuousBusyUs_ = busyPermille >= 950 ?
            continuousBusyUs_ + intervalNs / 1000 : 0;
        if (busyPermille > 850 || continuousBusyUs_ >= 50000) {
            wantedLevel = NativeAdrenoA6xxOperatingPointCount;
            downWindows_ = 0;
        } else if (sufficientLevel > currentLevel) {
            wantedLevel = sufficientLevel;
            downWindows_ = 0;
        } else if (active) {
            if (currentLevel > 1 && neededHz * 10 <=
                uint64_t(NativeAdrenoA6xxOperatingPoints[currentLevel - 2].frequencyHz) * 8) {
                if (++downWindows_ >= 4 + downHold_) {
                    wantedLevel = currentLevel - 1;
                }
            } else {
                downWindows_ = 0;
            }
        } else if (currentUs - lastActiveUs_ >= 80000) {
            wantedLevel = 1;
            downWindows_ = 0;
        }
    }
    if (active) {
        lastActiveUs_ = currentUs;
    }
    if (boosted && wantedLevel < boostLevel_) {
        wantedLevel = boostLevel_;
    }
    uint32_t captureHz = 0;
    const bool captureEnabled = NativeAdrenoGpuProfile::captureFrequency(&captureHz);
    const uint32_t fixedLevel = captureEnabled ?
        (captureHz ? performanceLevelFor(captureHz) : 0) : fixedPerformanceLevel_;
    if (fixedLevel) {
        wantedLevel = fixedLevel;
    }

    NativeAdrenoTracePerformance sample{};
    sample.thermal_cap_hz = native_adreno_thermal_cap_hz(&sample.temperature_millic,
        &sample.thermal_valid_mask, &sample.thermal_flags);
    uint32_t capLevel = NativeAdrenoA6xxOperatingPointCount;
    while (capLevel > 1 &&
        NativeAdrenoA6xxOperatingPoints[capLevel - 1].frequencyHz > sample.thermal_cap_hz) {
        --capLevel;
    }
    const bool thermalLimited = wantedLevel > capLevel;
    if (thermalLimited) {
        wantedLevel = capLevel;
    }

    sample.busy_ticks = deltaTicks;
    sample.interval_ns = busyValid ? intervalNs : 0;
    sample.busy_permille = busyPermille;
    sample.requested_hz = NativeAdrenoA6xxOperatingPoints[wantedLevel - 1].frequencyHz;
    sample.flags = (busyValid ? NativeAdrenoTracePerformanceBusyValid : 0) |
        (boosted && !fixedLevel ? NativeAdrenoTracePerformanceBoost : 0) |
        (fixedLevel ? NativeAdrenoTracePerformanceFixedFrequency : 0) |
        (pending ? NativeAdrenoTracePerformancePendingWork : 0) |
        (thermalLimited ? NativeAdrenoTracePerformanceThermalLimited : 0);

    if (wantedLevel != currentLevel) {
        status = gmu_.setPerformance(wantedLevel);
        if (!status) {
            sample.flags |= NativeAdrenoTracePerformanceVoteChanged;
            downHold_ = wantedLevel > currentLevel ? 1 : 0;
        }
        downWindows_ = 0;
    }
    const uint32_t acknowledgedLevel = gmu_.acknowledgedPerformance();
    const auto &acknowledged = NativeAdrenoA6xxOperatingPoints[acknowledgedLevel - 1];
    sample.acknowledged_hz = acknowledged.frequencyHz;
    sample.bandwidth_index = acknowledged.bandwidthIndex;
    sample.vote_status = uint32_t(status);
    sample.timestamp_ns = now(nullptr) * 1000;
    performance_ = sample;
    capturePerformanceEnabled_ = captureEnabled;
    capturePerformanceHz_ = captureHz;
    NativeAdrenoGpuProfile::recordPerformance(sample);

    lastPerformanceUs_ = currentUs;
    lastBusyTicks_ = busyTicks;
    nextPerformancePollUs_ = currentUs + 16000;
    return status;
}

bool A642LState::profileAccessible(void *cookie)
{
    uint32_t gx=0,fence=0;
    return !read(cookie,NativeAdrenoA6xxGmuRegisters,0x1f8d0*4-0x6a000,&gx) &&
        !read(cookie,NativeAdrenoA6xxGmuRegisters,0x23b10*4-0x6a000,&fence) &&
        !(gx&0xc0) && !fence;
}

int A642LState::readAlwaysOn(void *cookie, uint64_t *ticks)
{
    if (!ticks) {
        return 1;
    }

    // Samsung a6xx_read_alwayson uses GMU AO for CPU AHB reads on this target.
    for (unsigned attempt = 0; attempt < 4; ++attempt) {
        uint32_t before = 0;
        uint32_t low = 0;
        uint32_t after = 0;
        if (read(cookie, NativeAdrenoA6xxGmuRegisters, 0x1f889 * 4 - 0x6a000, &before) ||
            read(cookie, NativeAdrenoA6xxGmuRegisters, 0x1f888 * 4 - 0x6a000, &low) ||
            read(cookie, NativeAdrenoA6xxGmuRegisters, 0x1f889 * 4 - 0x6a000, &after)) {
            return 1;
        }
        if (before == after) {
            *ticks = (uint64_t(after) << 32) | low;
            return 0;
        }
    }

    return 1;
}

IOReturn A642LState::acquireProfileAccess(void *cookie)
{
    return static_cast<A642LState *>(cookie)->gmu_.acquirePerformanceCounters();
}

IOReturn A642LState::releaseProfileAccess(void *cookie)
{
    return static_cast<A642LState *>(cookie)->gmu_.releasePerformanceCounters();
}

int A642LState::barrier(void *)
{
    __asm__ volatile("dsb sy" ::: "memory");
    return 0;
}

int A642LState::publishRing(void *cookie,const volatile uint32_t *,size_t bytes)
{
    A642LState *self=static_cast<A642LState *>(cookie);
    if(self->cpTrace_) {
        IOLog("NativeAdrenoCp: ring publication begin bytes=%lx\n",bytes);
    }
    const IOReturn status=self->gpuContext_.buffer(1).publishForDevice(0,bytes);
    if(self->cpTrace_) {
        IOLog("NativeAdrenoCp: ring publication end status=%08x\n",status);
    }
    return status?1:0;
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

int A642LState::idle(void *cookie)
{
    IODelay(5);
    return static_cast<A642LState *>(cookie)->observeInitialization()?1:0;
}

IOReturn A642LState::observeInitialization()
{
    IOReturn status=smmuProvider_.observe();
    if(!status) {
        status=gmu_.poll();
    }
    if(status || !irqAttached_ || hardwareIrqEnabled_) {
        return status;
    }
    NativeAdrenoIrqResult result{};
    status=irq_.handle(&result);
    if(status || (result.flags&NativeAdrenoIrqFatal)) {
        IOLog("NativeAdrenoCp: initialization fault status=%08x rbbm=%08x cp=%08x protect=%08x flags=%08x\n",
            status,result.status,result.cp_status,result.protect_status,result.flags);
        return status?status:kIOReturnIOError;
    }
    return kIOReturnSuccess;
}

void A642LState::gpuInterrupt(void *cookie)
{
    A642LState *self=static_cast<A642LState *>(cookie);
    NativeAdrenoIrqResult result{};
    const IOReturn status=self->irq_.handle(&result);
    native_adreno_a6xx_stats[11]=result.status;
    native_adreno_a6xx_stats[12]=result.cp_status;
    native_adreno_a6xx_stats[13]=result.protect_status;
    const bool failed=status || (result.flags&NativeAdrenoIrqFatal);
    if(failed) {
        __atomic_store_n(&self->hardwareIrqFailed_,1,__ATOMIC_RELEASE);
        native_adreno_smmu_disable_irq(self->resources_.gpuInterrupt);
    }
    if(failed || (result.flags&NativeAdrenoIrqFence)) {
        native_adreno_gpu_interrupt(failed);
    }
}

IOReturn A642LState::checkGpuTranslations()
{
    const uint64_t addresses[]={GpuSqeIova,GpuRingIova,GpuMemptrsIova,GpuPwrupIova,GpuScratchIova};
    const unsigned banks[]={resources_.gpuBank,resources_.gpuBank,resources_.lpacBank,resources_.lpacBank};
    const uint16_t sids[]={uint16_t(resources_.gpuSid),uint16_t(resources_.gpuSid|resources_.gpuSidMask),
        uint16_t(resources_.lpacSid),uint16_t(resources_.lpacSid|resources_.lpacSidMask)};
    for(unsigned stream=0;stream<4;++stream) {
        const uint16_t sid=sids[stream];
        for(unsigned index=0;index<GpuBufferCount;++index) {
            const NativeAdrenoMemory &buffer=gpuContext_.buffer(index);
            if(buffer.spanCount()!=1) {
                return kIOReturnUnsupported;
            }
            const bool privileged=index!=4;
            const bool instruction=index==0;
            const bool writes=index>=2;
            for(size_t offset=0;offset<buffer.bytes();offset+=4096) {
                const IOReturn status=smmuProvider_.checkTranslation(banks[stream],sid,
                    addresses[index]+offset,buffer.spans()[0].physical+offset,
                    privileged,instruction,writes);
                if(status) {
                    return status;
                }
            }
        }
    }
    IOLog("NativeAdrenoSmmu: all four GPU and LPAC streams translated every bootstrap page\n");
    return smmuProvider_.observe();
}

IOReturn A642LState::mapWindows()
{
    if(native_adreno_a6xx_resources(&resources_) || resources_.chipId!=0x06030500 ||
        resources_.gmemBytes!=0x80000 || resources_.frequencyHz!=315000000 || resources_.ddrType!=7 ||
        resources_.gpuSidMask!=0x400 || resources_.gmuSidMask!=0x400 || resources_.lpacSidMask!=0x400 ||
        resources_.gpuSid>0xffff || resources_.gmuSid>0xffff ||
        resources_.lpacSid!=1 || resources_.gpuSid!=0 || resources_.gmuSid!=5 ||
        !resources_.gpuAsid || resources_.gpuAsid>0xffff ||
        !resources_.gmuAsid || resources_.gmuAsid>0xffff ||
        !resources_.lpacAsid || resources_.lpacAsid>0xffff ||
        resources_.gpuAsid==resources_.gmuAsid || resources_.gpuAsid==resources_.lpacAsid ||
        resources_.gmuAsid==resources_.lpacAsid) {
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
    // The ZAP reservation is DDR loaded through Normal NC, matching PIL's ioremap_wc.
    zapMap_=mapWindow(resources_.zap,kIOMapWriteCombineCache);
    if(!zapMap_) {
        return kIOReturnNoMemory;
    }
    return kIOReturnSuccess;
}

IOReturn A642LState::reserveSmmuResources()
{
    uint32_t id0=0,id1=0;
    if(smmuIo_.read32(smmuIo_.cookie,0x20,&id0) ||
        smmuIo_.read32(smmuIo_.cookie,0x24,&id1)) {
        return kIOReturnIOError;
    }
    const unsigned slotCount=id0&255,bankCount=id1&255,firstStage1=(id1>>16)&255;
    const size_t stride=size_t(1)<<((id1>>31)?16:12);
    const size_t pages=size_t(1)<<(((id1>>28)&7)+1);
    const bool extended=id0&(1u<<8);
    if(!(id0&(1u<<27)) || slotCount<6 || slotCount>128 || bankCount>64 ||
        firstStage1>=bankCount || 2*pages*stride>smmuIo_.register_bytes) {
        IOLog("NativeAdrenoSmmu: allocation unsupported id=%08x/%08x slots=%u stage1=%u..%u\n",
            id0,id1,slotCount,firstStage1,bankCount);
        return kIOReturnUnsupported;
    }
    bool freeSlots[128]={},reservedSlots[128]={},referencedBanks[64]={},freeBanks[64]={};
    const uint16_t requestedSids[6]={uint16_t(resources_.gpuSid),
        uint16_t(resources_.gpuSid|resources_.gpuSidMask),uint16_t(resources_.gmuSid),
        uint16_t(resources_.gmuSid|resources_.gmuSidMask),uint16_t(resources_.lpacSid),
        uint16_t(resources_.lpacSid|resources_.lpacSidMask)};
    const uint32_t sidBits=(id0>>9)&15;
    const uint32_t sidMask=extended?0xffffu:((1u<<sidBits)-1);
    for(unsigned slot=0;slot<slotCount;++slot) {
        uint32_t smr=0,s2cr=0;
        if(smmuIo_.read32(smmuIo_.cookie,0x800+4*slot,&smr) ||
            smmuIo_.read32(smmuIo_.cookie,0xc00+4*slot,&s2cr)) {
            return kIOReturnIOError;
        }
        const bool valid=extended?bool(s2cr&(1u<<10)):bool(smr&(1u<<31));
        const unsigned type=(s2cr>>16)&3,bank=s2cr&255;
        freeSlots[slot]=!valid && type==2;
        if(valid && !type && bank<64) {
            referencedBanks[bank]=true;
        }
        if(valid) {
            const uint32_t mask=(smr>>16)&(extended?0xffffu:0x7fffu);
            for(uint16_t sid:requestedSids) {
                if(!((sid^(smr&0xffff))&~mask&sidMask)) {
                    IOLog("NativeAdrenoSmmu: allocation conflicts slot=%u smr=%08x s2cr=%08x sid=%04x\n",
                        slot,smr,s2cr,sid);
                    return kIOReturnBusy;
                }
            }
        }
    }
    for(unsigned bank=firstStage1;bank<bankCount;++bank) {
        const size_t offset=(pages+bank)*stride;
        uint32_t sctlr=0;
        if(offset>smmuIo_.register_bytes || smmuIo_.register_bytes-offset<0x7f8 ||
            smmuIo_.read32(smmuIo_.cookie,offset,&sctlr)) {
            return kIOReturnIOError;
        }
        freeBanks[bank]=!(sctlr&1) && !referencedBanks[bank];
    }

    unsigned selectedSlots[3][2]={{UINT32_MAX,UINT32_MAX},{UINT32_MAX,UINT32_MAX},{UINT32_MAX,UINT32_MAX}};
    const unsigned preferredSlots[3]={resources_.gpuSlot,resources_.gmuSlot,resources_.lpacSlot};
    for(unsigned role=0;role<3;++role) {
        const unsigned first=preferredSlots[role];
        if(first<slotCount && first+1<slotCount && freeSlots[first] && freeSlots[first+1] &&
            !reservedSlots[first] && !reservedSlots[first+1]) {
            selectedSlots[role][0]=first;
            selectedSlots[role][1]=first+1;
            reservedSlots[first]=true;
            reservedSlots[first+1]=true;
        }
    }
    for(unsigned role=0;role<3;++role) {
        for(unsigned index=0;index<2;++index) {
            if(selectedSlots[role][index]!=UINT32_MAX) {
                continue;
            }
            for(unsigned slot=0;slot<slotCount;++slot) {
                if(freeSlots[slot] && !reservedSlots[slot]) {
                    selectedSlots[role][index]=slot;
                    reservedSlots[slot]=true;
                    break;
                }
            }
            if(selectedSlots[role][index]==UINT32_MAX) {
                IOLog("NativeAdrenoSmmu: six free stream slots unavailable count=%u\n",slotCount);
                return kIOReturnNoResources;
            }
        }
    }
    unsigned selectedBanks[3]={UINT32_MAX,UINT32_MAX,UINT32_MAX};
    const unsigned preferredBanks[3]={resources_.gpuBank,resources_.gmuBank,resources_.lpacBank};
    for(unsigned role=0;role<3;++role) {
        const unsigned bank=preferredBanks[role];
        if(bank>=firstStage1 && bank<bankCount && freeBanks[bank]) {
            selectedBanks[role]=bank;
            freeBanks[bank]=false;
        }
    }
    for(unsigned role=0;role<3;++role) {
        if(selectedBanks[role]!=UINT32_MAX) {
            continue;
        }
        for(unsigned bank=firstStage1;bank<bankCount;++bank) {
            if(freeBanks[bank]) {
                selectedBanks[role]=bank;
                freeBanks[bank]=false;
                break;
            }
        }
        if(selectedBanks[role]==UINT32_MAX) {
            IOLog("NativeAdrenoSmmu: three disabled unreferenced context banks unavailable\n");
            return kIOReturnNoResources;
        }
    }
    IOLog("NativeAdrenoSmmu: allocated slots=%u banks=%u GPU cb=%u slots=%u/%u SID=%04x/%04x GMU cb=%u slots=%u/%u SID=%04x/%04x LPAC cb=%u slots=%u/%u SID=%04x/%04x\n",
        slotCount,bankCount,selectedBanks[0],selectedSlots[0][0],selectedSlots[0][1],
        requestedSids[0],requestedSids[1],selectedBanks[1],selectedSlots[1][0],selectedSlots[1][1],
        requestedSids[2],requestedSids[3],selectedBanks[2],selectedSlots[2][0],selectedSlots[2][1],
        requestedSids[4],requestedSids[5]);

    /* These invalid slots fault transactions with the board's non-shareable
     * override before the context objects attach their exact SID routes. */
    for(unsigned role=0;role<3;++role) {
        for(unsigned index=0;index<2;++index) {
            const unsigned slot=selectedSlots[role][index];
            uint32_t smr=0,before=0,after=0;
            if(smmuIo_.read32(smmuIo_.cookie,0x800+4*slot,&smr) ||
                smmuIo_.read32(smmuIo_.cookie,0xc00+4*slot,&before)) {
                return kIOReturnIOError;
            }
            const bool valid=extended?bool(before&(1u<<10)):bool(smr&(1u<<31));
            if(valid || ((before>>16)&3)!=2) {
                IOLog("NativeAdrenoSmmu: reservation changed slot=%u smr=%08x s2cr=%08x\n",slot,smr,before);
                return kIOReturnBusy;
            }
            const uint32_t faultRoute=(2u<<16)|(3u<<8);
            if(smmuIo_.write32(smmuIo_.cookie,0xc00+4*slot,faultRoute) ||
                smmuIo_.barrier(smmuIo_.cookie) ||
                smmuIo_.read32(smmuIo_.cookie,0xc00+4*slot,&after)) {
                return kIOReturnIOError;
            }
            IOLog("NativeAdrenoSmmu: reserved slot=%u fault_before=%08x fault_after=%08x\n",slot,before,after);
            if(after!=faultRoute) {
                return kIOReturnIOError;
            }
        }
    }
    resources_.gpuBank=selectedBanks[0];
    resources_.gmuBank=selectedBanks[1];
    resources_.lpacBank=selectedBanks[2];
    for(unsigned index=0;index<2;++index) {
        gpuSlots_[index]=selectedSlots[0][index];
        gmuSlots_[index]=selectedSlots[1][index];
        lpacSlots_[index]=selectedSlots[2][index];
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
    constexpr uint32_t cm3ResetOffset=0x1f800*4-0x6a000;
    uint32_t resetBefore=0,resetAfter=0;
    if(read(this,NativeAdrenoA6xxGmuRegisters,cm3ResetOffset,&resetBefore) ||
        write(this,NativeAdrenoA6xxGmuRegisters,cm3ResetOffset,1)) {
        IOLog("NativeAdrenoSmmu: GMU CM3 reset access failed\n");
        return kIOReturnIOError;
    }
    IODelay(1);
    if(read(this,NativeAdrenoA6xxGmuRegisters,cm3ResetOffset,&resetAfter) || !(resetAfter&1)) {
        IOLog("NativeAdrenoSmmu: GMU CM3 reset did not assert before=%08x after=%08x\n",resetBefore,resetAfter);
        return kIOReturnIOError;
    }
    IOLog("NativeAdrenoSmmu: GMU CM3 held reset before=%08x after=%08x\n",resetBefore,resetAfter);
    NativeAdrenoSnapshotSmmu(&smmuIo_,"before-platform-setup");

    IOReturn status=reserveSmmuResources();
    if(status) {
        return status;
    }
    status=smmuProvider_.prepare(resources_,smmuIo_);
    if(status) {
        return status;
    }
    NativeAdrenoGmuContextReport &report=gmuContextReport_;
    status=gmuContext_.bringUp(&smmuIo_,resources_.gmuBank,resources_.gmuAsid,
        gmuSlots_[0],uint16_t(resources_.gmuSid),nullptr,&report,
        uint16_t(resources_.gmuSid|resources_.gmuSidMask),gmuSlots_[1]);
    native_adreno_a6xx_stats[4]=report.step;
    native_adreno_a6xx_stats[5]=report.mismatch;
    NativeAdrenoLogSmmuReport("gmu",&report);
    if(status) {
        if(report.program_writes) {
            NativeAdrenoSnapshotSmmu(&smmuIo_,"after-gmu-failure");
        }
        return status;
    }
    const uint16_t sids[2]={uint16_t(resources_.gpuSid),uint16_t(resources_.gpuSid|resources_.gpuSidMask)};
    status=gpuContext_.bringUp(&smmuIo_,resources_.gpuBank,resources_.gpuAsid,gpuSlots_,sids,nullptr,&gpuContextReport_);
    native_adreno_a6xx_stats[6]=gpuContextReport_.step;
    native_adreno_a6xx_stats[7]=gpuContextReport_.program_status;
    NativeAdrenoLogSmmuReport("gpu",&gpuContextReport_);
    if(status) {
        if(gpuContextReport_.program_writes) {
            NativeAdrenoSnapshotSmmu(&smmuIo_,"after-gpu-failure");
        }
        return status;
    }
    const uint16_t lpacSids[2]={uint16_t(resources_.lpacSid),
        uint16_t(resources_.lpacSid|resources_.lpacSidMask)};
    status=lpacContext_.bringUpShared(&smmuIo_,resources_.lpacBank,resources_.lpacAsid,
        lpacSlots_,lpacSids,&gpuContext_,&lpacContextReport_);
    NativeAdrenoLogSmmuReport("lpac",&lpacContextReport_);
    if(status) {
        if(lpacContextReport_.program_writes) {
            NativeAdrenoSnapshotSmmu(&smmuIo_,"after-lpac-failure");
        }
        return status;
    }
    return kIOReturnSuccess;
}

IOReturn A642LState::startCp()
{
    ringIo_={this,gpuWrite,gpuRead,barrier,writeWptr,readGpuBusy,readAlwaysOn};
    IOLog("NativeAdrenoCp: checking GMU keepalive and full AHB fence before GX registers\n");
    IOReturn status=gmu_.keepAlive(true);
    if(status) {
        return status;
    }
    cpTrace_=true;
    uint32_t gxStatus=0,fenceControl=0,gxDomain=0,spClock=0;
    if(read(this,NativeAdrenoA6xxGmuRegisters,0x1f8d0*4-0x6a000,&gxStatus) ||
        read(this,NativeAdrenoA6xxGmuRegisters,0x23b10*4-0x6a000,&fenceControl) ||
        read(this,NativeAdrenoA6xxGpuCc,0x100c,&gxDomain) ||
        read(this,NativeAdrenoA6xxGmuRegisters,0x1a880*4-0x6a000,&spClock)) {
        return kIOReturnIOError;
    }
    IOLog("NativeAdrenoCp: GX power/clock=%08x AHB fence=%08x GX GDSCR=%08x SP clock=%08x\n",
        gxStatus,fenceControl,gxDomain,spClock);
    if((gxStatus&0xc0) || fenceControl || !(gxDomain&(1u<<31))) {
        return kIOReturnNotReady;
    }
    cpTouched_=true;
    IOLog("NativeAdrenoCp: core register initialization begin with SP clock retained\n");
    status=NativeAdrenoA6xxHwInit::program(&ringIo_,resources_.highestBankBit);
    if(status) {
        return status;
    }

    const NativeAdrenoGpuProfileAccess profileAccess{
        acquireProfileAccess, releaseProfileAccess, profileAccessible
    };
    status=NativeAdrenoGpuProfile::initialize(&ringIo_,profileAccess);
    if(status) {
        return status;
    }

    const uint32_t counterRegisters[]={0x1f841,0x1f842};
    const uint32_t counterSelectors[]={0x20,0x04};
    for(unsigned index=0;index<2;++index) {
        const uint32_t offset=counterRegisters[index]*4-0x6a000;
        uint32_t previous=0;
        if(read(this,NativeAdrenoA6xxGmuRegisters,offset,&previous) ||
            write(this,NativeAdrenoA6xxGmuRegisters,offset,(previous&~0xffu)|counterSelectors[index])) {
            return kIOReturnIOError;
        }
    }

    if(write(this,NativeAdrenoA6xxGmuRegisters,0x23b0e*4-0x6a000,0xff000000) ||
        write(this,NativeAdrenoA6xxGmuRegisters,0x1f840*4-0x6a000,1)) {
        return kIOReturnIOError;
    }

    IOLog("NativeAdrenoCp: GMU hardware clock controls begin\n");
    const uint32_t clockRegisters[][2]={
        {0x23b09,0x20000},{0x23b0a,0x10111},{0x23b0b,0x5555},{0x1f802,0}
    };
    for(const auto &entry:clockRegisters) {
        if(write(this,NativeAdrenoA6xxGmuRegisters,entry[0]*4-0x6a000,entry[1])) {
            return kIOReturnIOError;
        }
    }

    uint32_t topClock=0;
    if(gpuRead(this,0xae,&topClock)) {
        return kIOReturnIOError;
    }

    IOLog("NativeAdrenoCp: inherited RBBM_CLOCK_CNTL=%08x\n",topClock);
    if(topClock!=0x8aa8aa82) {
        if(read(this,NativeAdrenoA6xxGmuRegisters,0x1a880*4-0x6a000,&spClock) ||
            write(this,NativeAdrenoA6xxGmuRegisters,0x1a880*4-0x6a000,spClock&~1u)) {
            return kIOReturnIOError;
        }

        status=NativeAdrenoA6xxHwInit::programClockGating(&ringIo_);
        if(status) {
            return status;
        }

        uint32_t gbif=0;
        if(gpuRead(this,0xe3a,&gbif) || gpuWrite(this,0xe3a,(gbif&~0x70000u)|2) ||
            read(this,NativeAdrenoA6xxGmuRegisters,0x1a880*4-0x6a000,&spClock) ||
            write(this,NativeAdrenoA6xxGmuRegisters,0x1a880*4-0x6a000,spClock|1) ||
            gpuWrite(this,0xae,0x8aa8aa82)) {
            return kIOReturnIOError;
        }
    } else {
        IOLog("NativeAdrenoCp: hardware clock gating already configured\n");
    }

    status=NativeAdrenoA6xxHwInit::finish(&ringIo_);
    if(status) {
        return status;
    }

    IOLog("NativeAdrenoCp: checking TBU translations before SQE start\n");
    status=checkGpuTranslations();
    if(status) {
        return status;
    }
    status=irq_.attach(&ringIo_,NativeAdrenoA642L);
    if(!status) {
        irqAttached_=true;
        status=irq_.programMask(nullptr);
    }
    if(status) {
        return status;
    }

    if(native_adreno_sqe_firmware_image_size<=4 ||
        native_adreno_sqe_firmware_image_size-4>gpuContext_.buffer(0).bytes()) {
        return kIOReturnBadArgument;
    }
    IOLog("NativeAdrenoCp: SQE copy begin cpu=%p physical=%llx iova=%llx bytes=%llx\n",
        gpuContext_.buffer(0).mapping(),gpuContext_.buffer(0).spans()[0].physical,
        uint64_t(GpuSqeIova),native_adreno_sqe_firmware_image_size-4);
    memcpy(gpuContext_.buffer(0).mapping(),native_adreno_sqe_firmware_image+4,
        size_t(native_adreno_sqe_firmware_image_size-4));
    IOLog("NativeAdrenoCp: SQE copy completed\n");
    /* The empty register-list header is valid with IFPC held disabled. */
    memset(gpuContext_.buffer(3).mapping(),0,GpuPageBytes);
    memset(gpuContext_.buffer(4).mapping(),0,GpuPageBytes);
    IOLog("NativeAdrenoCp: SQE publication begin\n");
    status=gpuContext_.buffer(0).publishForDevice(0,gpuContext_.buffer(0).bytes());
    IOLog("NativeAdrenoCp: SQE publication completed status=%08x\n",status);
    if(status) {
        return status;
    }
    __asm__ volatile("dsb sy" ::: "memory");

    uint32_t inheritedRptr=0,inheritedWptr=0,inheritedSqe=0;
    if(gpuRead(this,0x806,&inheritedRptr) ||
        gpuRead(this,0x807,&inheritedWptr) ||
        gpuRead(this,0x808,&inheritedSqe)) {
        return kIOReturnIOError;
    }
    IOLog("NativeAdrenoCp: inherited ring rptr=%08x wptr=%08x sqe=%08x\n",
        inheritedRptr,inheritedWptr,inheritedSqe);
    if(inheritedRptr || inheritedWptr) {
        return kIOReturnNotReady;
    }

    NativeAdrenoRingCache cache{this,publishRing,acquireFence};
    IOLog("NativeAdrenoCp: ring attach begin ring=%llx fence=%llx\n",uint64_t(GpuRingIova),uint64_t(GpuMemptrsIova));
    status=ring_.attach(&ringIo_,&cache,gpuContext_.buffer(1).mapping(),GpuRingBytes,GpuRingIova,
        gpuContext_.buffer(2).mapping(),GpuPageBytes,GpuMemptrsIova,NativeAdrenoA642L);
    IOLog("NativeAdrenoCp: ring attach completed status=%08x\n",status);
    if(status) {
        return status;
    }
    native_adreno_fence_phys=gpuContext_.buffer(2).spans()[0].physical;
    NativeAdrenoCpInit cp;
    NativeAdrenoCpInitConfig config{GpuSqeIova,GpuPwrupIova,0,NativeAdrenoA642L};
    uint32_t bootstrapWptr=0;
    IOLog("NativeAdrenoCp: SQE start and CP_ME_INIT submit begin\n");
    cpTrace_=false;
    status=cp.runBootstrap(&ringIo_,&ring_,&config,&bootstrapWptr);
    cpTrace_=true;
    IOLog("NativeAdrenoCp: CP_ME_INIT submit completed status=%08x wptr=%u\n",status,bootstrapWptr);
    clock_={this,now,idle};
    cpTrace_=false;
    uint32_t observedRptr=0;
    IOLog("NativeAdrenoCp: CP_ME_INIT idle wait begin\n");
    if(!status && ring_.waitBootstrap(bootstrapWptr,&clock_,2000000,&observedRptr)!=NativeAdrenoRingFenceComplete) {
        status=kIOReturnTimeout;
    }
    cpTrace_=true;
    IOLog("NativeAdrenoCp: CP_ME_INIT idle wait completed status=%08x rptr=%u/%u\n",status,observedRptr,bootstrapWptr);
    if(status) {
        return status;
    }

    IOLog("NativeAdrenoA6xx: CP init completed, authenticating PAS 13 shader\n");
    status=zap_.load(resources_.zap,reinterpret_cast<void *>(zapMap_->getVirtualAddress()));
    IOLog("NativeAdrenoCp: PAS shader startup completed status=%08x phase=%u SCM=%lld/%lld\n",
        status,zap_.phase(),zap_.transport(),zap_.result());
    native_adreno_a6xx_stats[16]=zap_.phase();
    native_adreno_a6xx_stats[17]=uint32_t(zap_.transport());
    native_adreno_a6xx_stats[18]=uint32_t(zap_.result());
    if(status) {
        return status;
    }

    uint32_t secureMode[2]={0};
    if(AdrenoPM4Type7(0x66,1,&secureMode[0])!=AdrenoPM4Success) {
        return kIOReturnBadArgument;
    }

    IOLog("NativeAdrenoCp: CP_SET_SECURE_MODE submit begin\n");
    status=ring_.submitBootstrap(secureMode,2,&bootstrapWptr);
    cpTrace_=false;
    if(!status && ring_.waitBootstrap(bootstrapWptr,&clock_,2000000,&observedRptr)!=NativeAdrenoRingFenceComplete) {
        status=kIOReturnTimeout;
    }
    cpTrace_=true;
    IOLog("NativeAdrenoCp: secure mode exit completed status=%08x rptr=%u/%u\n",status,observedRptr,bootstrapWptr);
    if(status) {
        return status;
    }

    uint32_t words[4]={0};
    if(AdrenoPM4Type7(0x3d,3,&words[0])!=AdrenoPM4Success) {
        return kIOReturnBadArgument;
    }

    words[1]=uint32_t(GpuScratchIova);
    words[2]=uint32_t(GpuScratchIova>>32);
    words[3]=0x6421cafe;
    uint32_t fence=0;
    IOLog("NativeAdrenoCp: scratch write submit begin\n");
    status=ring_.submit(words,4,&fence);
    IOLog("NativeAdrenoCp: scratch submit completed status=%08x fence=%u\n",status,fence);
    cpTrace_=false;
    if(!status && ring_.wait(fence,&clock_,20000,nullptr)!=NativeAdrenoRingFenceComplete) {
        status=kIOReturnTimeout;
    }
    cpTrace_=true;
    IOLog("NativeAdrenoCp: scratch fence wait completed status=%08x observed=%u\n",status,ring_.completedFence());
    native_adreno_a6xx_stats[8]=fence;
    native_adreno_a6xx_stats[9]=ring_.completedFence();
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

    status=submit_.attach(gpuContext_.vm(),&ring_);
    if(status) {
        return status;
    }
    NativeAdrenoTimeIo time{this,gpuRead};
    status=timer_.attach(&time,NativeAdrenoTimeCp);
    if(status) {
        return status;
    }
    info_ = {};
    info_.chip_id = resources_.chipId;
    info_.gmem_bytes = resources_.gmemBytes;
    info_.gmem_base = 0;
    info_.uche_trap_base = 0x1fffffffff000;
    info_.va_start = 0x20000000;
    info_.va_size = 0x60000000;
    info_.highest_bank_bit = resources_.highestBankBit;
    info_.ubwc_swizzle = 6;
    info_.macrotile_mode = 0;
    info_.ubwc_version = 3;
    info_.flags = NativeAdrenoDeviceClientIova|NativeAdrenoDeviceCachedCoherent|NativeAdrenoDeviceCachedCoherentImports;

    return native_adreno_publish(gpuContext_.vm(),&submit_,&ring_,&clock_,&timer_,&info_,32,40);
}

IOReturn A642LState::fail(IOReturn status)
{
    NativeAdrenoGpuProfile::fault();
    if(hardwareIrqEnabled_) {
        native_adreno_smmu_disable_irq(resources_.gpuInterrupt);
        hardwareIrqEnabled_=false;
    }
    smmuProvider_.observe();
    smmuProvider_.disableInterrupts();
    native_adreno_a6xx_stats[1]=uint32_t(status);
    native_adreno_control_update(native_adreno_a6xx_stats[0],uint32_t(status),"failed");
    failedPowerReport_=power_.report();
    failedGmuReport_=gmu_.report();
    failureCaptured_=true;
    native_adreno_fault();
    bool stopped=true;
    if(cpTouched_) {
        stopped=NativeAdrenoA6xxHwInit::stop(&ringIo_,1000)==kIOReturnSuccess;
    }
    if (stopped) {
        const IOReturn releaseStatus = gmu_.releasePerformanceCounters();
        if (releaseStatus) {
            IOLog("NativeAdrenoGpuProfile: kind=error phase=fault_release status=%08x\n",
                uint32_t(releaseStatus));
        }
    }
    const IOReturn gmuStopped=gmu_.stop();
    if (!gmuStopped) {
        NativeAdrenoGpuProfile::hardwareStopped();
    }
    native_adreno_a6xx_stats[20]=stopped;
    native_adreno_a6xx_stats[21]=uint32_t(gmuStopped);
    if(stopped && !gmuStopped) {
        native_adreno_dma_stopped();
        power_.stop(true);
    }
    dump();
    return status;
}

IOReturn A642LState::start()
{
    uint32_t fixedMhz = 0;
    PE_parse_boot_argn("native-adreno-fixed-mhz", &fixedMhz, sizeof(fixedMhz));
    if (fixedMhz) {
        fixedPerformanceLevel_ = performanceLevelFor(uint64_t(fixedMhz) * 1000000);
        if (NativeAdrenoA6xxOperatingPoints[fixedPerformanceLevel_ - 1].frequencyHz !=
            uint64_t(fixedMhz) * 1000000) {
            return fail(kIOReturnBadArgument);
        }
    }

    native_adreno_a6xx_stats[0]=1;
    native_adreno_control_update(1,0,"starting");
    IOLog("NativeAdrenoA6xx: mapping SM7325 board resources\n");
    IOReturn status=mapWindows();
    if(status) {
        return fail(status);
    }
    native_adreno_a6xx_stats[0]=2;
    native_adreno_control_update(2,0,"starting");
    IOLog("NativeAdrenoA6xx: enabling CX clocks and parent votes\n");
    const NativeAdrenoA6xxIo io{this,read,write,delay};
    status=power_.enableCx(io, resources_.ddrType);
    if(status) {
        return fail(status);
    }
    native_adreno_a6xx_stats[0]=3;
    native_adreno_control_update(3,0,"starting");
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
    native_adreno_control_update(4,0,"starting");
    IOLog("NativeAdrenoA6xx: loading A660 GMU and starting HFI\n");
    status=gmu_.start(config);
    if(!status) {
        status=smmuProvider_.observe();
    }
    if(status) {
        return fail(status);
    }
    native_adreno_a6xx_stats[0]=5;
    native_adreno_control_update(5,0,"starting");
    IOLog("NativeAdrenoA6xx: requesting GX at 315 MHz\n");
    status=power_.enableGx();
    if(!status) {
        status=gmu_.keepAlive(true);
    }
    if(status) {
        return fail(status);
    }
    native_adreno_a6xx_stats[0]=6;
    native_adreno_control_update(6,0,"starting");
    IOLog("NativeAdrenoA6xx: starting A642L CP and GPU memory write\n");
    status=startCp();
    cpTrace_=false;
    if(status) {
        return fail(status);
    }
    status=native_adreno_smmu_register_irq(resources_.gpuInterrupt,gpuInterrupt,this);
    if(!status) {
        hardwareIrqEnabled_=true;
        status=native_adreno_smmu_enable_irq(resources_.gpuInterrupt);
    }
    if(status) {
        return fail(status);
    }
    lastActiveUs_ = now(nullptr);
    __atomic_store_n(&activityKick_, 0, __ATOMIC_RELEASE);
    status = pollPerformance(false);
    if (status) {
        return fail(status);
    }
    IOLog("NativeAdrenoDvfs: ready ddr=%u levels=315,450,550 interval_us=16000 fixed_mhz=%u ack_mhz=%u cap_mhz=%u\n",
        resources_.ddrType, fixedMhz, performance_.acknowledged_hz / 1000000,
        performance_.thermal_cap_hz / 1000000);

    ready_=true;
    native_adreno_a6xx_stats[0]=7;
    native_adreno_control_update(7,0,"ready");
    dump();
    return kIOReturnSuccess;
}

void A642LState::poll()
{
    if(!ready_) {
        return;
    }

    const uint64_t current = now(nullptr);
    if (current >= nextHealthPollUs_) {
        nextHealthPollUs_ = current + 10000;
        IOReturn gmuStatus = smmuProvider_.observe();
        if (!gmuStatus) {
            gmuStatus = gmu_.poll();
        }
        if (gmuStatus) {
            ready_ = false;
            native_adreno_a6xx_stats[0] = 8;
            fail(gmuStatus);
            return;
        }
    }

    const IOReturn status=__atomic_load_n(&hardwareIrqFailed_,__ATOMIC_ACQUIRE)?
        kIOReturnIOError:kIOReturnSuccess;
    uint32_t submitted=0,completed=0;
    const int progress=native_adreno_progress(&submitted,&completed);
    if(completed!=watchedFence_ || submitted==completed) {
        watchedFence_=completed;
        watchedSince_=current;
    }
    const bool timeout=submitted!=completed && current-watchedSince_>500000;
    if(status || progress || timeout) {
        ready_=false;
        native_adreno_a6xx_stats[0]=8;
        fail(status?status:(timeout?kIOReturnTimeout:kIOReturnIOError));
        return;
    }

    uint32_t captureHz = 0;
    bool captureEnabled = NativeAdrenoGpuProfile::captureFrequency(&captureHz);
    if (current >= nextPerformancePollUs_ || activityPending() ||
        captureEnabled != capturePerformanceEnabled_ || captureHz != capturePerformanceHz_) {
        const IOReturn performanceStatus = pollPerformance(submitted != completed);
        if (performanceStatus) {
            ready_ = false;
            native_adreno_a6xx_stats[0] = 8;
            fail(performanceStatus);
            return;
        }
    }

    const IOReturn profileStatus = NativeAdrenoGpuProfile::poll();
    if (profileStatus) {
        ready_ = false;
        native_adreno_a6xx_stats[0] = 8;
        fail(profileStatus);
        return;
    }

    // The last retired capture job releases its fixed GPU/DDR selection here.
    captureEnabled = NativeAdrenoGpuProfile::captureFrequency(&captureHz);
    if (captureEnabled != capturePerformanceEnabled_ || captureHz != capturePerformanceHz_) {
        const IOReturn performanceStatus = pollPerformance(submitted != completed);
        if (performanceStatus) {
            ready_ = false;
            native_adreno_a6xx_stats[0] = 8;
            fail(performanceStatus);
        }
    }
}

uint32_t A642LState::waitMilliseconds() const
{
    if (!ready_) {
        return 10;
    }

    const uint64_t current = now(nullptr);
    const uint64_t deadline = nextHealthPollUs_ < nextPerformancePollUs_ ?
        nextHealthPollUs_ : nextPerformancePollUs_;
    if (deadline <= current) {
        return 1;
    }

    return uint32_t((deadline - current + 999) / 1000);
}

bool A642LState::activityPending() const
{
    return ready_ && __atomic_load_n(&activityKick_, __ATOMIC_ACQUIRE) != 0;
}

void A642LState::dump() const
{
    const auto &power=failureCaptured_?failedPowerReport_:power_.report();
    const auto &gmu=failureCaptured_?failedGmuReport_:gmu_.report();
    IOLog("NativeAdrenoA6xx: step=%u status=%08x cp_fence=%u/%u readback=%08x irq=%08x cp=%08x protect=%08x\n",
        native_adreno_a6xx_stats[0],native_adreno_a6xx_stats[1],native_adreno_a6xx_stats[8],
        native_adreno_a6xx_stats[9],native_adreno_a6xx_stats[10],native_adreno_a6xx_stats[11],
        native_adreno_a6xx_stats[12],native_adreno_a6xx_stats[13]);
    IOLog("NativeAdrenoA6xx: power=%u/%08x domain=%u offset=%x value=%x expected=%x mask=%x\n",
        power.phase,power.status,power.domain,power.offset,power.value,power.expected,power.mask);
    IOLog("NativeAdrenoA6xx: gmu=%u/%08x domain=%u offset=%x value=%x hfi=%x/%x ack=%u zap=%u scm=%lld/%lld\n",
        gmu.phase,gmu.status,gmu.domain,gmu.offset,gmu.value,gmu.hfiMessage,gmu.hfiError,
        gmu.acknowledged,zap_.phase(),zap_.transport(),zap_.result());
    IOLog("NativeAdrenoDvfs: ack_hz=%u requested_hz=%u ddr_index=%u cap_hz=%u busy_permille=%u temperature_millic=%d thermal_valid=%x thermal_flags=%x flags=%x vote_status=%08x\n",
        performance_.acknowledged_hz, performance_.requested_hz, performance_.bandwidth_index,
        performance_.thermal_cap_hz, performance_.busy_permille, performance_.temperature_millic,
        performance_.thermal_valid_mask, performance_.thermal_flags, performance_.flags,
        performance_.vote_status);
    if(gmuContextReport_.step) {
        NativeAdrenoLogSmmuReport("gmu-retained",&gmuContextReport_);
    }
    if(gpuContextReport_.step) {
        NativeAdrenoLogSmmuReport("gpu-retained",&gpuContextReport_);
    }
}

uint64_t vmNow()
{
    uint64_t value=0;
    NativeAdrenoKernelTime::now_us(nullptr,&value);
    return value;
}

void registrationWorker(void *,wait_result_t)
{
    thread_set_thread_name(current_thread(),"adreno-control");
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
    const IOReturn published=native_adreno_control_publish();
    if(published) {
        IOLog("NativeAdrenoA6xx: control service registration failed %08x\n",published);
        native_adreno_a6xx_stats[1]=uint32_t(published);
        return;
    }
    IOLog("NativeAdrenoA6xx: control ready, waiting for administrator Start request\n");
}

void hardwareWorker(void *,wait_result_t)
{
    thread_set_thread_name(current_thread(),"adreno-a642l");
    IOLog("NativeAdrenoA6xx: hardware worker started by administrator request\n");
    native_adreno_vm_now_us=vmNow;
    native_adreno_pool_mib=0;
    state=new A642LState;
    if(!state) {
        native_adreno_a6xx_stats[1]=uint32_t(kIOReturnNoMemory);
        native_adreno_control_update(0,uint32_t(kIOReturnNoMemory),"failed");
        return;
    }
    if(state->start()!=kIOReturnSuccess) {
        return;
    }
    for(;;) {
        uint64_t deadline = 0;
        clock_interval_to_deadline(state->waitMilliseconds(), NSEC_PER_MSEC, &deadline);
        assert_wait_deadline(&performanceWake, THREAD_UNINT, deadline);
        if (state->activityPending()) {
            thread_wakeup(&performanceWake);
        }
        thread_block(THREAD_CONTINUE_NULL);
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
    const kern_return_t status=kernel_thread_start(registrationWorker,nullptr,&thread);
    if(status==KERN_SUCCESS) {
        thread_deallocate(thread);
    } else {
        native_adreno_a6xx_stats[1]=uint32_t(status);
    }
}

extern "C" IOReturn native_adreno_a6xx_request_start(void)
{
    uint32_t expected=0;
    if(!__atomic_compare_exchange_n(&hardwareLaunched,&expected,1,false,__ATOMIC_ACQ_REL,__ATOMIC_ACQUIRE)) {
        return kIOReturnBusy;
    }
    IOLog("NativeAdrenoA6xx: administrator Start accepted\n");
    native_adreno_control_update(0,0,"starting");
    thread_t thread=nullptr;
    const kern_return_t status=kernel_thread_start(hardwareWorker,nullptr,&thread);
    if(status!=KERN_SUCCESS) {
        native_adreno_a6xx_stats[1]=uint32_t(status);
        native_adreno_control_update(0,uint32_t(status),"failed");
        return kIOReturnNoResources;
    }
    thread_deallocate(thread);
    return kIOReturnSuccess;
}

extern "C" void native_adreno_a6xx_dump_stats(void)
{
    IOLog("NativeAdrenoA6xx: registration_requested=%u hardware_requested=%u step=%u status=%08x\n",
        launched,hardwareLaunched,native_adreno_a6xx_stats[0],native_adreno_a6xx_stats[1]);
    if(state) {
        state->dump();
    }
}
