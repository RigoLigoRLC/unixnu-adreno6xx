/* SPDX-License-Identifier: MIT */
#include "NativeAdrenoSubmissionProfile.h"
#include "NativeAdrenoRing.h"
#include "NativeAdrenoVm.h"
#include "NativeAdrenoKernelTime.h"
#include "AdrenoPM4.h"
#include <IOKit/IOLib.h>
#include <IOKit/IOLocks.h>
#include <pexpert/pexpert.h>

namespace {
constexpr uint64_t ProfileIova = 0x304000;
constexpr size_t ProfileBytes = NativeAdrenoProfileSlots * NativeAdrenoProfileSlotBytes;
constexpr size_t ProfilePageBytes = 4096;
constexpr uint32_t SnapshotBytes = NativeAdrenoProfileSlotBytes / 2;
constexpr uint32_t AlwaysOnRegister = 0x980;
constexpr uint64_t TimestampMask = (UINT64_C(1) << 48) - 1;
constexpr unsigned ValueCount = NativeAdrenoProfileCounterCount + 1;
constexpr unsigned SelectorWord = ValueCount * 2;
constexpr unsigned MarkerWord = SelectorWord + NativeAdrenoProfileCounterCount;
constexpr unsigned RecordCapacity = 512;

/* Counter describes a fixed A6xx selector and its adjacent 64-bit value pair. */
struct Counter {
    uint32_t selector;
    uint32_t event;
    uint32_t low;
};

// SM7325 a6xx_reg.h supplies addresses; Mesa a6xx_perfcntrs.xml supplies events.
constexpr Counter counters[NativeAdrenoProfileCounterCount] = {
    {0x50a, 0, 0x422},
    {0xae24, 0, 0x4ce},
    {0xae25, 1, 0x4d0},
    {0xae26, 35, 0x4d2},
    {0xae27, 40, 0x4d4},
    {0xb61b, 0, 0x4a4},
    {0xe27, 0, 0x48c},
    {0xae22, 4, 0x4ca},
    {0xae23, 5, 0x4cc},
    {0xe24, 8, 0x486},
    {0xe25, 4, 0x488},
    {0xe26, 15, 0x48a}
};

struct Record {
    NativeAdrenoProfileCpuTiming cpu{};
    uint64_t sequence = 0;
    uint64_t enqueuedUs = 0;
    uint64_t retiredUs = 0;
    uint64_t gpuTicks = 0;
    uint64_t queueTicks = 0;
    uint64_t deltas[NativeAdrenoProfileCounterCount]{};
    uint32_t fence = 0;
    uint32_t counterMask = 0;
    uint32_t valid = 0;
    uint32_t queueValid = 0;
    bool sampled = false;
};

/* Duration accumulates completed submissions' host or device elapsed time. */
struct Duration {
    uint64_t total = 0;
    uint64_t maximum = 0;

    void add(uint64_t value)
    {
        total+=value;
        if(value>maximum) {
            maximum=value;
        }
    }
};

/* Aggregate contains cumulative CPU timings for all completed submissions and
 * GPU measurements only for submissions selected by the sampling interval. */
struct Aggregate {
    Duration cpu;
    Duration validation;
    Duration cache;
    Duration publish;
    Duration tlb;
    Duration ring;
    Duration ibs;
    Duration gpu;
    Duration queue;
    uint64_t records = 0;
    uint64_t sampled = 0;
    uint64_t valid = 0;
    uint64_t queueCount = 0;
    uint64_t invalid = 0;
    uint64_t queueInvalid = 0;
    uint64_t counterTotals[NativeAdrenoProfileCounterCount]{};
    uint64_t counterSamples[NativeAdrenoProfileCounterCount]{};
    uint32_t counterValidMask = 0;
    uint32_t populatedMask = 0;
    uint32_t depthMaximum = 0;
    uint32_t lastFence = 0;
};

IOLock *recordLock;
Record records[RecordCapacity];
uint64_t produced;
uint64_t consumed;
uint64_t lost;
uint64_t snapshotSerial;
Aggregate aggregate;
uint32_t sampleEvery = 16;
bool detailEnabled;

uint64_t readValue(const volatile uint32_t *words,unsigned index)
{
    return uint64_t(words[index * 2 + 1]) << 32 | words[index * 2];
}

bool readAlwaysOn(const NativeAdrenoRingIo *io,uint64_t *value)
{
    for(unsigned attempt=0;attempt<4;++attempt) {
        uint32_t high=0,low=0,after=0;
        if(io->read32(io->cookie,AlwaysOnRegister+1,&high) ||
            io->read32(io->cookie,AlwaysOnRegister,&low) ||
            io->read32(io->cookie,AlwaysOnRegister+1,&after)) {
            return false;
        }
        if(high==after) {
            *value=(uint64_t(high)<<32|low)&TimestampMask;
            return true;
        }
    }
    return false;
}

size_t emitPacket(uint32_t *words,size_t at,uint32_t opcode,
    const uint32_t *payload,uint32_t count)
{
    if(AdrenoPM4Type7(opcode,count,&words[at])!=AdrenoPM4Success) {
        return 0;
    }
    for(unsigned index=0;index<count;++index) {
        words[at+1+index]=payload[index];
    }
    return at+count+1;
}

size_t emitRegister(uint32_t *words,size_t at,uint32_t reg,uint64_t address,bool wide)
{
    // CP_REG_TO_MEM uses CNT=2 and bit 30 for the atomic 64-bit register read.
    const uint32_t payload[]={reg|(wide?((2u<<18)|(1u<<30)):0),
        uint32_t(address),uint32_t(address>>32)};
    return emitPacket(words,at,0x3e,payload,3);
}

void append(Record &record)
{
    IOLockLock(recordLock);
    record.sequence=++produced;
    if(produced-consumed>RecordCapacity) {
        ++lost;
        ++consumed;
    }
    ++aggregate.records;
    aggregate.lastFence=record.fence;
    aggregate.cpu.add(record.cpu.cpuUs);
    aggregate.validation.add(record.cpu.validationUs);
    aggregate.cache.add(record.cpu.cacheUs);
    aggregate.publish.add(record.cpu.publishUs);
    aggregate.tlb.add(record.cpu.tlbUs);
    aggregate.ring.add(record.cpu.ringUs);
    aggregate.ibs.add(record.cpu.ibs);
    if(record.cpu.depth>aggregate.depthMaximum) {
        aggregate.depthMaximum=record.cpu.depth;
    }
    if(record.sampled) {
        ++aggregate.sampled;
        if(!record.valid) {
            ++aggregate.invalid;
        }
        if(!record.queueValid) {
            ++aggregate.queueInvalid;
        }
    }
    if(record.valid) {
        ++aggregate.valid;
        aggregate.gpu.add(record.gpuTicks);
    }
    if(record.queueValid) {
        ++aggregate.queueCount;
        aggregate.queue.add(record.queueTicks);
    }
    for(unsigned index=0;index<NativeAdrenoProfileCounterCount;++index) {
        if(record.counterMask&(1u<<index)) {
            aggregate.counterValidMask|=1u<<index;
            ++aggregate.counterSamples[index];
            aggregate.counterTotals[index]+=record.deltas[index];
            if(record.deltas[index]) {
                aggregate.populatedMask|=1u<<index;
            }
        }
    }
    records[(record.sequence-1)%RecordCapacity]=record;
    IOLockUnlock(recordLock);
}
}

uint64_t NativeAdrenoProfileNowUs()
{
    uint64_t value=0;
    NativeAdrenoKernelTime::now_us(nullptr,&value);
    return value;
}

IOReturn NativeAdrenoSubmissionProfile::attach(NativeAdrenoVm *vm,const NativeAdrenoRingIo *io)
{
    if(enabled_ || memory_.backed()) {
        return kIOReturnBusy;
    }
    if(!vm || !io || !io->read32 || !io->write32 || !io->barrier) {
        return kIOReturnBadArgument;
    }
    IOLock *lock=IOLockAlloc();
    if(!lock) {
        return kIOReturnNoMemory;
    }
    __atomic_store_n(&recordLock,lock,__ATOMIC_RELEASE);

    uint32_t interval=16;
    uint32_t detail=0;
    PE_parse_boot_argn("native-adreno-profile-every",&interval,sizeof(interval));
    PE_parse_boot_argn("native-adreno-profile-detail",&detail,sizeof(detail));
    sampleEvery_=interval && interval<=65536?interval:16;
    sampleEvery=sampleEvery_;
    detailEnabled=detail!=0;

    IOReturn status=memory_.allocate(ProfileBytes,40,true,ProfileBytes);
    if(!status) {
        status=memory_.makeWriteCombined();
    }
    if(!status) {
        status=vm->map(&memory_,0,ProfileIova,
            ArmSmmu4KRead|ArmSmmu4KWrite|ArmSmmu4KPrivileged,0,2,&handle_);
    }
    if(status) {
        return status;
    }

    // The service calls attach before SQE starts, with GX held powered.
    for(unsigned index=7;index<NativeAdrenoProfileCounterCount;++index) {
        if(io->write32(io->cookie,counters[index].selector,counters[index].event)) {
            return kIOReturnIOError;
        }
    }
    if(io->barrier(io->cookie)) {
        return kIOReturnIOError;
    }
    enabled_=true;
    IOLog("NativeAdrenoProfile: ready=1 slots=%u counters=%u clock_hz=19200000 mode=sampled sample_every=%u detail=%u\n",
        NativeAdrenoProfileSlots,NativeAdrenoProfileCounterCount,sampleEvery_,detailEnabled?1u:0u);
    return kIOReturnSuccess;
}

IOReturn NativeAdrenoSubmissionProfile::prepare(uint32_t fence,const NativeAdrenoProfileCpuTiming &cpu,
    bool sampled)
{
    if(!enabled_ || !fence) {
        return kIOReturnNotReady;
    }
    Slot &slot=slots_[fence%NativeAdrenoProfileSlots];
    if(slot.active) {
        return kIOReturnBusy;
    }
    if(!sampled) {
        slot={};
        slot.cpu=cpu;
        slot.fence=fence;
        slot.active=true;
        return kIOReturnSuccess;
    }
    const size_t offset=(fence%NativeAdrenoProfileSlots)*NativeAdrenoProfileSlotBytes;
    volatile uint32_t *words=static_cast<volatile uint32_t *>(memory_.mapping())+offset/4;
    for(unsigned index=0;index<NativeAdrenoProfileSlotBytes/4;++index) {
        words[index]=0;
    }
    // Memory accepts whole pages; this Normal NC allocation only needs its store barrier.
    const size_t pageOffset=offset&~(ProfilePageBytes-1);
    const IOReturn status=memory_.publishForDevice(pageOffset,ProfilePageBytes,true);
    if(status) {
        return status;
    }
    __asm__ volatile("dsb sy" ::: "memory");
    slot={};
    slot.cpu=cpu;
    slot.fence=fence;
    slot.active=true;
    slot.sampled=true;
    return kIOReturnSuccess;
}

void NativeAdrenoSubmissionProfile::cancel(uint32_t fence)
{
    Slot &slot=slots_[fence%NativeAdrenoProfileSlots];
    if(slot.active && slot.fence==fence) {
        slot={};
    }
}

IOReturn NativeAdrenoSubmissionProfile::emit(uint32_t fence,bool end,uint32_t *words,size_t capacity)
{
    if(!enabled_ || !words || capacity<NativeAdrenoProfileSnapshotWords ||
        slots_[fence%NativeAdrenoProfileSlots].fence!=fence ||
        !slots_[fence%NativeAdrenoProfileSlots].sampled) {
        return kIOReturnBadArgument;
    }
    const uint64_t base=ProfileIova+(fence%NativeAdrenoProfileSlots)*NativeAdrenoProfileSlotBytes+
        (end?SnapshotBytes:0);
    // A6xx WFI plus REG_TO_MEM waits for prior shader and memory work to finish.
    size_t count=emitPacket(words,0,0x26,nullptr,0);
    if(count) {
        count=emitRegister(words,count,AlwaysOnRegister,base,true);
    }
    for(unsigned index=0;index<NativeAdrenoProfileCounterCount && count;++index) {
        count=emitRegister(words,count,counters[index].low,base+(index+1)*8,true);
    }
    for(unsigned index=0;index<NativeAdrenoProfileCounterCount && count;++index) {
        count=emitRegister(words,count,counters[index].selector,base+(SelectorWord+index)*4,false);
    }
    if(count) {
        count=emitPacket(words,count,0x12,nullptr,0);
    }
    const uint64_t marker=base+MarkerWord*4;
    const uint32_t payload[]={uint32_t(marker),uint32_t(marker>>32),fence};
    if(count) {
        count=emitPacket(words,count,0x3d,payload,3);
    }
    if(count) {
        count=emitPacket(words,count,0x12,nullptr,0);
    }
    return count==NativeAdrenoProfileSnapshotWords?kIOReturnSuccess:kIOReturnIOError;
}

void NativeAdrenoSubmissionProfile::markEnqueued(uint32_t fence,const NativeAdrenoRingIo *io)
{
    if(!enabled_ || !fence) {
        return;
    }
    Slot &slot=slots_[fence%NativeAdrenoProfileSlots];
    if(slot.active && slot.fence==fence) {
        if(slot.sampled) {
            slot.anchorValid=readAlwaysOn(io,&slot.enqueuedTicks);
        }
        slot.enqueuedUs=NativeAdrenoProfileNowUs();
    }
}

void NativeAdrenoSubmissionProfile::setCpuTiming(uint32_t fence,const NativeAdrenoProfileCpuTiming &cpu)
{
    if(!enabled_ || !fence) {
        return;
    }
    Slot &slot=slots_[fence%NativeAdrenoProfileSlots];
    if(slot.active && slot.fence==fence) {
        slot.cpu=cpu;
        ++submissions_;
    }
}

void NativeAdrenoSubmissionProfile::collect(uint32_t fence)
{
    if(!enabled_ || !fence) {
        return;
    }
    Slot &slot=slots_[fence%NativeAdrenoProfileSlots];
    if(!slot.active || slot.fence!=fence) {
        return;
    }
    Record record{};
    record.cpu=slot.cpu;
    record.fence=fence;
    record.enqueuedUs=slot.enqueuedUs;
    record.retiredUs=NativeAdrenoProfileNowUs();
    record.sampled=slot.sampled;
    if(!record.sampled) {
        append(record);
        slot={};
        return;
    }
    const size_t offset=(fence%NativeAdrenoProfileSlots)*NativeAdrenoProfileSlotBytes;
    // Other slots can remain active on this Normal NC page during the load barrier.
    const size_t pageOffset=offset&~(ProfilePageBytes-1);
    const IOReturn acquired=memory_.acquireDeviceWrites(pageOffset,ProfilePageBytes);
    if(acquired) {
        IOLog("NativeAdrenoProfile: kind=error stage=acquire-snapshot fence=%u status=%08x\n",
            fence,acquired);
    }
    __asm__ volatile("dsb sy" ::: "memory");
    const volatile uint32_t *begin=static_cast<volatile uint32_t *>(memory_.mapping())+offset/4;
    const volatile uint32_t *end=begin+SnapshotBytes/4;
    if(!acquired && begin[MarkerWord]==fence && end[MarkerWord]==fence) {
        const uint64_t beginTicks=readValue(begin,0)&TimestampMask;
        const uint64_t endTicks=readValue(end,0)&TimestampMask;
        record.gpuTicks=(endTicks-beginTicks)&TimestampMask;
        record.valid=beginTicks && endTicks && record.gpuTicks<(TimestampMask>>1);
        record.queueTicks=(beginTicks-slot.enqueuedTicks)&TimestampMask;
        record.queueValid=record.valid && slot.anchorValid && record.queueTicks<(TimestampMask>>1);
        for(unsigned index=0;index<NativeAdrenoProfileCounterCount && record.valid;++index) {
            if(begin[SelectorWord+index]!=counters[index].event ||
                end[SelectorWord+index]!=counters[index].event) {
                continue;
            }
            const uint64_t before=readValue(begin,index+1);
            const uint64_t after=readValue(end,index+1);
            if(after>=before) {
                record.deltas[index]=after-before;
                record.counterMask|=1u<<index;
            }
        }
    }
    append(record);
    slot={};
}

extern "C" void native_adreno_profile_dump(void)
{
    IOLock *lock=__atomic_load_n(&recordLock,__ATOMIC_ACQUIRE);
    if(!lock) {
        return;
    }
    IOLockLock(lock);
    const uint64_t snapshot=++snapshotSerial;
    const uint64_t until=produced;
    const uint64_t totalLost=lost;
    const Aggregate totals=aggregate;
    const uint32_t interval=sampleEvery;
    const bool detail=detailEnabled;
    IOLockUnlock(lock);
    IOLog("NativeAdrenoProfile: kind=summary mode=aggregate snapshot=%llu records=%llu sample_every=%u sampled=%llu valid=%llu lost=%llu invalid=%llu queue_invalid=%llu counter_valid_mask=%x populated_mask=%x last_seq=%llu last_fence=%u\n",
        snapshot,totals.records,interval,totals.sampled,totals.valid,totalLost,totals.invalid,
        totals.queueInvalid,totals.counterValidMask,totals.populatedMask,until,totals.lastFence);
    IOLog("NativeAdrenoProfile: kind=cpu_totals snapshot=%llu count=%llu cpu_us=%llu validation_us=%llu cache_us=%llu publish_us=%llu tlb_us=%llu ring_us=%llu ibs=%llu depth_max=%u\n",
        snapshot,totals.records,totals.cpu.total,totals.validation.total,totals.cache.total,
        totals.publish.total,totals.tlb.total,totals.ring.total,totals.ibs.total,totals.depthMaximum);
    IOLog("NativeAdrenoProfile: kind=cpu_max snapshot=%llu cpu_us=%llu validation_us=%llu cache_us=%llu publish_us=%llu tlb_us=%llu ring_us=%llu ibs=%llu\n",
        snapshot,totals.cpu.maximum,totals.validation.maximum,totals.cache.maximum,
        totals.publish.maximum,totals.tlb.maximum,totals.ring.maximum,totals.ibs.maximum);
    IOLog("NativeAdrenoProfile: kind=gpu_totals snapshot=%llu count=%llu gpu_ticks=%llu gpu_max_ticks=%llu queue_count=%llu queue_ticks=%llu queue_max_ticks=%llu clock_hz=19200000\n",
        snapshot,totals.valid,totals.gpu.total,totals.gpu.maximum,totals.queueCount,
        totals.queue.total,totals.queue.maximum);
    IOLog("NativeAdrenoProfile: kind=counter_totals snapshot=%llu part=0 core=%llu sp_busy=%llu sp_alu=%llu vs_alu=%llu fs_alu=%llu tp_busy=%llu\n",
        snapshot,totals.counterTotals[0],totals.counterTotals[1],totals.counterTotals[2],
        totals.counterTotals[3],totals.counterTotals[4],totals.counterTotals[5]);
    IOLog("NativeAdrenoProfile: kind=counter_totals snapshot=%llu part=1 uche_busy=%llu sp_stall_tp=%llu sp_stall_uche=%llu read_sp=%llu read_tp=%llu write_sp=%llu\n",
        snapshot,totals.counterTotals[6],totals.counterTotals[7],totals.counterTotals[8],
        totals.counterTotals[9],totals.counterTotals[10],totals.counterTotals[11]);
    IOLog("NativeAdrenoProfile: kind=counter_samples snapshot=%llu part=0 core=%llu sp_busy=%llu sp_alu=%llu vs_alu=%llu fs_alu=%llu tp_busy=%llu\n",
        snapshot,totals.counterSamples[0],totals.counterSamples[1],totals.counterSamples[2],
        totals.counterSamples[3],totals.counterSamples[4],totals.counterSamples[5]);
    IOLog("NativeAdrenoProfile: kind=counter_samples snapshot=%llu part=1 uche_busy=%llu sp_stall_tp=%llu sp_stall_uche=%llu read_sp=%llu read_tp=%llu write_sp=%llu\n",
        snapshot,totals.counterSamples[6],totals.counterSamples[7],totals.counterSamples[8],
        totals.counterSamples[9],totals.counterSamples[10],totals.counterSamples[11]);

    if(!detail) {
        return;
    }

    for(unsigned index=0;index<32;++index) {
        Record record{};
        IOLockLock(lock);
        if(consumed>=until) {
            IOLockUnlock(lock);
            break;
        }
        record=records[consumed%RecordCapacity];
        ++consumed;
        IOLockUnlock(lock);

        const auto &cpu=record.cpu;
        IOLog("NativeAdrenoProfile: kind=cpu seq=%llu fence=%u queue=%u cpu_us=%llu validation_us=%llu cache_us=%llu publish_us=%llu tlb_us=%llu ring_us=%llu depth=%u ibs=%u\n",
            record.sequence,record.fence,cpu.queue,cpu.cpuUs,cpu.validationUs,cpu.cacheUs,
            cpu.publishUs,cpu.tlbUs,cpu.ringUs,cpu.depth,cpu.ibs);
        IOLog("NativeAdrenoProfile: kind=gpu seq=%llu fence=%u sampled=%u valid=%u queue_valid=%u gpu_ticks=%llu gpu_us=%llu queue_ticks=%llu queue_us=%llu counter_mask=%x\n",
            record.sequence,record.fence,record.sampled?1u:0u,record.valid,record.queueValid,record.gpuTicks,
            record.gpuTicks*10/192,record.queueTicks,record.queueTicks*10/192,
            record.counterMask);
        IOLog("NativeAdrenoProfile: kind=gpu seq=%llu fence=%u submit_us=%llu enqueue_us=%llu retire_us=%llu\n",
            record.sequence,record.fence,cpu.submitUs,record.enqueuedUs,record.retiredUs);
        IOLog("NativeAdrenoProfile: kind=counters seq=%llu fence=%u core=%llu sp_busy=%llu sp_alu=%llu vs_alu=%llu fs_alu=%llu tp_busy=%llu\n",
            record.sequence,record.fence,record.deltas[0],record.deltas[1],record.deltas[2],
            record.deltas[3],record.deltas[4],record.deltas[5]);
        IOLog("NativeAdrenoProfile: kind=counters seq=%llu fence=%u uche_busy=%llu sp_stall_tp=%llu sp_stall_uche=%llu read_sp=%llu read_tp=%llu write_sp=%llu\n",
            record.sequence,record.fence,record.deltas[6],record.deltas[7],record.deltas[8],
            record.deltas[9],record.deltas[10],record.deltas[11]);
    }
}
