// SPDX-License-Identifier: MIT
// AGXAccelerator exposes the running Adreno service through the original
// IOAcceleratorES ancestry. Metal reads its plugin properties and constructs
// NativeAdrenoMetalDevice; GPU connections retain the existing BO and submit ABI.
#include "OriginalAcceleratorABI.h"
#include <IOKit/IOLib.h>
#include <IOKit/IOUserClient.h>
#include <libkern/c++/OSSymbol.h>
#include <mach/kmod.h>

extern "C" IOReturn native_adreno_new_user_client(IOService *,task_t,void *,UInt32,OSDictionary *,IOUserClient **,unsigned);
extern "C" boolean_t PE_parse_boot_argn(const char *,void *,int);
extern "C" void native_adreno_note_property_read(const char *);
extern "C" kern_return_t OSKextLoadKextWithIdentifier(const char *);
extern "C" kmod_info_t g_kernel_kmod_info;
extern "C" void native_adreno_accelerator_static_init(void)
    __asm__("__GLOBAL__sub_I_NativeAdrenoAccelerator.cpp");

class AGXAccelerator : public IOAcceleratorES {
    OSDeclareDefaultStructors(AGXAccelerator)
public:
    using IORegistryEntry::copyProperty;

    IOReturn newUserClient(task_t owningTask,void *securityID,UInt32 type,OSDictionary *properties,
        IOUserClient **handler) APPLE_KEXT_OVERRIDE;
    OSObject *copyProperty(const OSSymbol *key) const APPLE_KEXT_OVERRIDE;
    OSObject *copyProperty(const OSString *key) const APPLE_KEXT_OVERRIDE;
    OSObject *copyProperty(const char *key) const APPLE_KEXT_OVERRIDE;
};
OSDefineMetaClassAndStructors(AGXAccelerator,IOAcceleratorES)

IOReturn AGXAccelerator::newUserClient(task_t owningTask,void *securityID,UInt32 type,OSDictionary *properties,
    IOUserClient **handler)
{
    unsigned limit=40;
    PE_parse_boot_argn("native-adreno-metal-opens",&limit,sizeof(limit));
    return native_adreno_new_user_client(this,owningTask,securityID,type,properties,handler,limit);
}
OSObject *AGXAccelerator::copyProperty(const OSSymbol *key) const
{
    native_adreno_note_property_read(key?key->getCStringNoCopy():nullptr);
    return IORegistryEntry::copyProperty(key);
}
OSObject *AGXAccelerator::copyProperty(const OSString *key) const
{
    native_adreno_note_property_read(key?key->getCStringNoCopy():nullptr);
    return IORegistryEntry::copyProperty(key);
}
OSObject *AGXAccelerator::copyProperty(const char *key) const
{
    native_adreno_note_property_read(key);
    return IORegistryEntry::copyProperty(key);
}

/* The accelerator is retained for the lifetime of its running GPU provider.
 * Its class initializer runs only after the original parent has registered. */
extern "C" IOReturn native_adreno_accelerator_publish(IOService *provider,uint32_t chipId)
{
    static AGXAccelerator *accelerator=nullptr;
    static bool initialized=false;
    if(!provider || accelerator) {
        return kIOReturnBadArgument;
    }

    if(!initialized) {
        const kern_return_t load=OSKextLoadKextWithIdentifier("com.apple.iokit.IOGPUFamily");
        if(load!=KERN_SUCCESS) {
            IOLog("NativeAdrenoA6xx: original IOGPUFamily load failed %08x\n",load);
            return kIOReturnNotReady;
        }

        const OSSymbol *name=OSSymbol::withCString("IOAcceleratorES");
        const OSMetaClass *meta=name?OSMetaClass::getMetaClassWithName(name):nullptr;
        const bool compatible=meta && meta->getClassSize()==sizeof(IOAcceleratorES);
        OSSafeReleaseNULL(name);
        if(!compatible) {
            return kIOReturnUnsupported;
        }

        void *handle=OSMetaClass::preModLoad(g_kernel_kmod_info.name);
        if(!handle) {
            return kIOReturnNoResources;
        }
        native_adreno_accelerator_static_init();
        const OSReturn status=OSMetaClass::postModLoad(handle);
        if(status!=kOSReturnSuccess) {
            return status;
        }
        initialized=true;
    }

    AGXAccelerator *service=new AGXAccelerator;
    if(!service || !service->init()) {
        OSSafeReleaseNULL(service);
        return kIOReturnNoMemory;
    }
    service->setName("AGXAccelerator");
    if(!service->setProperty("MetalPluginName","NativeAdrenoMetal") ||
       !service->setProperty("MetalPluginClassName","NativeAdrenoMetalDevice") ||
       !service->setProperty("NativeSandboxGrantProcess","backboardd,iconservicesagent,KaleidoscopePoster,AegirPoster") ||
       !service->setProperty("ChipId",chipId,32)) {
        service->release();
        return kIOReturnNoMemory;
    }

    if(!service->attach(provider)) {
        service->release();
        return kIOReturnNoResources;
    }
    if(!service->start(provider)) {
        service->detach(provider);
        service->release();
        return kIOReturnNotReady;
    }

    accelerator=service;
    service->registerService();
    IOLog("NativeAdrenoA6xx: Metal accelerator published chip=%08x plugin=NativeAdrenoMetal\n",chipId);
    return kIOReturnSuccess;
}
