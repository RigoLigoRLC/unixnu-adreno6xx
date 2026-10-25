/* SPDX-License-Identifier: MIT */
#ifndef NATIVE_ADRENO_A6XX_SERVICE_H
#define NATIVE_ADRENO_A6XX_SERVICE_H
#include <stdint.h>

/* Each window contains the physical range supplied by the board's ADT node. */
struct NativeAdrenoA6xxWindow {
    uint64_t physical;
    uint64_t bytes;
};

/* The SM7325 board supplies windows and stream ownership before GPU startup. */
struct NativeAdrenoA6xxResources {
    NativeAdrenoA6xxWindow gpu,gmu,smmu,gpucc,gcc,pdc,rscc;
    NativeAdrenoA6xxWindow commandDb,rpmh,aop,ipcc,cxMisc;
    NativeAdrenoA6xxWindow zap;
    uint32_t chipId,gmemBytes,frequencyHz,highestBankBit;
    uint32_t gpuBank,gpuAsid,gpuSlot,gpuSid,gpuSidMask;
    uint32_t gmuBank,gmuAsid,gmuSlot,gmuSid,gmuSidMask;
};

extern "C" int native_adreno_a6xx_resources(NativeAdrenoA6xxResources *);
extern "C" void native_adreno_a6xx_start(void);
extern "C" void native_adreno_a6xx_dump_stats(void);

#endif
