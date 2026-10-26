/* SPDX-License-Identifier: GPL-2.0-only */
#ifndef NATIVE_ADRENO_A6XX_GMU_H
#define NATIVE_ADRENO_A6XX_GMU_H

#include "NativeAdrenoA6xxPower.h"
#include <IOKit/IOLocks.h>

struct NativeAdrenoA6xxGmuBuffer {
    void *cpu;
    uint64_t iova;
    size_t bytes;
};

struct NativeAdrenoA6xxGmuConfig {
    NativeAdrenoA6xxIo io;
    const uint8_t *firmware;
    size_t firmwareBytes;
    NativeAdrenoA6xxGmuBuffer buffers[5]; /* icache, dummy, debug, log, HFI */
    const NativeAdrenoA6xxTables *tables;
};

/* NativeAdrenoA6xxGmu boots the A660 GMU firmware on SM7325. The caller first
 * powers CX, programs the GMU SMMU context, and maps the five uncached buffers.
 * start holds CM3 in reset while loading its TCM and mapped firmware segments,
 * initializes the A6xx HFI queues, configures the A660 RSCC program and the
 * AOP-owned PDC handoff, then requires real firmware acknowledgements for the
 * performance table, bandwidth table and core start. The HFI startup vote selects
 * 315 MHz and bandwidth entry three.
 * poll reads the GMU fault registers and consumes bounded
 * response/debug queue contents while the service owns the running device.
 * The caller owns GPU/SMMU setup and retains every buffer until stop has
 * halted CM3 and GPU DMA has separately stopped. */
class NativeAdrenoA6xxGmu final {
    NativeAdrenoA6xxGmuConfig config_{};
    NativeAdrenoA6xxReport report_{};
    uint32_t sequence_ = 0;
    IOLock *lock_ = nullptr;
    bool attempted_ = false;
    bool running_ = false;

    IOReturn read(uint32_t, uint32_t, uint32_t *);
    IOReturn write(uint32_t, uint32_t, uint32_t);
    IOReturn gmuWrite(uint32_t, uint32_t);
    IOReturn poll(uint32_t, uint32_t, uint32_t, uint32_t, uint32_t);
    IOReturn loadFirmware();
    IOReturn initializeQueues();
    IOReturn configureRegisters();
    IOReturn configureSequencers();
    IOReturn drainQueue(unsigned, uint32_t, bool * = nullptr, uint32_t * = nullptr);
    IOReturn clearQueueInterrupts();
    IOReturn exchange(uint32_t, const uint32_t *, size_t, uint32_t * = nullptr);
    IOReturn health();
    void captureFaultQueue();
    void logGpuAccessState(const char *) const;

public:
    enum Phase : uint32_t {
        None, Reset, Firmware, Queues, Registers, FirmwareBoot, Sequencers,
        HfiBoot, HfiVersion, PerformanceTable, BandwidthTable, Features,
        CoreStart, PerformanceVote, GpuWake, Ready, Stop, Runtime
    };

    IOReturn start(const NativeAdrenoA6xxGmuConfig &);
    IOReturn keepAlive(bool);
    IOReturn poll();
    IOReturn stop();
    bool running() const { return running_; }
    const NativeAdrenoA6xxReport &report() const { return report_; }
};

#endif
