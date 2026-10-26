/* SPDX-License-Identifier: MIT */
#include "NativeAdrenoVm.h"
#include "NativeAdrenoVmAlias.h"
extern "C" volatile uint32_t native_adreno_trace_stage;
extern "C" {int (*native_adreno_dram_read32)(uint64_t,uint32_t *);int (*native_adreno_cached_read32)(uint64_t,uint32_t *);
    int (*native_adreno_line_audit)(uint64_t,size_t,uint32_t *,uint32_t *);}
#include <string.h>
#include <IOKit/IOLib.h>
/* 1 once the board proved the GPU's cacheable inner-shareable accesses coherent with the CPU's caches in both directions (the A740's
 * ADRENO_QUIRK_HAS_CACHED_COHERENT behind the dma-coherent GPU SMMU, which Linux maps with IOMMU_CACHE): a mapping with write-back
 * attributes then needs no CPU cache maintenance. 0 keeps every mapping maintained as before. Counters: maintenance passes skipped for
 * coherent mappings at map, unmap, submission and completion. */
extern "C" {volatile uint32_t native_adreno_io_coherent;volatile uint32_t native_adreno_coherent_skips[4];}
static bool coherentMapping(unsigned writeback) {return writeback && native_adreno_io_coherent;}
/* GPU-written imports (render targets) are mapped non-coherent (the GPU stopped inside its end-of-pass clean writing them through
 * write-back mappings), and the CPU never touches their pixels while the GPU owns them: they were cleaned when mapped and need no
 * maintenance per submission, which cost about 60 ms per frame. */
static bool importedUntouched(const NativeAdrenoMemory *memory) {return memory->imported() && native_adreno_io_coherent;}
static bool overlap(uint64_t a,uint64_t ab,uint64_t b,uint64_t bb)
{return a<b+bb && b<a+ab;}
/* Mappings of one cache/shareability/privilege class may share pages; only pages shared across classes refuse. */
static unsigned aliasClass(unsigned permissions,unsigned writeback,unsigned shareability)
{return writeback|(shareability<<1)|((permissions&ArmSmmu4KPrivileged)?8u:0u);}
static bool validBinding(const NativeAdrenoVmRegion &r,unsigned upper)
{
    return r.tables && r.arena && r.state && !r.tables->quarantined() &&
        r.tables->deviceReferences() && r.tables->spanCount()==1 &&
        r.tables->spans()[0].bytes==r.tables->bytes() && !r.tables->spans()[0].offset &&
        r.arena->mapping==r.tables->mapping() && r.arena->upper_region==upper &&
        r.arena->config.table_physical==r.tables->spans()[0].physical &&
        r.arena->capacity_pages==r.tables->bytes()/4096 && r.state->prepared &&
        !r.state->poisoned && !r.state->tables.leaf_pages;
}
ArmSmmu4KLiveRegion NativeAdrenoVm::region(unsigned upper) const
{
    return {regions_[upper].arena,regions_[upper].state,nullptr,0};
}
/* Board hook and per-phase time of the two table transactions, for the log: [0] map, [1] unmap; each row is count, the phases in order, the slowest call
 * and the bytes mapped (int-134: BoMap averaged 135 ms and the desktop crawled). */
extern "C" {uint64_t (*native_adreno_vm_now_us)(void)=nullptr;volatile uint64_t native_adreno_vm_phase_us[2][8];}
namespace {
struct PhaseClock {
    uint64_t last=native_adreno_vm_now_us?native_adreno_vm_now_us():0,began=last;
    uint64_t phases[8]{};
    void lap(unsigned row,unsigned slot) {
        (void)row;
        if(!native_adreno_vm_now_us)return;
        const uint64_t now=native_adreno_vm_now_us();phases[slot]+=now-last;last=now;
    }
    void done(unsigned row,uint64_t bytes) {
        if(!native_adreno_vm_now_us)return;
        const uint64_t total=native_adreno_vm_now_us()-began;
        for(unsigned slot=1;slot<=5;++slot) {
            native_adreno_vm_phase_us[row][slot]+=phases[slot];
        }
        native_adreno_vm_phase_us[row][0]+=1;native_adreno_vm_phase_us[row][7]+=bytes;
        if(total>native_adreno_vm_phase_us[row][6])native_adreno_vm_phase_us[row][6]=total;
    }
};
}
ArmSmmu4KStatus NativeAdrenoVm::observe() const
{
    ArmSmmu4KStreamResult routes;
    ArmSmmu4KStatus status=ArmSmmu4KObserveStreams(&io_,streams_,context_,&routes);if(status)return status;
    ArmSmmu4KLiveRegion lower=region(0),upper=region(1);ArmSmmu4KLiveResult audit;
    status=ArmSmmu4KLiveAuditContext(context_,lower.arena?&lower:nullptr,upper.arena?&upper:nullptr,&audit);
    if(status)return status;
    return ArmSmmu4KObserveStreams(&io_,streams_,context_,&routes);
}
IOReturn NativeAdrenoVm::fail(ArmSmmu4KStatus status)
{
    poisoned_=true;result_.operation=status;result_.complete=false;
    for(size_t i=0;mappings_ && i<mapping_capacity_;++i)if(mappings_[i].memory)mappings_[i].memory->quarantine();
    for(auto &r:regions_)if(r.arena) {r.tables->quarantine();r.state->poisoned=1;}
    ArmSmmu4KStreamResult routes;
    result_.quarantine=ArmSmmu4KFaultStreams(&io_,streams_,&routes);
    return kIOReturnIOError;
}
IOReturn NativeAdrenoVm::attach(const ArmSmmu4KIo *io,ArmSmmu4KContextConfig *context,
    ArmSmmu4KStreamState *streams,const NativeAdrenoVmRegion *lower,const NativeAdrenoVmRegion *upper,
    size_t max_objects,unsigned budget)
{
    if(attached_ || mappings_ || poisoned_)return kIOReturnBusy;
    if(!io || !context || !streams || (!lower && !upper) || !max_objects || max_objects>NativeAdrenoVmMaxObjects ||
        !budget || budget>1048576 || (lower && !validBinding(*lower,0)) || (upper && !validBinding(*upper,1)))
        return kIOReturnBadArgument;
    mappings_=IONewData(Mapping,max_objects);
    if(!mappings_) {
        return kIOReturnNoMemory;
    }
    mapping_capacity_=max_objects;
    memset(mappings_,0,sizeof(Mapping)*mapping_capacity_);
    for(size_t i=0;i<mapping_capacity_;++i) {
        mappings_[i].next_free=i+1;
    }
    free_head_=0;
    io_=*io;context_=context;streams_=streams;budget_=budget;
    if(lower)regions_[0]=*lower;if(upper)regions_[1]=*upper;
    for(unsigned i=0;i<2;++i)if(regions_[i].arena) {
        if(NativeAdrenoBindSmmuVisibility(regions_[i].tables,&visibility_[i])!=kIOReturnSuccess) {
            IODeleteData(mappings_,Mapping,mapping_capacity_);
            regions_[0]={};regions_[1]={};return kIOReturnBadArgument;
        }
    }
    attached_=true;
    ArmSmmu4KStatus status=observe();if(status)return fail(status);
    return kIOReturnSuccess;
}
IOReturn NativeAdrenoVm::detach()
{
    if(poisoned_ || objects_ || spanCount())return kIOReturnBusy;
    if(!attached_)return kIOReturnNotReady;
    IODeleteData(mappings_,Mapping,mapping_capacity_);
    mapping_capacity_=0;
    attached_=false;regions_[0]={};regions_[1]={};context_=nullptr;streams_=nullptr;
    return kIOReturnSuccess;
}
IOReturn NativeAdrenoVm::findAddress(unsigned upper,size_t bytes,uint64_t alignment,uint64_t *out) const
{
    if(!out)return kIOReturnBadArgument;*out=0;
    if(!attached_ || poisoned_)return kIOReturnNotReady;
    if(upper>1 || !regions_[upper].arena || !bytes || (bytes&4095) || alignment<4096 ||
        (alignment&(alignment-1)))return kIOReturnBadArgument;
    const ArmSmmu4KLiveArena *a=regions_[upper].arena;
    uint64_t offset=0;
    for(const Mapping *record=lowerBound(a->iova);;record=record->next) {
        uint64_t rem=(a->iova+offset)&(alignment-1),padding=rem?alignment-rem:0;
        if(padding>a->bytes-offset)return kIOReturnNoResources;offset+=padding;
        if(bytes>a->bytes-offset)return kIOReturnNoResources;
        if(!record || record->iova-a->iova>=a->bytes ||
            (record->iova-a->iova>=offset && bytes<=record->iova-a->iova-offset))
            {*out=a->iova+offset;return kIOReturnSuccess;}
        uint64_t end=record->iova-a->iova+record->memory->bytes();
        if(end>offset)offset=end;
    }
}
IOReturn NativeAdrenoVm::preflight(const NativeAdrenoMemory *memory,unsigned upper,uint64_t iova,
    unsigned permissions,unsigned writeback,unsigned shareability) const
{
    if(!memory || upper>1 || !regions_[upper].arena || !memory->backed() || memory->quarantined() ||
        !memory->bytes() || !memory->spanCount() || !(permissions&ArmSmmu4KRead) || (permissions&~15u) ||
        writeback>1 || (shareability!=2 && shareability!=3))return kIOReturnBadArgument;
    const ArmSmmu4KLiveArena *a=regions_[upper].arena;
    if((iova&4095) || iova<a->iova || iova-a->iova>=a->bytes || memory->bytes()>a->bytes-(iova-a->iova))
        return kIOReturnBadArgument;
    if(objects_>=mapping_capacity_) {
        return kIOReturnNoResources;
    }
    const Mapping *next=lowerBound(iova);
    const Mapping *previous=next?next->previous:last_;
    if(next && next->iova-iova<memory->bytes()) {
        return kIOReturnBusy;
    }
    if(previous && iova-previous->iova<previous->memory->bytes()) {
        return kIOReturnBusy;
    }
    if(a->config.output_bits<32 || a->config.output_bits>48)return kIOReturnBadArgument;
    uint64_t offset=0,limit=UINT64_C(1)<<a->config.output_bits;
    for(size_t j=0;j<memory->spanCount();++j) {
        const auto &span=memory->spans()[j];
        if(span.offset!=offset || !span.physical || !span.bytes || ((span.physical|span.bytes)&4095) ||
            span.bytes>memory->bytes()-offset || span.physical>=limit || span.bytes>limit-span.physical)
            return kIOReturnBadArgument;
        for(const auto &r:regions_)if(r.arena && overlap(span.physical,span.bytes,
            r.arena->config.table_physical,r.arena->capacity_pages*UINT64_C(4096)))return kIOReturnBadArgument;
        offset+=span.bytes;
    }
    if(offset!=memory->bytes())return kIOReturnBadArgument;
    /* The class counts skip physical comparisons when all BOs use the same
     * attributes. Conflicting objects keep a sorted physical index, so a small
     * new BO queries that index without visiting the object's other pages. */
    const unsigned mine=aliasClass(permissions,writeback,shareability);
    if(class_objects_[mine]==objects_) {
        return kIOReturnSuccess;
    }
    uint32_t *order=a->alias_scratch;
    size_t new_count=0;
    for(const Mapping *record=first_;record;record=record->next) {
        const Mapping &other=*record;
        if(aliasClass(other.permissions,other.writeback,other.shareability)==mine) {
            continue;
        }
        if(memory->spanCount()<=other.spans || !order || memory->spanCount()>a->alias_scratch_entries) {
            NativeAdrenoPhysicalIndex index(other.memory->spans(),other.physical_order,other.physical_count,true);
            for(size_t j=0;j<memory->spanCount();++j) {
                const auto &span=memory->spans()[j];
                if(index.overlaps(span.physical,span.bytes)) {
                    return kIOReturnBadArgument;
                }
            }
        } else {
            if(!new_count) {
                for(size_t j=0;j<memory->spanCount();++j) {
                    order[j]=(uint32_t)j;
                }
                NativeAdrenoPhysicalIndex index(memory->spans(),order,memory->spanCount());
                new_count=index.count();
            }
            NativeAdrenoPhysicalIndex index(memory->spans(),order,new_count,true);
            for(size_t j=0;j<other.spans;++j) {
                const auto &span=other.memory->spans()[j];
                if(index.overlaps(span.physical,span.bytes)) {
                    return kIOReturnBadArgument;
                }
            }
        }
    }
    return kIOReturnSuccess;
}
NativeAdrenoVm::Mapping *NativeAdrenoVm::find(uint64_t handle) const
{
    if(!handle || !mappings_) {
        return nullptr;
    }
    const size_t slot=handle&(NativeAdrenoVmMaxObjects-1);
    if(slot>=mapping_capacity_) {
        return nullptr;
    }
    Mapping &record=mappings_[slot];
    return record.handle==handle && record.memory?&record:nullptr;
}
NativeAdrenoVm::Mapping *NativeAdrenoVm::lowerBound(uint64_t iova) const
{
    Mapping *record=RB_ROOT(&addresses_.root),*next=nullptr;
    while(record) {
        if(record->iova<iova) {
            record=RB_RIGHT(record,address_link);
        } else {
            next=record;
            record=RB_LEFT(record,address_link);
        }
    }
    return next;
}
const NativeAdrenoVm::Mapping *NativeAdrenoVm::findAddressRecord(uint64_t iova) const
{
    const Mapping *next=lowerBound(iova);
    if(next && next->iova==iova) {
        return next;
    }
    const Mapping *previous=next?next->previous:last_;
    if(previous && iova-previous->iova<previous->memory->bytes()) {
        return previous;
    }
    return nullptr;
}
void NativeAdrenoVm::insert(Mapping *record,unsigned upper)
{
    record->next=lowerBound(record->iova);
    record->previous=record->next?record->next->previous:last_;
    if(record->next) {
        record->next->previous=record;
    } else {
        last_=record;
    }
    if(record->previous) {
        record->previous->next=record;
    } else {
        first_=record;
    }
    addresses_.Root_RB_INSERT(&addresses_.root,record);
    ++objects_;
    ++class_objects_[aliasClass(record->permissions,record->writeback,record->shareability)];
    if(upper) {
        upper_count_+=record->spans;
    } else {
        lower_count_+=record->spans;
    }
}
void NativeAdrenoVm::erase(Mapping *record,unsigned upper)
{
    addresses_.Root_RB_REMOVE(&addresses_.root,record);
    if(record->next) {
        record->next->previous=record->previous;
    } else {
        last_=record->previous;
    }
    if(record->previous) {
        record->previous->next=record->next;
    } else {
        first_=record->next;
    }
    --objects_;
    --class_objects_[aliasClass(record->permissions,record->writeback,record->shareability)];
    if(upper) {
        upper_count_-=record->spans;
    } else {
        lower_count_-=record->spans;
    }
}
IOReturn NativeAdrenoVm::map(NativeAdrenoMemory *memory,unsigned upper,uint64_t iova,
    unsigned permissions,unsigned writeback,unsigned shareability,uint64_t *handle)
{
    if(!handle)return kIOReturnBadArgument;*handle=0;result_={};
    if(!attached_ || poisoned_)return kIOReturnNotReady;
    PhaseClock clock;
    IOReturn checked=preflight(memory,upper,iova,permissions,writeback,shareability);if(checked)return checked;
    if(next_handle_>(UINT64_MAX>>12) || free_head_>=mapping_capacity_) {
        return kIOReturnNoResources;
    }
    clock.lap(0,5);

    const size_t spans=memory->spanCount();
    ArmSmmu4KRange *changes=IONewData(ArmSmmu4KRange,spans);
    uint32_t *physical_order=IONewData(uint32_t,spans);
    if(!changes || !physical_order) {
        if(changes) {
            IODeleteData(changes,ArmSmmu4KRange,spans);
        }
        if(physical_order) {
            IODeleteData(physical_order,uint32_t,spans);
        }
        return kIOReturnNoMemory;
    }
    for(size_t i=0;i<spans;++i) {
        const auto &span=memory->spans()[i];
        changes[i]={iova+span.offset,span.physical,span.bytes,permissions,writeback,shareability};
        physical_order[i]=(uint32_t)i;
    }
    NativeAdrenoPhysicalIndex index(memory->spans(),physical_order,spans);
    checked=memory->holdForDevice();
    if(checked) {
        IODeleteData(changes,ArmSmmu4KRange,spans);
        IODeleteData(physical_order,uint32_t,spans);
        return checked;
    }

    Mapping *record=&mappings_[free_head_];
    free_head_=record->next_free;
    *record={};
    record->memory=memory;
    record->ranges=changes;
    record->physical_order=physical_order;
    record->physical_count=index.count();
    record->handle=(next_handle_++<<12)|size_t(record-mappings_);
    record->iova=iova;
    record->spans=spans;
    record->permissions=permissions;
    record->writeback=writeback;
    record->shareability=shareability;
    clock.lap(0,1);
    if(coherentMapping(writeback))native_adreno_coherent_skips[0]=native_adreno_coherent_skips[0]+1;
    else {
        checked=memory->publishForDevice(0,memory->bytes(),permissions&ArmSmmu4KWrite,distrust_write_combined_);
        if(checked)return fail(ArmSmmu4KIoError);
    }
    clock.lap(0,2);
    ArmSmmu4KLiveRegion lower=region(0),high=region(1);ArmSmmu4KLiveResult update;
    arm_smmu_4k_live_now_us=native_adreno_vm_now_us;
    const ArmSmmu4KStatus status=ArmSmmu4KLiveUpdateContextRanges(&io_,streams_->bank,context_,lower.arena?&lower:nullptr,
        high.arena?&high:nullptr,upper,&visibility_[upper],changes,spans,1,budget_,&update);
    if(status)return fail(status);
    clock.lap(0,3);
    insert(record,upper);record->mapped_spans=spans;result_.completed_spans=spans;
    clock.lap(0,4);clock.done(0,memory->bytes());
    *handle=record->handle;result_.complete=true;return kIOReturnSuccess;
}
IOReturn NativeAdrenoVm::unmap(uint64_t handle)
{
    native_adreno_trace_stage=10;
    result_={};if(!attached_ || poisoned_)return kIOReturnNotReady;
    Mapping *record=find(handle);if(!record)return kIOReturnBadArgument;
    if(record->jobs)return kIOReturnBusy;
    PhaseClock clock;
    native_adreno_trace_stage=11;
    unsigned upper=regions_[1].arena && record->iova>=regions_[1].arena->iova;
    if(coherentMapping(record->writeback))native_adreno_coherent_skips[1]=native_adreno_coherent_skips[1]+1;
    else if(record->permissions&ArmSmmu4KWrite) {
        if(record->memory->acquireDeviceWrites(0,record->memory->bytes())!=kIOReturnSuccess)
            return fail(ArmSmmu4KIoError);
    }
    clock.lap(1,2);
    /* The record retains exactly the ranges installed by map(). */
    const size_t spans=record->spans;
    ArmSmmu4KLiveRegion lower=region(0),high=region(1);ArmSmmu4KLiveResult update;
    native_adreno_trace_stage=12;
    arm_smmu_4k_live_now_us=native_adreno_vm_now_us;
    const ArmSmmu4KStatus status=ArmSmmu4KLiveUpdateContextRanges(&io_,streams_->bank,context_,lower.arena?&lower:nullptr,
        high.arena?&high:nullptr,upper,&visibility_[upper],record->ranges,spans,0,budget_,&update);
    native_adreno_trace_stage=13;
    if(status)return fail(status);
    clock.lap(1,3);
    erase(record,upper);record->mapped_spans=0;result_.completed_spans=spans;
    native_adreno_trace_stage=14;
    clock.lap(1,4);
    if(record->memory->retireAfterQuiescence()!=kIOReturnSuccess)return fail(ArmSmmu4KStateMismatch);
    IODeleteData(record->ranges,ArmSmmu4KRange,spans);
    IODeleteData(record->physical_order,uint32_t,spans);
    clock.lap(1,5);clock.done(1,record->memory->bytes());
    *record={};
    record->next_free=free_head_;
    free_head_=size_t(record-mappings_);
    result_.complete=true;return kIOReturnSuccess;
}
IOReturn NativeAdrenoVm::describe(uint64_t handle,uint64_t *iova,size_t *bytes,
    unsigned *permissions) const
{
    if(!iova || !bytes || !permissions)return kIOReturnBadArgument;
    *iova=0;*bytes=0;*permissions=0;
    if(!attached_ || poisoned_)return kIOReturnNotReady;
    if(const Mapping *record=find(handle)) {
        const Mapping &mapping=*record;
        *iova=mapping.iova;*bytes=mapping.memory->bytes();*permissions=mapping.permissions;
        return mapping.mapped_spans==mapping.spans?kIOReturnSuccess:kIOReturnNotReady;
    }
    return kIOReturnBadArgument;
}
/* TLBIALL of the bank before a submission. Where a table update has observed the bank's image (each region's whole-verification
 * update remembers its register offset), the invalidation alone: the image and faults were observed there and every 64th table
 * operation since (int-159: a submission spent 1.3 ms in the kernel, most of it reading SMMU registers). */
IOReturn NativeAdrenoVm::invalidateTlb()
{
    if(!attached_ || poisoned_)return kIOReturnNotReady;
    ArmSmmu4KProgramResult r;
    uint64_t offset=0;
    for(const auto &region:regions_)if(region.state && region.state->context_offset)offset=region.state->context_offset;
    const ArmSmmu4KStatus status=offset?ArmSmmu4KSyncContextTlb(&io_,offset,context_->asid,1,budget_,&r):
        ArmSmmu4KInvalidateAllOwnedContext(&io_,streams_->bank,context_,budget_,&r);
    return status?fail(status):kIOReturnSuccess;
}
IOReturn NativeAdrenoVm::publishMapped()
{
    if(!attached_ || poisoned_)return kIOReturnNotReady;
    for(const Mapping *record=first_;record;record=record->next) {
        const Mapping &m=*record;
        if(coherentMapping(m.writeback) || importedUntouched(m.memory)) {native_adreno_coherent_skips[2]=native_adreno_coherent_skips[2]+1;continue;}
        if(m.memory->publishForDevice(0,m.memory->bytes(),true,distrust_write_combined_))return fail(ArmSmmu4KIoError);
    }
    return kIOReturnSuccess;
}
IOReturn NativeAdrenoVm::acquireImported()
{
    if(!attached_ || poisoned_)return kIOReturnNotReady;
    for(const Mapping *record=first_;record;record=record->next) {
        const Mapping &m=*record;
        if(!m.memory->imported() || !(m.permissions&ArmSmmu4KWrite))continue;
        if(coherentMapping(m.writeback) || importedUntouched(m.memory)) {native_adreno_coherent_skips[3]=native_adreno_coherent_skips[3]+1;continue;}
        if(m.memory->acquireDeviceWrites(0,m.memory->bytes()))return fail(ArmSmmu4KIoError);
    }
    return kIOReturnSuccess;
}
IOReturn NativeAdrenoVm::sampleWord(uint64_t iova,uint32_t *cached,uint32_t *dram) const
{
    if(!cached || !dram)return kIOReturnBadArgument;
    *cached=*dram=~0u;
    if(!attached_ || poisoned_)return kIOReturnNotReady;
    if(iova&3)return kIOReturnBadArgument;
    if(const Mapping *record=findAddressRecord(iova)) {
        const Mapping &m=*record;
        const size_t offset=(size_t)(iova-m.iova);
        if(m.memory->mapping())*cached=static_cast<const volatile uint32_t *>(m.memory->mapping())[offset/4];
        for(size_t j=0;j<m.memory->spanCount();++j) {
            const auto &span=m.memory->spans()[j];
            if(offset<span.offset || offset-span.offset>=span.bytes)continue;
            uint32_t value=0;
            /* DRAM first: the cached reader writes its line back, so read after it DRAM would always agree (int-099). */
            if(native_adreno_dram_read32 && !native_adreno_dram_read32(span.physical+(offset-span.offset),&value))*dram=value;
            /* A write-combined BO's own mapping is non-cacheable: read the cacheable alias instead, so a dirty line is still seen. */
            if(native_adreno_cached_read32 && !native_adreno_cached_read32(span.physical+(offset-span.offset),&value))*cached=value;
            break;
        }
        return kIOReturnSuccess;
    }
    return kIOReturnBadArgument;
}
IOReturn NativeAdrenoVm::reassertWriteCombined(uint32_t *reset) const
{
    if(!reset)return kIOReturnBadArgument;
    if(!attached_ || poisoned_)return kIOReturnNotReady;
    for(const Mapping *record=first_;record;record=record->next) {
        const Mapping &m=*record;
        const IOReturn status=m.memory->reassertWriteCombined(reset);
        if(status)return status;
    }
    return kIOReturnSuccess;
}
IOReturn NativeAdrenoVm::auditRange(uint64_t iova,size_t bytes,uint32_t *lines,uint32_t *dirty,uint32_t *stale) const
{
    if(!lines || !dirty || !stale)return kIOReturnBadArgument;
    if(!attached_ || poisoned_)return kIOReturnNotReady;
    if(!native_adreno_line_audit)return kIOReturnUnsupported;
    if(!bytes || iova>UINT64_MAX-bytes)return kIOReturnBadArgument;
    if(const Mapping *record=findAddressRecord(iova)) {
        const Mapping &m=*record;
        if(!m.memory->writeCombined())return kIOReturnSuccess;   /* maintained before every submission anyway */
        /* Whole lines; the range is clipped to its BO. */
        size_t at=(size_t)(iova-m.iova)&~(size_t)63;
        const size_t end=(size_t)(iova-m.iova)+bytes>m.memory->bytes()?m.memory->bytes():(size_t)(iova-m.iova)+bytes;
        while(at<end) {
            const NativeAdrenoPhysicalSpan *span=nullptr;
            for(size_t j=0;j<m.memory->spanCount();++j) {
                const auto &s=m.memory->spans()[j];
                if(at>=s.offset && at-s.offset<s.bytes) {span=&s;break;}
            }
            if(!span)return kIOReturnBadArgument;
            const uint64_t physical=span->physical+(at-span->offset);
            size_t chunk=4096-(size_t)(physical&4095);
            if(chunk>span->offset+span->bytes-at)chunk=span->offset+span->bytes-at;
            if(chunk>end-at)chunk=(end-at+63)&~(size_t)63;
            if(native_adreno_line_audit(physical,chunk,dirty,stale))return kIOReturnIOError;
            *lines+=(uint32_t)(chunk/64);at+=chunk;
        }
        return kIOReturnSuccess;
    }
    return kIOReturnBadArgument;
}
IOReturn NativeAdrenoVm::peekWords(uint64_t iova,uint32_t *out,size_t count) const
{
    if(!out || !attached_ || poisoned_)return kIOReturnBadArgument;
    for(size_t k=0;k<count;++k)out[k]=0;
    if(const Mapping *record=findAddressRecord(iova)) {
        const Mapping &m=*record;
        if(!m.memory->mapping()) {
            return kIOReturnBadArgument;
        }
        const size_t offset=(size_t)(iova-m.iova);
        for(size_t k=0;k<count && offset+4*k+4<=m.memory->bytes();++k)out[k]=static_cast<const volatile uint32_t *>(m.memory->mapping())[offset/4+k];
        return kIOReturnSuccess;
    }
    return kIOReturnBadArgument;
}
IOReturn NativeAdrenoVm::holdJob(uint64_t handle)
{
    if(!attached_ || poisoned_)return kIOReturnNotReady;
    Mapping *record=find(handle);if(!record)return kIOReturnBadArgument;
    if(record->jobs==UINT32_MAX)return kIOReturnNoResources;
    IOReturn status=record->memory->holdForDevice();if(status)return status;
    ++record->jobs;return kIOReturnSuccess;
}
IOReturn NativeAdrenoVm::retireJobAfterCompletion(uint64_t handle)
{
    if(!attached_ || poisoned_)return kIOReturnNotReady;
    Mapping *record=find(handle);if(!record || !record->jobs)return kIOReturnBadArgument;
    IOReturn status=record->memory->retireAfterQuiescence();if(status)return status;
    --record->jobs;return kIOReturnSuccess;
}
