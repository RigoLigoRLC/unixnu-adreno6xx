/* SPDX-License-Identifier: GPL-2.0-only */
#ifndef NATIVE_ADRENO_A6XX_POWER_H
#define NATIVE_ADRENO_A6XX_POWER_H

#include <IOKit/IOTypes.h>
#include <stddef.h>
#include <stdint.h>

enum NativeAdrenoA6xxDomain : uint32_t {
    NativeAdrenoA6xxGpu,
    NativeAdrenoA6xxGmuRegisters,
    NativeAdrenoA6xxGpuCc,
    NativeAdrenoA6xxGcc,
    NativeAdrenoA6xxPdc,
    NativeAdrenoA6xxRscc,
    NativeAdrenoA6xxCommandDb,
    NativeAdrenoA6xxRpmh,
    NativeAdrenoA6xxAop,
    NativeAdrenoA6xxIpcc,
    NativeAdrenoA6xxCxMisc,
    NativeAdrenoA6xxDomains
};

/* NativeAdrenoA6xxIo connects the SM7325 register procedures to mappings owned
 * by NativeAdrenoService. Offsets are bytes within the selected window. The
 * callbacks reject an unaligned access or an access beyond the mapped window.
 * The service retains every mapping and all DMA allocations after a failure. */
struct NativeAdrenoA6xxIo {
    void *cookie;
    int (*read32)(void *, uint32_t domain, uint32_t byteOffset, uint32_t *);
    int (*write32)(void *, uint32_t domain, uint32_t byteOffset, uint32_t);
    void (*delayUs)(void *, uint32_t);
};

struct NativeAdrenoA6xxReport {
    uint32_t phase;
    uint32_t domain;
    uint32_t offset;
    uint32_t value;
    uint32_t expected;
    uint32_t mask;
    IOReturn status;
    uint32_t firmwareCore;
    uint32_t hfiVersion;
    uint32_t hfiMessage;
    uint32_t hfiError;
    uint32_t acknowledged;
    uint32_t hfiQueue;
    uint32_t hfiHeader;
    char hfiErrorText[65];
    uint32_t gmuInitResult;
    uint32_t gmuInterrupts;
    uint32_t gmuAoInterrupts;
    uint32_t gmuFenceStatus;
};

/* NativeAdrenoA6xxOperatingPoint pairs each SM7325 speed-bin 117 frequency
 * with its command-db voltage level and DDR7 bandwidth-table index. */
struct NativeAdrenoA6xxOperatingPoint {
    uint32_t frequencyHz;
    uint32_t arcLevel;
    uint32_t bandwidthIndex;
};

constexpr unsigned NativeAdrenoA6xxOperatingPointCount = 3;
constexpr NativeAdrenoA6xxOperatingPoint NativeAdrenoA6xxOperatingPoints[] = {
    {315000000, 64, 3},
    {450000000, 128, 6},
    {550000000, 192, 9}
};

/* NativeAdrenoA6xxTables contains the A660 HFI wire payloads. GPU entry zero
 * requests off; entries one through three request 315, 450 and 550 MHz. GMU entry one
 * requests 200 MHz at MIN_SVS. Power builds the ARC indexes and BCM commands
 * from the running SM7325 command database before changing a power register. */
struct NativeAdrenoA6xxTables {
    uint32_t performance[58];
    uint32_t bandwidth[159];
    uint32_t cxAddress;
    uint32_t mxAddress;
    uint32_t gfxAddress;
    uint32_t xoAddress;
    uint32_t cxIndex;
    uint32_t mxIndex;
    uint32_t gfxIndex;
};

/* NativeAdrenoA6xxPower owns the host votes that keep the GMU and SMMU alive.
 * enableCx reads command-db, disables ACD through AOP QMP, raises the parent
 * rails through an unused APPS DRV2 active TCS, and enables the Yupik clocks
 * and CX domain. The GMU subsequently controls the GPU clock through
 * HFI. enableGx observes the GX power, GMU fence and GX clock state after the
 * GMU wake request. stop requires the caller to have stopped GPU and GMU DMA
 * first; host votes and mappings remain owned throughout this one-shot run. */
class NativeAdrenoA6xxPower final {
    enum BranchRule : unsigned { BranchHalt, BranchDelay, BranchSkip, BranchVoted };

    NativeAdrenoA6xxIo io_{};
    NativeAdrenoA6xxTables tables_{};
    NativeAdrenoA6xxReport report_{};
    bool cxReady_ = false;
    bool gxReady_ = false;

    IOReturn read(uint32_t, uint32_t, uint32_t *);
    IOReturn write(uint32_t, uint32_t, uint32_t);
    IOReturn update(uint32_t, uint32_t, uint32_t, uint32_t);
    IOReturn poll(uint32_t, uint32_t, uint32_t, uint32_t, uint32_t);
    IOReturn buildTables();
    IOReturn disableAcd();
    IOReturn voteRails();
    IOReturn branch(uint32_t, uint32_t, uint32_t, BranchRule, uint32_t = 0);
    IOReturn setSource(uint32_t, uint32_t);

public:
    enum Phase : uint32_t {
        None, CommandDatabase, AopAcd, ParentVotes, GccClocks, CxDomain,
        GmuClock, HubClock, GpuCcClocks, CxReady, GxDomain, GxReady, Stopped
    };

    IOReturn enableCx(const NativeAdrenoA6xxIo &, uint32_t ddrType = 7);
    IOReturn enableGx();
    IOReturn stop(bool dmaStopped);
    const NativeAdrenoA6xxTables &tables() const { return tables_; }
    const NativeAdrenoA6xxReport &report() const { return report_; }
};

#endif
