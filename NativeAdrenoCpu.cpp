/* SPDX-License-Identifier: MIT */
#include "NativeAdrenoMemory.h"
#include <kern/cpu_data.h>
#include <vm/vm_map.h>
#include <mach/vm_param.h>
#include <vm/memory_types.h>
#include <pexpert/pexpert.h>
extern "C" uint64_t gPhysBase,gPhysSize;
extern "C" unsigned int pmap_cache_attributes(ppnum_t pn);
/* The board diagnostic ABI retains these zero-valued counters. */
extern "C" {volatile uint64_t native_adreno_cache_stats[5];}
extern "C" {volatile uint64_t native_adreno_reassert_ticks;}

IOReturn NativeAdrenoQueryCpu4K(NativeAdrenoCpuState *out)
{
    if(!out)return kIOReturnBadArgument;
    *out={};
    disable_preemption();
    __asm__ volatile("mrs %0, tcr_el1" : "=r"(out->tcr));
    __asm__ volatile("mrs %0, ttbr0_el1" : "=r"(out->ttbr0));
    __asm__ volatile("mrs %0, ttbr1_el1" : "=r"(out->ttbr1));
    __asm__ volatile("mrs %0, sctlr_el1" : "=r"(out->sctlr));
    out->kernel_page_bytes=page_size;
    out->kernel_map_page_bytes=vm_map_page_size(kernel_map);
    enable_preemption();
    bool native=PAGE_SIZE==4096 && out->kernel_page_bytes==4096 && out->kernel_map_page_bytes==4096 &&
        ((out->tcr>>14)&3)==0 && ((out->tcr>>30)&3)==2 &&
        (out->sctlr&1) && (out->ttbr1&UINT64_C(0x0000fffffffff000));
    return native?kIOReturnSuccess:kIOReturnUnsupported;
}
NativeAdrenoCpuCacheMode NativeAdrenoQueryPageCacheMode(uint64_t physical)
{
    if((physical&4095) || physical<gPhysBase || physical-gPhysBase>=gPhysSize ||
        gPhysSize-(physical-gPhysBase)<4096) {
        return NativeAdrenoCpuCacheMode::Unknown;
    }

    const unsigned attribute=pmap_cache_attributes((ppnum_t)(physical>>12))&VM_WIMG_MASK;
    switch(attribute) {
        case VM_WIMG_COPYBACK:
        case VM_WIMG_INNERWBACK:
        case VM_WIMG_WTHRU:
            return NativeAdrenoCpuCacheMode::Cached;
        case VM_WIMG_WCOMB:
            return NativeAdrenoCpuCacheMode::NormalUncached;
        case VM_WIMG_RT: {
            int managed_rt_normal=0;
            PE_parse_boot_argn("t733-managed-rt-normal",&managed_rt_normal,sizeof(managed_rt_normal));
            if(managed_rt_normal==1) {
                return NativeAdrenoCpuCacheMode::NormalUncached;
            }
            return NativeAdrenoCpuCacheMode::Unknown;
        }
        default:
            return NativeAdrenoCpuCacheMode::Unknown;
    }
}
