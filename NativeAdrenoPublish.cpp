/* SPDX-License-Identifier: MIT */
/* IOKit surface for an already-running GPU context. The board code (kits/sheng) brings the GPU up, then hands this file
 * the live VM, submit object, ring, clock and optional timer; one IOService is published and each IOServiceOpen creates a
 * connection that mounts its own NativeAdrenoUserClient core. Up to NativeAdrenoMaxClients connections (one per Metal process) share
 * the VM, the submit object and the ring: each core has its own handles, its own queue range on the ring and its own window of the
 * one GPU address space (the render server the large one), and closing a connection waits for its own jobs only, then unmaps and
 * frees everything it created. Not yet done: per-process GPU page tables (the windows are enforced by the driver, not by the SMMU)
 * and a reset path. This is not the Metal or Turnip ABI; it carries NativeAdrenoABI.h unchanged.
 * BO CPU mappings use physical-range descriptors; Turnip requests write-combined CPU mappings for the BO pages.
 * AGXDeviceUserClient serves the Metal plugin's connections. The accelerator's
 * NativeSandboxGrantProcess property supplies the process authorization used by
 * the kernel's IOServiceOpen path; the client class name alone is insufficient. */
#include <IOKit/IOLib.h>
#include <IOKit/IOMemoryDescriptor.h>
#include <IOKit/IOBufferMemoryDescriptor.h>
#include <IOKit/IOKitKeys.h>
#include <IOKit/IOService.h>
#include <IOKit/IOPlatformExpert.h>
#include <IOKit/IOUserClient.h>
#include <string.h>
#include <kern/thread.h>
#include <mach/kmod.h>
#include <pexpert/pexpert.h>
#include "NativeAdrenoUserClient.h"
#include "NativeAdrenoIntegration.h"
#include "NativeAdrenoA6xxService.h"
#include "NativeAdrenoDisplay.h"

static NativeAdrenoIntegration integration = {};
extern "C" kmod_info_t g_kernel_kmod_info;
extern "C" void native_adreno_publish_static_init(void)
    __asm__("__GLOBAL__sub_I_NativeAdrenoPublish.cpp");
extern "C" IOReturn native_adreno_accelerator_publish(IOService *,uint32_t);

extern "C" void native_adreno_set_integration(const NativeAdrenoIntegration *callbacks)
{
    integration = callbacks ? *callbacks : NativeAdrenoIntegration{};
}

extern "C" void proc_selfname(char *, int);
/* The plugin's telemetry table (rows of text the Metal bridge keeps), sent as markers: 0xC000|row starts a row, 0x8000|(a<<7)|b appends two characters. */
extern "C" {char native_adreno_metal_rows[48][96];volatile uint32_t native_adreno_metal_rows_seq;}

extern "C" {
/* What the processes that reached this driver did, for the boot diagnostic page. Plain counters and short strings. */
struct NativeAdrenoMetalDiag {
    uint32_t opens, busy, bad, closes, reports, property_reads, foreign_opens, foreign_type, name_reads, calls, last_selector, last_status, ext_entries, ext_selector, ext_status, ext_in, ext_out, tunnel_calls, probes, sel_bits[8], mb_maps, mb_calls, marks_total, marks[16], chk_macf, chk_res, chk_type, type_pass_lo, type_pass_hi, attach_status, inflight, inflight_since_calls, forced_unmaps;
    char opener[3][17];      /* process names of the latest connections, newest first */
    char reader[3][17];      /* process names that read a registry property of the accelerator, newest first */
    char name_reader[17];    /* latest process that read MetalPluginName specifically */
    char starter[3][17];     /* processes that sent plugin marker 1 (the plugin's initWithAcceleratorPort: started), newest first */
    char report[64];         /* the plugin's own result line (SetMetadata text), latest */
    char report2[64];        /* the plugin's Turnip-phase line (text starting "T " or "W "), latest */
    uint32_t grants;         /* open requests covered by the accelerator's process grant when a sandbox check refused */
} native_adreno_metal_diag;
/* BO CPU views (clientMemoryForType for a BO token): [0] calls, [1] new descriptors, [2] served from the view cache, [3] refused (not a BO / out of
 * resources), [4] last refusal's IOReturn, [5] threads inside now. int-115 showed a backboardd thread in IOConnectMapMemory64 ~650 times a second. */
extern "C" {volatile uint32_t native_adreno_view_diag[6],native_adreno_view_kept;}   /* kept: BoCpuUnmap calls that left the view for another lease */
}

/* The open type the Adreno plugin passes to IOServiceOpen ('AG'); it lets the driver tell the plugin from the original IOGPU library
 * (type 1 plus an API value) and from any other client of the accelerator. */
enum : UInt32 {NativeAdrenoPluginOpenType=0x4147,NativeAdrenoTunnelMagic=0x4e544e4c /* 'NTNL' */};
/* The display hand-off (boot-arg sheng-fb-gpu=1, drivers/GopFramebuffer) reaches the plugin through this already admitted connection: the
 * original sandbox profiles admit this class but not the presenter's own service. Methods 0x7000 + the presenter's NGP_METHOD numbers (mode,
 * wait for a frame, complete it) are forwarded unchanged, without the GPU lock (a wait sleeps); memory type NativeAdrenoScanoutType is the
 * writable scanout. Nothing here touches the GPU. */
enum : UInt32 {NativeAdrenoHandoffBase=0x7000,NativeAdrenoHandoffLast=0x7003,NativeAdrenoScanoutType=0x5343,
               NativeAdrenoHandoffOpenType=0x4148};
/* Open type 0x4148 is a connection that serves only the hand-off: it does not mount a GPU core, so it does not need (or take) a
 * GPU connection slot (the Vulkan driver holds one for as long as the Metal device lives). Same class, so the same sandbox admission. */
/* Export (int-147): method NativeAdrenoExportMethod hands one in-order piece of a blob to the board's UFS log (kits/sheng/BootLog.c), which writes
 * the newest complete blob to its export area. The render server sends its pipeline caches this way, so a later image can be built with them.
 * Input: {generation, total, offset, bytes} then the bytes. No GPU state is touched; any connection of the render server may send it. */
enum : UInt32 {NativeAdrenoExportMethod=0x7100,NativeAdrenoExportMaxPiece=3968};
extern "C" {volatile uint32_t native_adreno_export_stats[4];}   /* [0] pieces taken, [1] refused, [2] last generation, [3] its total bytes */

/* Mailbox: a last-resort command channel for a sandbox that admits no external-method selector at all. It is one shared page (memory
 * type NativeAdrenoMailboxType through IOConnectMapMemory64, which the selector filter does not cover) and a kernel poller that runs
 * whatever the live connection writes into it. Layout (little endian dwords): +0 magic, +4 request seq, +8 response seq, +12 method,
 * +16 status, +20 input bytes, +24 output bytes, input at +0x40 (max 0x700), output at +0x800 (max 0x700). */
enum : UInt32 {NativeAdrenoMailboxType=0xC0DE,NativeAdrenoMailboxMagic=0x4d41494c /* 'MAIL' */};
namespace {
IOBufferMemoryDescriptor *gMailbox=nullptr;
volatile uint32_t *gMailboxWords=nullptr;
volatile bool gPollerStarted=false;
}
class AGXDeviceUserClient;
/* The render server's connection serves the mailbox; it is retained while set (under g.lock), so the poller never runs on a freed object. */
static AGXDeviceUserClient *gMailboxOwner=nullptr;
void nativeAdrenoMailboxPoller(void *,wait_result_t);
static IOReturn runCore(AGXDeviceUserClient *,uint32_t method,const void *input,size_t input_size,void *output,size_t output_size);

/* The two sandbox gates IOServiceOpen applies after newUserClient (XNU IOUserClient.cpp is_io_service_open_extended): the MACF open check on
 * the client object and the sandbox's filter resolver. They are run here for the caller too, only to record which one refuses it. */
extern "C" int mac_iokit_check_open(void *cred,void *client,unsigned type);
extern "C" void *kauth_cred_get(void);
extern const struct io_filter_callbacks *gIOUCFilterCallbacks;

extern "C" volatile uint32_t native_adreno_trace_stage;
extern "C" {
/* One entry per IOServiceOpen of the accelerator (not markers): who, which type, what the two sandbox gates said. */
struct NativeAdrenoOpenRecord {char name[20]; uint32_t type, macf, res, attach, foreign;};
struct NativeAdrenoOpenRecord native_adreno_open_log[8];
volatile uint32_t native_adreno_open_log_count;
/* One row per process name that opened the plugin type (first eight names): opens granted, refused (sandbox, cap or no free slot),
 * closes, the slot of its live connection (0xff none), the highest GPU address its mappings reached (MiB) and its last refusal. */
struct NativeAdrenoClientRecord {char name[20]; uint32_t opens, refused, closes, slot, peak_mib, last_refusal;};
struct NativeAdrenoClientRecord native_adreno_client_log[8];
/* The last text line (the plugin's failure reason) each process other than the render server streamed: process name and a 64-byte line. */
struct NativeAdrenoClientText {char name[20]; char text[64];};
struct NativeAdrenoClientText native_adreno_client_text[4];
/* Why the latest mount of a connection failed: [0] 1 no free slot, 2 the core's open, 3 the window scope; [1] its IOReturn; [2] the slot; [3] failures so far. */
volatile uint32_t native_adreno_mount_fail[4];
/* Processes that read MetalPluginName (the Metal discovery), first twelve names, with counts. */
struct NativeAdrenoAskerRecord {char name[20]; uint32_t reads;};
struct NativeAdrenoAskerRecord native_adreno_metal_askers[12];
}
/* GPU address windows inside the board's VA range (GpuPowerProbe gDeviceInfo: 0x20000000..0x80000000 of the 2 GiB aperture). Slot 0 is the
 * render server's (it held the whole 1.5 GiB alone before int-155 and used at most a few hundred MiB); slots 1 and 2 are for other Metal
 * processes. A kernel connection (the board's self-test) maps where it likes and is never scoped. */
/* Slot 0 is the render server's 1 GiB (it reached 934 MiB in int-196); the rest of the aperture is shared by three more processes (PosterBoard for the
 * wallpaper, iconservicesagent for the icons, WidgetRenderer_Default for the widgets: int-196's screen pictures showed a black wallpaper and white widget
 * boxes, none of which had a native device), 170 MiB each (page aligned). int-198: iconservicesagent reached 128 MiB into its window and then lost its
 * Metal device; whether the window is the cause is what the [gpu windows] log row decides before these sizes change. */
enum : unsigned {NativeAdrenoMaxClients=4};
static const struct {uint64_t start,bytes;} kClientWindows[NativeAdrenoMaxClients]={
    {0x20000000ull,0x40000000ull},{0x60000000ull,0x0AAA9000ull},{0x6AAA9000ull,0x0AAA9000ull},{0x75552000ull,0x0AAA9000ull}};

namespace {
void shiftName(char names[3][17]) {
    memcpy(names[2], names[1], 17);
    memcpy(names[1], names[0], 17);
    proc_selfname(names[0], 17);
}
struct Published {
    bool published=false;
    bool failed=false;
    NativeAdrenoStatus status;
    NativeAdrenoVm *vm=nullptr;
    NativeAdrenoSubmit *submit=nullptr;
    NativeAdrenoRing *ring=nullptr;
    NativeAdrenoRingClock clock{};
    const NativeAdrenoTimer *timer=nullptr;
    const NativeAdrenoAbiDeviceInfo *info=nullptr;
    IOLock *lock=nullptr;
    IOService *service=nullptr;
    bool slots[NativeAdrenoMaxClients]{};
    bool kernel_client=false;
    uint32_t input_bits=0,output_bits=0;
    uint32_t opened=0,closed=0;
} g;
}

class NativeAdrenoGpuPublished : public IOService {
    OSDeclareDefaultStructors(NativeAdrenoGpuPublished)
public:
    IOReturn newUserClient(task_t owningTask,void *securityID,UInt32 type,
        OSDictionary *properties,IOUserClient **handler) APPLE_KEXT_OVERRIDE;
};
OSDefineMetaClassAndStructors(NativeAdrenoGpuPublished,IOService)

class AGXDeviceUserClient : public IOUserClient {
    OSDeclareDefaultStructors(AGXDeviceUserClient)
    bool live_=false;
    bool closing_=false;
    IOReturn viewTeardownStatus_=kIOReturnSuccess;
    bool handoffOnly_=false;
    bool kernel_=false;
    NativeAdrenoUserClient *core_=nullptr;   /* own core; deleted in free(), so a call that raced the close sees live_ false, never freed memory */
    unsigned slot_=NativeAdrenoMaxClients;
    char name_[20]{};
    /* One user-view descriptor per BO token, reused by every clientMemoryForType call. IOConnectUnmapMemory64 asks clientMemoryForType for
     * the descriptor again and unmaps only a mapping that belongs to THAT descriptor (kIOMapReference); a fresh descriptor per call made the
     * unmap a silent no-op, the user PTEs outlived BoCpuUnmap, and BoFree handed pages with a live user mapping to the VM:
     * pmap_assert_free panicked the freeing core and the panic screen parked it (the "BoFree hang", dead touch). */
    struct View {uint32_t token;IOMemoryDescriptor *descriptor;};
    /* One view per live BO token. The cache must hold every BO the client can have (NativeAdrenoClientMaxObjects): a BO whose view is not
     * cached makes IOConnectUnmapMemory64 (which asks clientMemoryForType again) a no-op, the user mapping outlives BoFree and the freed pages panic
     * pmap_assert_free. A cache of 64 held until the render server's hundreds of mapped BOs (int-087). */
    enum {MaxViews=NativeAdrenoClientMaxObjects};
    View *views_=nullptr;   /* on the heap: 1024 entries would make the client object too big for the kernel allocator's fixed-size classes */
    IOReturn dropViews(uint32_t token);
    static IOReturn action(OSObject *target,void *reference,IOExternalMethodArguments *arguments);
    IOReturn run(uint32_t method,const void *input,size_t input_size,void *output,size_t output_size);
public:
    void setHandoffOnly() {handoffOnly_=true;}
    bool mount(bool kernel);
    friend IOReturn runCore(AGXDeviceUserClient *,uint32_t,const void *,size_t,void *,size_t);
    friend void nativeAdrenoMailboxPoller(void *,wait_result_t);
    IOReturn clientClose() APPLE_KEXT_OVERRIDE;
    void free() APPLE_KEXT_OVERRIDE;
    IOReturn clientMemoryForType(UInt32 type,IOOptionBits *options,IOMemoryDescriptor **memory) APPLE_KEXT_OVERRIDE;
    IOReturn externalMethod(uint32_t selector,IOExternalMethodArguments *arguments,
        IOExternalMethodDispatch *dispatch,OSObject *target,void *reference) APPLE_KEXT_OVERRIDE;
};
OSDefineMetaClassAndStructors(AGXDeviceUserClient,IOUserClient)

/* NativeAdrenoControl accepts the administrator's first hardware start
 * request after userspace has connected USB. Its properties report progress;
 * NativeAdrenoGpuPublished appears after the CP has completed GPU work. */
class NativeAdrenoControl : public IOService {
    OSDeclareDefaultStructors(NativeAdrenoControl)
public:
    IOReturn setProperties(OSObject *properties) APPLE_KEXT_OVERRIDE;
};
OSDefineMetaClassAndStructors(NativeAdrenoControl,IOService)

static NativeAdrenoControl *controlService=nullptr;

IOReturn NativeAdrenoControl::setProperties(OSObject *properties)
{
    const IOReturn privilege=IOUserClient::clientHasPrivilege(current_task(),kIOClientPrivilegeAdministrator);
    if(privilege) {
        return privilege;
    }
    OSDictionary *dictionary=OSDynamicCast(OSDictionary,properties);
    if(!dictionary || dictionary->getCount()!=1 || dictionary->getObject("Start")!=kOSBooleanTrue) {
        return kIOReturnBadArgument;
    }
    return native_adreno_a6xx_request_start();
}

extern "C" void native_adreno_control_update(uint32_t step,uint32_t status,const char *state)
{
    NativeAdrenoControl *control=controlService;
    if(!control) {
        return;
    }
    control->setProperty("Step",step,32);
    control->setProperty("Status",status,32);
    control->setProperty("State",state);
    control->setProperty("Attempted",state && strcmp(state,"waiting")?kOSBooleanTrue:kOSBooleanFalse);
}

extern "C" IOReturn native_adreno_control_publish(void)
{
    if(controlService) {
        return kIOReturnBusy;
    }
    NativeAdrenoControl *control=new NativeAdrenoControl;
    if(!control || !control->init() || !control->attach(IOService::getPlatform())) {
        OSSafeReleaseNULL(control);
        return kIOReturnNoResources;
    }
    controlService=control;
    control->setName("NativeAdrenoControl");
    control->setProperty("NativeSandboxGrant",kOSBooleanTrue);
    control->setProperty("ChipId",0x06030500u,32);
    native_adreno_control_update(0,0,"waiting");
    control->registerService();
    return kIOReturnSuccess;
}

extern "C" IOReturn native_adreno_initialize_classes(void)
{
    static bool initialized=false;
    if(initialized) {
        return kIOReturnBusy;
    }
    void *handle=OSMetaClass::preModLoad(g_kernel_kmod_info.name);
    if(!handle) {
        return kIOReturnNoResources;
    }
    native_adreno_publish_static_init();
    const OSReturn status=OSMetaClass::postModLoad(handle);
    if(status!=kOSReturnSuccess) {
        return status;
    }
    initialized=true;
    return kIOReturnSuccess;
}

void nativeAdrenoMailboxPoller(void *,wait_result_t)
{
    thread_set_thread_name(current_thread(),"sheng-gpu-mailbox");
    uint32_t last=0;
    for(;;) {
        IOSleep(1);
        if(!gMailboxOwner || !gMailboxWords)continue;
        const uint32_t request=gMailboxWords[1];
        if(request==last || request==gMailboxWords[2])continue;
        last=request;
        const uint32_t method=gMailboxWords[3],in_size=gMailboxWords[5],out_size=gMailboxWords[6];
        IOReturn status=kIOReturnBadArgument;
        if(in_size<=0x700 && out_size<=0x700) {
            uint8_t input[0x700],output[0x700];
            memcpy(input,const_cast<uint32_t *>(gMailboxWords)+0x10,in_size);
            IOLockLock(g.lock);
            AGXDeviceUserClient *owner=gMailboxOwner;
            if(owner)owner->retain();
            IOLockUnlock(g.lock);
            status=owner?runCore(owner,method,input,in_size,output,out_size):kIOReturnNotReady;
            if(owner)owner->release();
            if(!status)memcpy(const_cast<uint32_t *>(gMailboxWords)+0x200,output,out_size);
        }
        native_adreno_metal_diag.mb_calls=native_adreno_metal_diag.mb_calls+1;
        gMailboxWords[4]=uint32_t(status);
        __asm__ volatile("dmb sy" ::: "memory");
        gMailboxWords[2]=request;
        __asm__ volatile("dmb sy" ::: "memory");
    }
}

/* The client log row of name (created for the first eight names); null when the table is full. Under g.lock. */
static NativeAdrenoClientRecord *clientRecord(const char *name)
{
    for(auto &record:native_adreno_client_log) {
        if(!record.name[0]) {strlcpy(record.name,name,sizeof record.name);record.slot=0xff;return &record;}
        if(!strncmp(record.name,name,sizeof record.name))return &record;
    }
    return nullptr;
}
bool AGXDeviceUserClient::mount(bool kernel)
{
    proc_selfname(name_,sizeof name_);
    NativeAdrenoUserClient *core=new NativeAdrenoUserClient;
    if(!core)return false;
    IOLockLock(g.lock);
    bool ok=false;
    /* The render server takes slot 0 (the large window) when free; every other process, or a second render-server connection, the next free one. */
    unsigned slot=NativeAdrenoMaxClients;
    if(kernel)slot=g.kernel_client?NativeAdrenoMaxClients:0;
    else {
        if(!strncmp(name_,"backboardd",sizeof name_) && !g.slots[0])slot=0;
        for(unsigned i=1;slot==NativeAdrenoMaxClients && i<NativeAdrenoMaxClients;++i)if(!g.slots[i])slot=i;
    }
    const IOReturn opened=!g.failed && slot<NativeAdrenoMaxClients?core->open(NativeAdrenoAbiVersion,g.vm,g.submit,g.ring,g.input_bits,g.output_bits):kIOReturnNoSpace;
    if(opened!=kIOReturnSuccess) {native_adreno_mount_fail[0]=slot<NativeAdrenoMaxClients?2:1;native_adreno_mount_fail[1]=uint32_t(opened);native_adreno_mount_fail[2]=slot;native_adreno_mount_fail[3]=native_adreno_mount_fail[3]+1;}
    if(opened==kIOReturnSuccess) {
        if(g.timer)core->attachTimer(g.timer);
        if(g.info)core->attachDeviceInfo(g.info);
        core->attachStatus(&g.status);
        /* Queue ranges: the kernel client keeps range 0 alone (no user slot is live while the board's self-test runs); users get their slot's. */
        const IOReturn scoped=kernel?core->setScope(0,0,0):core->setScope(slot*NativeAdrenoAbiMaxQueues,kClientWindows[slot].start,kClientWindows[slot].bytes);
        if(scoped==kIOReturnSuccess) {
            core_=core;core=nullptr;slot_=slot;kernel_=kernel;live_=true;ok=true;
            if(kernel)g.kernel_client=true;else g.slots[slot]=true;
            ++g.opened;
            if(NativeAdrenoClientRecord *record=clientRecord(name_))record->slot=slot;
        } else {
            native_adreno_mount_fail[0]=3;native_adreno_mount_fail[1]=uint32_t(scoped);native_adreno_mount_fail[2]=slot;native_adreno_mount_fail[3]=native_adreno_mount_fail[3]+1;
            core->releaseAll(&g.clock,0);
        }
    }
    /* The mailbox page and its poller serve the render server's connection only. */
    if(ok && !kernel && slot==0 && !strncmp(name_,"backboardd",sizeof name_)) {
        if(!gMailbox) {
            gMailbox=IOBufferMemoryDescriptor::inTaskWithOptions(kernel_task,kIODirectionInOut,4096,4096);
            if(gMailbox) {gMailboxWords=static_cast<volatile uint32_t *>(gMailbox->getBytesNoCopy());memset(const_cast<uint32_t *>(gMailboxWords),0,4096);}
        }
        if(gMailboxWords) {
            memset(const_cast<uint32_t *>(gMailboxWords),0,4096);
            gMailboxWords[0]=NativeAdrenoMailboxMagic;
            if(!gPollerStarted) {
                thread_t thread=nullptr;
                if(kernel_thread_start(nativeAdrenoMailboxPoller,nullptr,&thread)==KERN_SUCCESS) {thread_deallocate(thread);gPollerStarted=true;}
            }
        }
        if(gMailboxOwner)gMailboxOwner->release();
        retain();gMailboxOwner=this;
    }
    IOLockUnlock(g.lock);
    if(core)delete core;
    return ok;
}
/* Removes the cached view(s) of token (0 = all) and tears down every user mapping of them, so the BO's pages are never freed while a user
 * PTE still points at them, whether or not the client unmapped. */
IOReturn AGXDeviceUserClient::dropViews(uint32_t token)
{
    for(;;) {
        IOMemoryDescriptor *view=nullptr;
        size_t index=MaxViews;
        IOLockLock(g.lock);
        const IOReturn previous=viewTeardownStatus_;
        if(!previous && views_) {
            for(size_t i=0;i<MaxViews;++i) {
                if(views_[i].descriptor && (!token || views_[i].token==token)) {
                    view=views_[i].descriptor;
                    index=i;
                    break;
                }
            }
        }
        IOLockUnlock(g.lock);
        if(previous || !view) {
            return previous;
        }

        /* removeMappingForDescriptor transfers a retained map out of the client's
         * mapping set. Its user PTEs must be removed before the BO can release pages. */
        while(IOMemoryMap *map=removeMappingForDescriptor(view)) {
            const IOReturn status=map->unmap();
            map->release();
            if(status) {
                IOLockLock(g.lock);
                viewTeardownStatus_=status;
                IOLockUnlock(g.lock);
                return status;
            }
            native_adreno_metal_diag.forced_unmaps=native_adreno_metal_diag.forced_unmaps+1;
        }

        IOLockLock(g.lock);
        views_[index].descriptor=nullptr;
        views_[index].token=0;
        IOLockUnlock(g.lock);
        view->release();
    }
}
IOReturn AGXDeviceUserClient::clientClose()
{
    IOLockLock(g.lock);
    closing_=true;
    AGXDeviceUserClient *mailbox=nullptr;
    if(gMailboxOwner==this) {mailbox=gMailboxOwner;gMailboxOwner=nullptr;}
    IOLockUnlock(g.lock);

    IOReturn status=dropViews(0);
    IOLockLock(g.lock);
    if(!status && live_) {
        const uint64_t peak=core_->windowPeak();
        status=core_->releaseAll(&g.clock,2000000);
        /* A failed teardown leaves the objects live and the slot taken: nothing is forced or reused. */
        if(!status) {
            live_=false;
            if(kernel_)g.kernel_client=false;else if(slot_<NativeAdrenoMaxClients)g.slots[slot_]=false;
            ++g.closed;native_adreno_metal_diag.closes=native_adreno_metal_diag.closes+1;
            if(NativeAdrenoClientRecord *record=clientRecord(name_)) {
                record->closes=record->closes+1;record->slot=0xff;
                if(peak>>20>record->peak_mib)record->peak_mib=uint32_t(peak>>20);
            }
        }
    }
    IOLockUnlock(g.lock);
    if(mailbox)mailbox->release();
    if(!status)terminate();
    return status;
}
void AGXDeviceUserClient::free()
{
    const IOReturn status=dropViews(0);
    if(!status && views_) {IODelete(views_,View,MaxViews);views_=nullptr;}
    /* Still live only when a teardown failed: its objects stay referenced by the VM and the ring, so the core is kept, not freed. */
    if(core_ && !live_) {delete core_;core_=nullptr;}
    IOUserClient::free();
}
IOReturn AGXDeviceUserClient::clientMemoryForType(UInt32 type,IOOptionBits *options,IOMemoryDescriptor **memory)
{
    if(memory)*memory=nullptr;
    if(!memory || !options)return kIOReturnBadArgument;
    if(closing_ || viewTeardownStatus_)return kIOReturnNotReady;
    IOReturn status=kIOReturnNotFound;
    if(type==NativeAdrenoScanoutType) {
        if(!live_ && !handoffOnly_)return kIOReturnNotReady;
        IOMemoryDescriptor *scanout=integration.displayMemory?integration.displayMemory():nullptr;
        if(!scanout)return kIOReturnNotReady;
        *memory=scanout;                        /* already retained for the caller */
        *options=kIOMapAnywhere|kIOMapInhibitCache;   /* firmware framebuffer: device memory, never cached by the CPU */
        return kIOReturnSuccess;
    }
    if(type==NativeAdrenoMailboxType) {
        if(live_ && gMailbox && gMailboxOwner==this) {
            gMailbox->retain();
            *memory=gMailbox;
            *options=kIOMapAnywhere;   /* ordinary cached memory: both sides are CPUs, so it is coherent */
            native_adreno_metal_diag.mb_maps=native_adreno_metal_diag.mb_maps+1;
            return kIOReturnSuccess;
        }
        return kIOReturnNotReady;
    }
    native_adreno_view_diag[0]=native_adreno_view_diag[0]+1;
    /* [5]: threads inside this function right now (a thread stuck here, on the gate or in a mapping, shows as a non-zero count that stays). */
    struct Inside {Inside() {__atomic_fetch_add(&native_adreno_view_diag[5],1,__ATOMIC_RELAXED);} ~Inside() {__atomic_fetch_sub(&native_adreno_view_diag[5],1,__ATOMIC_RELAXED);}} inside;
    IOLockLock(g.lock);
    if(!live_ || closing_ || viewTeardownStatus_) {
        IOLockUnlock(g.lock);
        return kIOReturnNotReady;
    }
    if(!views_)views_=IONewZero(View,MaxViews);
    if(!views_) {IOLockUnlock(g.lock);return kIOReturnNoMemory;}
    for(size_t i=0;i<MaxViews;++i)if(views_[i].descriptor && views_[i].token==type) {
        views_[i].descriptor->retain();*memory=views_[i].descriptor;*options=kIOMapAnywhere|kIOMapInhibitCache;
        native_adreno_view_diag[2]=native_adreno_view_diag[2]+1;
        IOLockUnlock(g.lock);
        return kIOReturnSuccess;
    }
    NativeAdrenoMemory *backing=live_?core_->memoryForToken(type):nullptr;
    if(backing) {
        /* The descriptor describes the BO's physical spans without owning their
         * allocation. XNU replaces the user-option bits below with the caller's
         * map flags; Turnip supplies kIOMapWriteCombineCache for these RAM pages.
         * dropViews removes every tracked user mapping before the BO releases them. */
        /* A BO's pages can come scattered, one span per page (a 64 MiB BO is 16384 of them): the descriptor takes them all, as many as the GPU's
         * own table accepts. A fixed 64-entry array made every scattered BO above 256 KiB fail to map (VK_ERROR_MEMORY_MAP_FAILED, int-084),
         * only when memory happened to be fragmented. The descriptor copies the ranges. */
        enum {MaxSpans=65536};
        const size_t count=backing->spanCount();
        if(!count || count>MaxSpans)status=kIOReturnNoResources;
        else {
            IOAddressRange *ranges=IONewData(IOAddressRange,count);
            if(!ranges)status=kIOReturnNoMemory;
            else {
                for(size_t i=0;i<count;++i)ranges[i]={backing->spans()[i].physical,backing->spans()[i].bytes};
                IOMemoryDescriptor *descriptor=IOMemoryDescriptor::withAddressRanges(ranges,(UInt32)count,
                    static_cast<IOOptionBits>(kIODirectionInOut)|static_cast<IOOptionBits>(kIOMemoryTypePhysical64),nullptr);
                IODeleteData(ranges,IOAddressRange,count);
                if(!descriptor)status=kIOReturnNoMemory;
                else {
                    bool cached=false;
                    for(size_t i=0;i<MaxViews;++i)if(!views_[i].descriptor) {views_[i].token=type;views_[i].descriptor=descriptor;descriptor->retain();cached=true;break;}
                    if(!cached) {
                        /* Never hand out a view that cannot be torn down again. */
                        descriptor->release();
                        status=kIOReturnNoResources;
                    } else {
                        *memory=descriptor;
                        *options=kIOMapAnywhere|kIOMapInhibitCache;
                        native_adreno_view_diag[1]=native_adreno_view_diag[1]+1;
                        status=kIOReturnSuccess;
                    }
                }
            }
        }
    }
    IOLockUnlock(g.lock);
    if(status){native_adreno_view_diag[3]=native_adreno_view_diag[3]+1;native_adreno_view_diag[4]=(uint32_t)status;}
    return status;
}
/* Fence waits as the callers see them (int-147; method 6's own statistics count every poll): [0] waits, [1] us waited in total, [2] pauses
 * taken outside the connection lock, [3] the longest wait in us. */
extern "C" {volatile uint64_t native_adreno_wait_outer[4];}
static IOReturn runCore(AGXDeviceUserClient *connection,uint32_t method,const void *input,size_t input_size,void *output,size_t output_size)
{
    if(method==NativeAdrenoMethodSetMetadata && input && input_size==sizeof(NativeAdrenoAbiMetadata)) {
        const NativeAdrenoAbiMetadata *meta=static_cast<const NativeAdrenoAbiMetadata *>(input);
        /* The raw test's line and the Turnip phase's line ("T ..." result or "W ..." watchdog sample) are kept apart, so neither hides the other. */
        const bool turnip=meta->bytes>=2 && (meta->text[0]=='T' || meta->text[0]=='W') && meta->text[1]==' ';
        char *line=turnip?native_adreno_metal_diag.report2:native_adreno_metal_diag.report;
        uint32_t n=meta->bytes<sizeof(native_adreno_metal_diag.report)-1?meta->bytes:uint32_t(sizeof(native_adreno_metal_diag.report)-1);
        memset(line,0,sizeof(native_adreno_metal_diag.report));
        memcpy(line,meta->text,n);
        native_adreno_metal_diag.reports=native_adreno_metal_diag.reports+1;
    }
    /* A call that is still running when the boot page is photographed shows as inflight = its method number + 1 (0 = none), so a hang in the
     * kernel is attributable to one ABI method. */
    native_adreno_metal_diag.inflight=method+1;
    native_adreno_metal_diag.inflight_since_calls=native_adreno_metal_diag.calls;
    IOReturn status;
    if(method==NativeAdrenoMethodWaitFence && connection->live_ && input && input_size==sizeof(NativeAdrenoAbiWaitFence) && g.clock.now_us) {
        /* int-147: a fence wait polls under the lock but pauses outside it. int-146 slept inside the lock (up to the caller's 1 ms slice per
         * call), so every other thread's submit, map or allocation waited for the GPU to finish unrelated work: a frame could not be queued
         * while the previous one ran. Each poll is the core's own zero-timeout wait (same checks, retirement and statistics); the pause is the
         * board's (until the next GPU interrupt, at most 100 us). */
        NativeAdrenoAbiWaitFence poll{};
        memcpy(&poll,input,sizeof(poll));
        const uint64_t timeout_us=poll.timeout_ns/1000,began=g.clock.now_us(g.clock.cookie);
        poll.timeout_ns=0;
        uint64_t waited=0;
        for(;;) {
            /* The connection's own core while it is live; a close that ran between two polls ends the wait. */
            IOLockLock(g.lock);
            status=connection->live_ && !connection->closing_ && !connection->viewTeardownStatus_ && !g.failed?
                connection->core_->dispatch(method,&poll,sizeof(poll),output,output_size,&g.clock):kIOReturnNotReady;
            IOLockUnlock(g.lock);
            waited=g.clock.now_us(g.clock.cookie)-began;
            if(status!=kIOReturnTimeout || waited>=timeout_us)break;
            native_adreno_wait_outer[2]=native_adreno_wait_outer[2]+1;
            if(!g.clock.idle || g.clock.idle(g.clock.cookie))break;
        }
        native_adreno_wait_outer[0]=native_adreno_wait_outer[0]+1;native_adreno_wait_outer[1]=native_adreno_wait_outer[1]+waited;
        if(waited>native_adreno_wait_outer[3])native_adreno_wait_outer[3]=waited;
    } else {
        IOLockLock(g.lock);
        status=connection->live_ && !connection->closing_ && !connection->viewTeardownStatus_ && (!g.failed || method==NativeAdrenoMethodGetStatus)?
            connection->core_->dispatch(method,input,input_size,output,output_size,&g.clock):kIOReturnNotReady;
        if(method==NativeAdrenoMethodBoMap && !status && connection->live_ && !connection->kernel_)
            if(NativeAdrenoClientRecord *record=clientRecord(connection->name_))
                if(connection->core_->windowPeak()>>20>record->peak_mib)record->peak_mib=uint32_t(connection->core_->windowPeak()>>20);
        IOLockUnlock(g.lock);
    }
    native_adreno_metal_diag.inflight=0;
    native_adreno_metal_diag.calls=native_adreno_metal_diag.calls+1;
    native_adreno_metal_diag.last_selector=method;
    native_adreno_metal_diag.last_status=uint32_t(status);
    return status;
}
IOReturn AGXDeviceUserClient::run(uint32_t method,const void *input,size_t input_size,void *output,size_t output_size)
{
    if(method>=NativeAdrenoHandoffBase && method<=NativeAdrenoHandoffLast) {
        return integration.displayCall?
            integration.displayCall(method-NativeAdrenoHandoffBase,current_task(),input,input_size,output,output_size):
            kIOReturnUnsupported;
    }
    if(method==NativeAdrenoExportMethod) {
        struct Piece {uint32_t generation,total,offset,bytes;};
        char caller[20]={};proc_selfname(caller,sizeof caller);
        Piece piece{};
        if(input && input_size>=sizeof(piece))memcpy(&piece,input,sizeof(piece));
        const bool valid=!output_size && input && input_size>=sizeof(piece) && piece.bytes && piece.bytes<=NativeAdrenoExportMaxPiece &&
            input_size==sizeof(piece)+piece.bytes && !strncmp(caller,"backboardd",sizeof caller);
        const int stored=valid && integration.exportPut?integration.exportPut(piece.generation,piece.total,piece.offset,
            static_cast<const uint8_t *>(input)+sizeof(piece),piece.bytes):-1;
        if(stored) {native_adreno_export_stats[1]=native_adreno_export_stats[1]+1;return kIOReturnBadArgument;}
        native_adreno_export_stats[0]=native_adreno_export_stats[0]+1;
        native_adreno_export_stats[2]=piece.generation;native_adreno_export_stats[3]=piece.total;
        return kIOReturnSuccess;
    }
    if(method==NativeAdrenoMethodBoCpuUnmap && input && input_size==sizeof(NativeAdrenoAbiBoCpuUnmap)) {
        /* The view is shared by every CPU mapping of the BO, and dropViews removes all of them. A BO mapped twice (the view cache served 207
         * second requests in int-177) lost both mappings at the first unmap, and the other holder's next store hit an unmapped page: the
         * render server's crash in the command stream writes of vkCmdCopyBuffer. Only the last lease takes the view down; the earlier
         * ones leave their mapping until it, or the client unmaps it itself (IOConnectUnmapMemory64 works on the cached view). */
        const uint32_t token=static_cast<const NativeAdrenoAbiBoCpuUnmap *>(input)->token;
        IOLockLock(g.lock);
        const uint32_t leases=live_?core_->mappingLeases(token):0;
        IOLockUnlock(g.lock);
        if(leases>1)native_adreno_view_kept=native_adreno_view_kept+1;
        else {
            const IOReturn status=dropViews(token);
            if(status) {
                return status;
            }
        }
    }
    if(method==NativeAdrenoMethodBoImport) {
        /* The pages belong to the task making this call (an external method runs in the caller's context). */
        if(!input || input_size!=sizeof(NativeAdrenoAbiBoImport) || !output || output_size!=sizeof(NativeAdrenoAbiBoAllocReply))
            return kIOReturnBadArgument;
        NativeAdrenoAbiBoImport request{};
        memcpy(&request,input,sizeof(request));
        NativeAdrenoAbiBoAllocReply reply{};
        native_adreno_metal_diag.inflight=method+1;
        IOLockLock(g.lock);
        const IOReturn status=live_ && !closing_ && !viewTeardownStatus_?
            core_->importBo(&request,current_task(),&reply):kIOReturnNotReady;
        IOLockUnlock(g.lock);
        native_adreno_metal_diag.inflight=0;
        native_adreno_metal_diag.calls=native_adreno_metal_diag.calls+1;
        native_adreno_metal_diag.last_selector=method;
        native_adreno_metal_diag.last_status=uint32_t(status);
        if(!status)memcpy(output,&reply,sizeof(reply));
        return status;
    }
    return runCore(this,method,input,input_size,output,output_size);
}
IOReturn AGXDeviceUserClient::action(OSObject *target,void *,IOExternalMethodArguments *arguments)
{
    AGXDeviceUserClient *connection=OSDynamicCast(AGXDeviceUserClient,target);
    if(!connection || !arguments)return kIOReturnBadArgument;
    return connection->run(arguments->selector,arguments->structureInput,arguments->structureInputSize,
        arguments->structureOutput,arguments->structureOutputSize);
}
IOReturn AGXDeviceUserClient::externalMethod(uint32_t selector,IOExternalMethodArguments *arguments,
    IOExternalMethodDispatch *,OSObject *target,void *reference)
{
    /* One entry per ABI method; sizes are the exact wire records. */
    static const IOExternalMethodDispatch table[]={
        {&action,0,0,0,sizeof(NativeAdrenoAbiParams)},
        {&action,0,sizeof(NativeAdrenoAbiBoAlloc),0,sizeof(NativeAdrenoAbiBoAllocReply)},
        {&action,0,sizeof(NativeAdrenoAbiBoMap),0,sizeof(NativeAdrenoAbiBoMapReply)},
        {&action,0,sizeof(NativeAdrenoAbiBoFree),0,0},
        {&action,0,sizeof(NativeAdrenoAbiSubmit),0,sizeof(NativeAdrenoAbiSubmitReply)},
        {&action,0,sizeof(NativeAdrenoAbiWaitFence),0,0},
        {&action,0,0,0,sizeof(NativeAdrenoAbiTimestamp)},
        {&action,0,0,0,sizeof(NativeAdrenoAbiStatus)},
        {&action,0,sizeof(NativeAdrenoAbiMetadata),0,0},
        {&action,0,sizeof(NativeAdrenoAbiMetadata),0,sizeof(NativeAdrenoAbiMetadata)},
        {&action,0,sizeof(NativeAdrenoAbiQueue),0,sizeof(NativeAdrenoAbiQueueReply)},
        {&action,0,sizeof(NativeAdrenoAbiQueue),0,0},
        {&action,0,sizeof(NativeAdrenoAbiBoCpuMap),0,sizeof(NativeAdrenoAbiBoCpuMapReply)},
        {&action,0,sizeof(NativeAdrenoAbiBoCpuUnmap),0,0},
        {&action,0,0,0,sizeof(NativeAdrenoAbiDeviceInfo)},
        {&action,0,sizeof(NativeAdrenoAbiBoImport),0,sizeof(NativeAdrenoAbiBoAllocReply)}};
    native_adreno_metal_diag.ext_entries=native_adreno_metal_diag.ext_entries+1;
    if(selector<256)native_adreno_metal_diag.sel_bits[selector>>5]|=1u<<(selector&31);
    /* Tunnel. The sandbox resolves a filter policy for this class at open time and then admits only the selectors its profile lists,
     * so which selector numbers reach this object is decided outside the driver. A tunnel call carries the real ABI method in its
     * first eight bytes and may use ANY admitted selector: {magic, method, payload...} in, the method's exact output out. Method
     * 0xffffffff is a probe that only proves the selector was admitted. */
    if(arguments && arguments->structureInput && arguments->structureInputSize>=8 &&
        static_cast<const uint32_t *>(arguments->structureInput)[0]==NativeAdrenoTunnelMagic) {
        const uint32_t *header=static_cast<const uint32_t *>(arguments->structureInput);
        if(header[1]==0xffffffffu) {
            native_adreno_metal_diag.probes=native_adreno_metal_diag.probes+1;
            return kIOReturnSuccess;
        }
        native_adreno_metal_diag.tunnel_calls=native_adreno_metal_diag.tunnel_calls+1;
        const IOReturn tunneled=run(header[1],reinterpret_cast<const uint8_t *>(arguments->structureInput)+8,
            arguments->structureInputSize-8,arguments->structureOutput,arguments->structureOutputSize);
        native_adreno_metal_diag.ext_status=uint32_t(tunneled);
        return tunneled;
    }
    native_adreno_metal_diag.ext_selector=selector;
    native_adreno_metal_diag.ext_in=arguments?arguments->structureInputSize:0xffff;
    native_adreno_metal_diag.ext_out=arguments?arguments->structureOutputSize:0xffff;
    if(selector<1 || selector>sizeof(table)/sizeof(table[0])) {
        native_adreno_metal_diag.ext_status=uint32_t(kIOReturnUnsupported);
        return kIOReturnUnsupported;
    }
    const IOReturn result=IOUserClient::externalMethod(selector,arguments,const_cast<IOExternalMethodDispatch *>(&table[selector-1]),
        target?target:this,reference);
    native_adreno_metal_diag.ext_status=uint32_t(result);
    return result;
}
/* limit > 0 caps the connections ever granted: a plugin that crashes its host process on every launch (the process respawns and
 * loads it again) must not be able to keep a system process in a crash loop, so after the cap further opens are refused. */
extern "C" IOReturn native_adreno_new_user_client(IOService *provider,task_t owningTask,void *securityID,UInt32 type,
    OSDictionary *properties,IOUserClient **handler,unsigned limit)
{
    if(handler)*handler=nullptr;
    if(!handler || !g.published)return kIOReturnNotReady;
    /* Progress markers: open types 0x4147xxxx (xxxx nonzero) are not connections. The plugin sends one before and after each step so
     * that a sandbox that admits neither selectors nor mapping, or a crash, still leaves a trace of how far it got. */
    if((type>>16)==NativeAdrenoPluginOpenType && (type&0xffff)) {
        const uint32_t value=type&0xffff,slot=native_adreno_metal_diag.marks_total&15;
        /* Text stream into report2, one character per marker: 0x7fff starts a line, 0x70cc appends character cc. It needs no connection,
         * so the plugin's Turnip watchdog can report while Turnip itself holds its connection slot. Not counted as markers. */
        if(value>=0x8000) {
            static unsigned row=48,length;
            if(value>=0xC000) {
                row=value&0x3f;length=0;
                if(row<48)memset(native_adreno_metal_rows[row],0,sizeof(native_adreno_metal_rows[row]));
                native_adreno_metal_rows_seq=native_adreno_metal_rows_seq+1;
            } else if(row<48) {
                const char pair[2]={char((value>>7)&0x7f),char(value&0x7f)};
                for(unsigned i=0;i<2;++i)if(pair[i] && length<sizeof(native_adreno_metal_rows[row])-1)native_adreno_metal_rows[row][length++]=pair[i];
            }
            return kIOReturnUnsupported;
        }
        if(value>=0x7000 && value<0x8000) {
            /* 0x7ffe starts a line in report (GPU7) instead: the Metal bridge's result, after the raw test's line has been read. */
            static uint32_t length;
            static char *line=native_adreno_metal_diag.report2;
            static_assert(sizeof(native_adreno_metal_diag.report)==sizeof(native_adreno_metal_diag.report2),"stream lines");
            if(value==0x7fff || value==0x7ffe) {
                line=value==0x7fff?native_adreno_metal_diag.report2:native_adreno_metal_diag.report;
                /* A line from a process other than the render server goes to that process's own row (native_adreno_client_text): the render server
                 * rewrites report2 every ten seconds, so the reason another process's device failed would be gone from it. */
                char who[20]={};
                proc_selfname(who,sizeof who);
                if(value==0x7fff && strncmp(who,"backboardd",sizeof who)) {
                    for(auto &entry:native_adreno_client_text) {
                        if(!entry.name[0]) strlcpy(entry.name,who,sizeof entry.name);
                        if(!strncmp(entry.name,who,sizeof entry.name)) {line=entry.text;break;}
                    }
                }
                memset(line,0,sizeof(native_adreno_metal_diag.report2));length=0;
            }
            else if(value<0x7100 && length<sizeof(native_adreno_metal_diag.report2)-1) {line[length++]=char(value&0xff);line[length]=0;}
            return kIOReturnUnsupported;
        }
        native_adreno_metal_diag.marks[slot]=value;
        native_adreno_metal_diag.marks_total=native_adreno_metal_diag.marks_total+1;
        if(value==1) {
            memcpy(native_adreno_metal_diag.starter[2],native_adreno_metal_diag.starter[1],17);
            memcpy(native_adreno_metal_diag.starter[1],native_adreno_metal_diag.starter[0],17);
            proc_selfname(native_adreno_metal_diag.starter[0],17);
        }
        return kIOReturnUnsupported;
    }
    shiftName(native_adreno_metal_diag.opener);
    AGXDeviceUserClient *connection=new AGXDeviceUserClient;
    if(!connection)return kIOReturnNoMemory;
    if(!connection->initWithTask(owningTask,securityID,type,properties)) {
        connection->release();
        native_adreno_metal_diag.bad=native_adreno_metal_diag.bad+1;
        return kIOReturnNoResources;
    }
    /* XNU holds this gate across descriptor lookup, user mapping creation and
     * insertion into the client's mapping set. Close and BO teardown share it. */
    connection->setProperty(kIOUserClientDefaultLockingKey,kOSBooleanTrue);
    connection->setProperty(kIOUserClientDefaultLockingSingleThreadExternalMethodKey,kOSBooleanTrue);
    /* IOService::newUserClient attaches and starts what it creates (a client without a provider chain is also a reason for the sandbox to
     * refuse). Then the two sandbox gates IOServiceOpen would apply afterwards are run here for every opener and logged with the
     * process name and open type, so the boot page shows which processes the sandbox lets reach this class and with which type. */
    unsigned attach_state=0;
    if(provider) {
        const bool attached=connection->attach(provider);
        attach_state=attached?1:2;
        if(attached && !connection->start(provider)) {
            connection->detach(provider);
            attach_state=3;
        }
    }
    native_adreno_metal_diag.attach_status=attach_state;
    const uint32_t macf=uint32_t(mac_iokit_check_open(kauth_cred_get(),connection,type));
    uint32_t resolver=0xfeedfeed;
    if(gIOUCFilterCallbacks && gIOUCFilterCallbacks->io_filter_resolver) {
        io_filter_policy_t policy=0;
        const IOReturn resolved=gIOUCFilterCallbacks->io_filter_resolver(owningTask,connection,type,&policy);
        resolver=uint32_t(resolved);
        if(resolved==kIOReturnSuccess && policy && gIOUCFilterCallbacks->io_filter_release)gIOUCFilterCallbacks->io_filter_release(policy);
    }
    native_adreno_metal_diag.chk_type=type;
    native_adreno_metal_diag.chk_macf=macf;
    native_adreno_metal_diag.chk_res=resolver;
    {
        NativeAdrenoOpenRecord &record=native_adreno_open_log[native_adreno_open_log_count&7];
        proc_selfname(record.name,sizeof(record.name));
        record.type=type;record.macf=macf;record.res=resolver;record.attach=attach_state;record.foreign=type!=NativeAdrenoPluginOpenType;
        native_adreno_open_log_count=native_adreno_open_log_count+1;
    }
    if(type==NativeAdrenoHandoffOpenType) {
        connection->setHandoffOnly();
        *handler=connection;
        return kIOReturnSuccess;
    }
    /* Only the Adreno plugin's open (type NativeAdrenoPluginOpenType) is served. The original IOGPU library opens the accelerator with type 1
     * (and an API value in the upper bits) for an IOGPUDeviceUserClient, which this driver is not; refuse it cleanly. */
    if(type!=NativeAdrenoPluginOpenType) {
        connection->release();
        native_adreno_metal_diag.foreign_opens=native_adreno_metal_diag.foreign_opens+1;
        native_adreno_metal_diag.foreign_type=type;
        return kIOReturnUnsupported;
    }
    /* The kernel's open path reads NativeSandboxGrantProcess from the service
     * passed to IOServiceOpen. Match its comma-separated process names here
     * so the counters report every open covered by the service's grant. */
    const bool kernel=owningTask==kernel_task;
    char opener[20]={};
    proc_selfname(opener,sizeof opener);
    const OSString *grant=provider?OSDynamicCast(OSString,provider->getProperty("NativeSandboxGrantProcess")):nullptr;
    bool granted = false;
    if (grant) {
        for (const char *entry = grant->getCStringNoCopy(); *entry;) {
            size_t length = 0;
            while (entry[length] && entry[length] != ',') {
                ++length;
            }

            const size_t compared = length < 16 ? length : 16;
            if (length && !strncmp(opener, entry, compared) &&
                (length >= 16 || opener[length] == 0)) {
                granted = true;
                break;
            }

            entry += length + (entry[length] == ',' ? 1 : 0);
        }
    }

    if(!kernel && granted &&
       (macf || (resolver!=uint32_t(kIOReturnSuccess) && resolver!=uint32_t(kIOReturnUnsupported)))) {
        native_adreno_metal_diag.grants=native_adreno_metal_diag.grants+1;
    }
    /* limit caps the connections granted to each process name, so one process that crashes on every launch cannot loop forever, and cannot use
     * up the others' opens; a process other than the render server gets at most eight per boot. */
    const unsigned cap=strncmp(opener,"backboardd",sizeof opener)?(limit && limit<8?limit:8):limit;
    IOLockLock(g.lock);
    NativeAdrenoClientRecord *record=kernel?nullptr:clientRecord(opener);
    const bool capped=cap && record && record->opens>=cap;
    IOLockUnlock(g.lock);
    IOReturn refusal=kIOReturnSuccess;
    if(capped)refusal=kIOReturnBusy;
    else if(!connection->mount(kernel))refusal=kIOReturnBusy;   /* every slot taken */
    if(refusal) {
        connection->release();
        native_adreno_metal_diag.busy=native_adreno_metal_diag.busy+1;
        IOLockLock(g.lock);
        if(record) {record->refused=record->refused+1;record->last_refusal=uint32_t(refusal);}
        IOLockUnlock(g.lock);
        return refusal;
    }
    IOLockLock(g.lock);
    if(record)record->opens=record->opens+1;
    IOLockUnlock(g.lock);
    native_adreno_metal_diag.opens=native_adreno_metal_diag.opens+1;
    *handler=connection;
    return kIOReturnSuccess;
}
extern "C" void native_adreno_note_property_read(const char *key)
{
    if(key && !strcmp(key,"MetalPluginName")) {
        proc_selfname(native_adreno_metal_diag.name_reader,17);
        native_adreno_metal_diag.name_reads=native_adreno_metal_diag.name_reads+1;
        char name[20]={};
        proc_selfname(name,sizeof name);
        /* Unlocked: the discovery runs at process start, a lost count is harmless and a row is claimed by its first byte. */
        for(auto &asker:native_adreno_metal_askers) {
            if(!asker.name[0]) {strlcpy(asker.name,name,sizeof asker.name);asker.reads=1;break;}
            if(!strncmp(asker.name,name,sizeof asker.name)) {asker.reads=asker.reads+1;break;}
        }
    }
    shiftName(native_adreno_metal_diag.reader);
    native_adreno_metal_diag.property_reads=native_adreno_metal_diag.property_reads+1;
}
IOReturn NativeAdrenoGpuPublished::newUserClient(task_t owningTask,void *securityID,UInt32 type,
    OSDictionary *properties,IOUserClient **handler)
{
    return native_adreno_new_user_client(this,owningTask,securityID,type,properties,handler,0);
}

/* Publishes the service once. Everything passed in must outlive the system (the board probe keeps them in statics) and
 * the VM, submit object and ring must already be attached to the live GPU context. The core must not be open. */
extern "C" IOReturn native_adreno_publish(NativeAdrenoVm *vm,NativeAdrenoSubmit *submit,NativeAdrenoRing *ring,
    const NativeAdrenoRingClock *clock,const NativeAdrenoTimer *timer,const NativeAdrenoAbiDeviceInfo *info,
    uint32_t input_bits,uint32_t output_bits)
{
    if(g.published || !vm || !submit || !ring || !clock || !clock->now_us)return kIOReturnBadArgument;
    g.lock=IOLockAlloc();
    NativeAdrenoGpuPublished *service=new NativeAdrenoGpuPublished;
    if(!g.lock || !service || !service->init() || !service->attach(IOService::getPlatform())) {
        if(service)service->release();
        return kIOReturnNoMemory;
    }
    g.vm=vm;g.submit=submit;g.ring=ring;g.clock=*clock;g.timer=timer;g.info=info;g.input_bits=input_bits;g.output_bits=output_bits;
    g.service=service;g.published=true;
    service->setName("NativeAdrenoGpu");
    service->registerService();

    unsigned metal=1;
    PE_parse_boot_argn("native-adreno-metal",&metal,sizeof(metal));
    if(metal) {
        const IOReturn status=native_adreno_accelerator_publish(service,info?info->chip_id:0);
        service->setProperty("MetalStatus",uint32_t(status),32);
        service->setProperty("MetalAvailable",status==kIOReturnSuccess?kOSBooleanTrue:kOSBooleanFalse);
        if(status) {
            IOLog("NativeAdrenoA6xx: Metal accelerator publication failed %08x\n",status);
        }
    }
    return kIOReturnSuccess;
}
extern "C" uint32_t native_adreno_publish_stats(uint32_t *closed)
{
    if(closed)*closed=g.closed;
    return g.opened;
}

extern "C" {
extern volatile uint32_t native_adreno_method_stats[20][4];
extern volatile uint32_t native_adreno_client_stats[5][6];
extern volatile uint32_t native_adreno_pool_stats[8];
extern volatile uint32_t native_adreno_import_stage;
extern volatile uint64_t native_adreno_import_fail_pa,native_adreno_import_fail_offset;
}

/* The USB worker requests one sample of discovery, client and submission
 * counters. Logging takes place after releasing the GPU connection lock. */
extern "C" void native_adreno_metal_dump_stats(void)
{
    const NativeAdrenoMetalDiag diag=native_adreno_metal_diag;
    IOLog("NativeAdrenoMetal: begin published=%u trace=%u rows_seq=%u\n",
        unsigned(g.published),native_adreno_trace_stage,native_adreno_metal_rows_seq);
    IOLog("NativeAdrenoMetal: opens=%u busy=%u bad=%u closes=%u grants=%u reports=%u property_reads=%u name_reads=%u foreign_opens=%u foreign_type=%08x\n",
        diag.opens,diag.busy,diag.bad,diag.closes,diag.grants,diag.reports,
        diag.property_reads,diag.name_reads,diag.foreign_opens,diag.foreign_type);
    IOLog("NativeAdrenoMetal: calls=%u selector=%u status=%08x inflight=%u since_calls=%u ext_entries=%u ext_selector=%u ext_status=%08x ext_in=%u ext_out=%u tunnel=%u probes=%u\n",
        diag.calls,diag.last_selector,diag.last_status,diag.inflight,diag.inflight_since_calls,
        diag.ext_entries,diag.ext_selector,diag.ext_status,diag.ext_in,diag.ext_out,
        diag.tunnel_calls,diag.probes);
    IOLog("NativeAdrenoMetal: attach=%u macf=%u resolver=%u type=%08x pass=%08x/%08x mailbox_maps=%u mailbox_calls=%u forced_unmaps=%u\n",
        diag.attach_status,diag.chk_macf,diag.chk_res,diag.chk_type,diag.type_pass_hi,
        diag.type_pass_lo,diag.mb_maps,diag.mb_calls,diag.forced_unmaps);
    IOLog("NativeAdrenoMetal: selectors=%08x %08x %08x %08x %08x %08x %08x %08x\n",
        diag.sel_bits[0],diag.sel_bits[1],diag.sel_bits[2],diag.sel_bits[3],
        diag.sel_bits[4],diag.sel_bits[5],diag.sel_bits[6],diag.sel_bits[7]);
    IOLog("NativeAdrenoMetal: name_reader=%.*s report=%.*s report2=%.*s\n",
        17,diag.name_reader,64,diag.report,64,diag.report2);
    for(unsigned index=0;index<3;++index) {
        IOLog("NativeAdrenoMetal: recent=%u opener=%.*s reader=%.*s starter=%.*s\n",
            index,17,diag.opener[index],17,diag.reader[index],17,diag.starter[index]);
    }
    const uint32_t markCount=diag.marks_total<16?diag.marks_total:16;
    for(uint32_t index=0;index<markCount;++index) {
        const uint32_t serial=diag.marks_total-markCount+index;
        IOLog("NativeAdrenoMetal: marker=%u value=%04x\n",serial,diag.marks[serial&15]);
    }

    for(unsigned index=0;index<12;++index) {
        const NativeAdrenoAskerRecord asker=native_adreno_metal_askers[index];
        if(asker.name[0]) {
            IOLog("NativeAdrenoMetal: discovery process=%.*s reads=%u\n",20,asker.name,asker.reads);
        }
    }
    for(unsigned index=0;index<8;++index) {
        const NativeAdrenoOpenRecord open=native_adreno_open_log[index];
        if(open.name[0]) {
            IOLog("NativeAdrenoMetal: open=%u process=%.*s type=%08x macf=%u resolver=%u attach=%u foreign=%u\n",
                index,20,open.name,open.type,open.macf,open.res,open.attach,open.foreign);
        }
        const NativeAdrenoClientRecord client=native_adreno_client_log[index];
        if(client.name[0]) {
            IOLog("NativeAdrenoMetal: client=%.*s opens=%u refused=%u closes=%u slot=%u peak_mib=%u last_refusal=%08x\n",
                20,client.name,client.opens,client.refused,client.closes,client.slot,
                client.peak_mib,client.last_refusal);
        }
    }
    for(unsigned index=0;index<4;++index) {
        const NativeAdrenoClientText client=native_adreno_client_text[index];
        if(client.name[0]) {
            IOLog("NativeAdrenoMetal: client_report process=%.*s text=%.*s\n",20,client.name,64,client.text);
        }
    }
    for(unsigned index=0;index<48;++index) {
        char row[96];
        memcpy(row,native_adreno_metal_rows[index],sizeof(row));
        if(row[0]) {
            IOLog("NativeAdrenoMetal: row=%u text=%.*s\n",index,96,row);
        }
    }

    IOLog("NativeAdrenoMetal: mount_reason=%u status=%08x slot=%u failures=%u views=%u new=%u reused=%u refused=%u last_refusal=%08x inside=%u kept=%u\n",
        native_adreno_mount_fail[0],native_adreno_mount_fail[1],native_adreno_mount_fail[2],native_adreno_mount_fail[3],
        native_adreno_view_diag[0],native_adreno_view_diag[1],native_adreno_view_diag[2],native_adreno_view_diag[3],
        native_adreno_view_diag[4],native_adreno_view_diag[5],native_adreno_view_kept);
    for(unsigned index=0;index<5;++index) {
        const volatile uint32_t *counts=native_adreno_client_stats[index];
        IOLog("NativeAdrenoMetal: bo_slot=%u live=%u peak=%u refused=%u status=%08x refused_bytes=%u mapped_mib=%u\n",
            index,counts[0],counts[1],counts[2],counts[3],counts[4],counts[5]);
    }
    IOLog("NativeAdrenoMetal: pool_mib=%u pool_bos=%u fallback_bos=%u used_mib=%u peak_mib=%u scattered_bos=%u scattered_spans=%u imported_spans=%u\n",
        native_adreno_pool_stats[0],native_adreno_pool_stats[1],native_adreno_pool_stats[2],native_adreno_pool_stats[3],
        native_adreno_pool_stats[4],native_adreno_pool_stats[5],native_adreno_pool_stats[6],native_adreno_pool_stats[7]);
    IOLog("NativeAdrenoMetal: import_stage=%u fail_pa=%llx fail_offset=%llx waits=%llu total_us=%llu pauses=%llu longest_us=%llu\n",
        native_adreno_import_stage,native_adreno_import_fail_pa,native_adreno_import_fail_offset,
        native_adreno_wait_outer[0],native_adreno_wait_outer[1],native_adreno_wait_outer[2],native_adreno_wait_outer[3]);
    for(unsigned method=0;method<20;++method) {
        const volatile uint32_t *counts=native_adreno_method_stats[method];
        if(counts[0]) {
            IOLog("NativeAdrenoMetal: method=%u calls=%u failures=%u status=%08x total_us=%u\n",
                method,counts[0],counts[1],counts[2],counts[3]);
        }
    }

    if(g.published && g.lock && IOLockTryLock(g.lock)) {
        const NativeAdrenoSubmitStats submit=g.submit->stats();
        const size_t objects=g.vm->objectCount(),spans=g.vm->spanCount(),jobs=g.submit->outstanding();
        const uint32_t submitted=g.ring->lastSubmitted(),completed=g.ring->completedFence();
        const uint32_t opened=g.opened,closed=g.closed;
        const uint64_t faults=g.status.faults(),suspends=g.status.suspends();
        const bool failed=g.failed,vmPoisoned=g.vm->poisoned(),submitPoisoned=g.submit->poisoned(),ringPoisoned=g.ring->poisoned();
        IOLockUnlock(g.lock);

        IOLog("NativeAdrenoMetal: opened=%u closed=%u failed=%u faults=%llu suspends=%llu objects=%zu spans=%zu jobs=%zu poisoned_vm=%u poisoned_submit=%u poisoned_ring=%u\n",
            opened,closed,unsigned(failed),faults,suspends,objects,spans,jobs,
            unsigned(vmPoisoned),unsigned(submitPoisoned),unsigned(ringPoisoned));
        IOLog("NativeAdrenoMetal: submissions=%u completed=%u timeouts=%u poisoned_jobs=%u held=%zu retired=%zu peak_jobs=%zu submitted_fence=%u completed_fence=%u\n",
            submit.submissions,submit.completed,submit.timeouts,submit.poisoned_jobs,
            submit.references_held,submit.references_retired,submit.peak_jobs,submitted,completed);
    } else {
        IOLog("NativeAdrenoMetal: submission_sample=%s\n",g.published?"lock_busy":"not_published");
    }
    IOLog("NativeAdrenoMetal: end\n");
}

extern "C" void native_adreno_fault(void)
{
    if(!g.published || !g.lock) {
        return;
    }
    IOLockLock(g.lock);
    if(!g.failed) {
        g.failed=true;
        g.status.noteFault();
        g.ring->recordFault();
    }
    IOLockUnlock(g.lock);
}
extern "C" int native_adreno_progress(uint32_t *submitted,uint32_t *completed)
{
    if(!g.published || !g.lock || !submitted || !completed) {
        return 1;
    }
    IOLockLock(g.lock);
    IOReturn status=g.failed?kIOReturnNotReady:g.submit->retireCompleted();
    *submitted=g.ring->lastSubmitted();
    uint32_t observed=0;
    if(!status && *submitted) {
        const NativeAdrenoRingFence result=g.ring->observeFence(*submitted,nullptr,0,&observed);
        if(result==NativeAdrenoRingFencePoisoned) {
            status=kIOReturnIOError;
        }
    }
    *completed=observed;
    IOLockUnlock(g.lock);
    return status?1:0;
}
extern "C" IOService *native_adreno_published_service()
{
    return g.service;
}

static IOReturn pollSurfaceProducerLocked(uint32_t fence)
{
    if(g.failed || g.vm->poisoned() || g.submit->poisoned() || g.ring->poisoned()) {
        return kIOReturnIOError;
    }

    if(fence) {
        const NativeAdrenoRingFence observed=g.ring->wait(fence,nullptr,0,nullptr);
        if(observed==NativeAdrenoRingFenceTimeout) {
            return kIOReturnTimeout;
        }
        if(observed!=NativeAdrenoRingFenceComplete) {
            return kIOReturnIOError;
        }
        const IOReturn status=g.submit->retireCompleted();
        if(status) {
            return status;
        }
    }

    __asm__ volatile("dsb sy" ::: "memory");
    return kIOReturnSuccess;
}

extern "C" IOReturn native_adreno_capture_surface_producer(uint32_t *captured_fence)
{
    if(!captured_fence) {
        return kIOReturnBadArgument;
    }
    *captured_fence=0;
    if(!g.published) {
        return kIOReturnSuccess;
    }
    if(!g.lock) {
        return kIOReturnNotReady;
    }
    if(!IOLockTryLock(g.lock)) {
        return kIOReturnTimeout;
    }

    IOReturn status=kIOReturnSuccess;
    if(g.failed || g.vm->poisoned() || g.submit->poisoned() || g.ring->poisoned()) {
        status=kIOReturnIOError;
    } else {
        *captured_fence=g.ring->lastSubmitted();
    }
    IOLockUnlock(g.lock);
    return status;
}

extern "C" IOReturn native_adreno_poll_surface_producer(uint32_t captured_fence)
{
    if(!g.published) {
        return captured_fence?kIOReturnNotReady:kIOReturnSuccess;
    }
    if(!g.lock) {
        return kIOReturnNotReady;
    }
    if(!IOLockTryLock(g.lock)) {
        return kIOReturnTimeout;
    }

    const IOReturn status=pollSurfaceProducerLocked(captured_fence);
    IOLockUnlock(g.lock);
    return status;
}

extern "C" IOReturn native_adreno_wait_surface_producer(uint32_t timeout_us,
    uint32_t *captured_fence)
{
    if(captured_fence) {
        *captured_fence=0;
    }
    if(!g.published) {
        return kIOReturnSuccess;
    }
    if(!g.lock || !g.clock.now_us) {
        return kIOReturnNotReady;
    }

    const NativeAdrenoRingClock clock=g.clock;
    const uint64_t began=clock.now_us(clock.cookie);
    uint32_t fence=0;
    bool captured=false;
    for(;;) {
        IOReturn status=kIOReturnTimeout;
        if(IOLockTryLock(g.lock)) {
            if(!captured) {
                fence=g.ring->lastSubmitted();
                captured=true;
                if(captured_fence) {
                    *captured_fence=fence;
                }
            }

            status=pollSurfaceProducerLocked(fence);
            IOLockUnlock(g.lock);
        }

        if(status!=kIOReturnTimeout || clock.now_us(clock.cookie)-began>=timeout_us) {
            return status;
        }
        IOSleep(1);
    }
}
