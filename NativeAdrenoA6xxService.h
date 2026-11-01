/* SPDX-License-Identifier: MIT */
#ifndef NATIVE_ADRENO_A6XX_SERVICE_H
#define NATIVE_ADRENO_A6XX_SERVICE_H
#include <stdint.h>
#include <IOKit/IOTypes.h>

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
    NativeAdrenoA6xxWindow smmuTcu;
    NativeAdrenoA6xxWindow smmuTbu[2];
    NativeAdrenoA6xxWindow smmuTbuStatus[2];
    uint32_t smmuTbuSidStart[2];
    uint32_t smmuTbuSidCount[2];
    uint32_t smmuGlobalInterrupt; // Raw GIC INTID, including the SPI base of 32.
    uint32_t gpuInterrupt; // CP CACHE_FLUSH_TS and GPU fault interrupt.
    uint32_t smmuContextInterrupts[10]; // Hardware context-bank order.
    uint32_t chipId,gmemBytes,frequencyHz,highestBankBit;
    uint32_t ddrType; // Firmware-reported DDR type selects the stock bandwidth table.
    uint32_t gpuBank,gpuAsid,gpuSlot,gpuSid,gpuSidMask;
    uint32_t gmuBank,gmuAsid,gmuSlot,gmuSid,gmuSidMask;
    uint32_t lpacBank,lpacAsid,lpacSlot,lpacSid,lpacSidMask;
};

extern "C" int native_adreno_a6xx_resources(NativeAdrenoA6xxResources *);
/* The board returns its current GPU frequency cap and both TSENS readings'
 * validity. The temperature is the hottest valid GPU sensor, in millidegrees C. */
extern "C" uint32_t native_adreno_thermal_cap_hz(int32_t *temperatureMilliC,
    uint32_t *validMask, uint32_t *flags);
/* Registration keeps the GIC line disabled. The callback runs on the boot CPU
 * in primary interrupt context before EOI and must acknowledge its source.
 * The callback and context remain owned by the caller for the boot lifetime.
 */
extern "C" IOReturn native_adreno_smmu_register_irq(uint32_t interrupt,
    void (*callback)(void *), void *context);
extern "C" IOReturn native_adreno_smmu_enable_irq(uint32_t interrupt);
extern "C" IOReturn native_adreno_smmu_disable_irq(uint32_t interrupt);
extern "C" void native_adreno_gpu_interrupt(bool failed);
extern "C" void native_adreno_dma_stopped(void);
extern "C" void native_adreno_a6xx_start(void);
extern "C" void native_adreno_a6xx_dump_stats(void);
extern "C" IOReturn native_adreno_a6xx_request_start(void);
extern "C" IOReturn native_adreno_control_publish(void);
extern "C" void native_adreno_control_update(uint32_t step,uint32_t status,const char *state);

#endif
