/* SPDX-License-Identifier: MIT */
#include "NativeAdrenoGpuProfile.h"
#include "NativeAdrenoKernelTime.h"
#include <IOKit/IOLib.h>
#include <IOKit/IOLocks.h>
#include <kern/clock.h>

namespace {
constexpr unsigned CounterCount = 29;
constexpr unsigned SampleCapacity = 4096;
constexpr unsigned JobCapacity = 4096;
constexpr uint64_t SampleIntervalUs = 20000;

/* Counter describes one kernel-owned A6xx selector and its 64-bit register.
 * Register addresses and load bits follow Samsung's adreno_a6xx_perfcounter.c;
 * event numbers follow Mesa's a6xx_perfcntrs.xml. CP0 and SP0 remain reserved. */
struct Counter {
    const char *name;
    const char *unit;
    uint32_t selector;
    uint32_t event;
    uint32_t low;
    uint32_t loadBit;
};

constexpr Counter counters[CounterCount] = {
    {"core", "cycles", 0x50a, 0, 0x422, 17},
    {"sp_busy", "cycles", 0xae11, 0, 0x4a8, 84},
    {"sp_alu", "cycles", 0xae12, 1, 0x4aa, 85},
    {"vs_alu", "instructions", 0xae13, 35, 0x4ac, 86},
    {"fs_alu", "instructions", 0xae14, 40, 0x4ae, 87},
    {"fs_tex", "instructions", 0xae15, 37, 0x4b0, 88},
    {"vs_tex", "instructions", 0xae16, 33, 0x4b2, 89},
    {"sp_stall_tp", "cycles", 0xae17, 4, 0x4b4, 90},
    {"sp_stall_uche", "cycles", 0xae18, 5, 0x4b6, 91},
    {"sp_stall_rb", "cycles", 0xae19, 6, 0x4b8, 92},
    {"tp_busy", "cycles", 0xb61a, 0, 0x4a2, 81},
    {"tp_stall_uche", "cycles", 0xb61b, 1, 0x4a4, 82},
    {"uche_busy", "cycles", 0xe1c, 0, 0x476, 59},
    {"uche_stall_arbiter", "cycles", 0xe1d, 1, 0x478, 60},
    {"uche_read_tp", "beats", 0xe1e, 4, 0x47a, 61},
    {"uche_read_sp", "beats", 0xe1f, 8, 0x47c, 62},
    {"uche_write_sp", "requests", 0xe20, 15, 0x47e, 63},
    {"uche_gmem_read", "beats", 0xe21, 29, 0x480, 64},
    {"uche_vbif_latency_cycles", "cycles", 0xe22, 2, 0x482, 65},
    {"uche_vbif_latency_samples", "samples", 0xe23, 3, 0x484, 66},
    {"rb_busy", "cycles", 0x8e16, 0, 0x4e2, 113},
    {"rb_stall_ccu", "cycles", 0x8e17, 44, 0x4e4, 114},
    {"ccu_gmem_read", "events", 0x8e1b, 9, 0x462, 49},
    {"ccu_gmem_write", "events", 0x8e1c, 10, 0x464, 50},
    {"cp_busy", "cycles", 0x8d1, 2, 0x402, 1},
    {"cp_wfi_stall", "cycles", 0x8d2, 24, 0x404, 2},
    {"cp_sync_stall", "cycles", 0x8d3, 23, 0x406, 3},
    {"fs_stage_duration", "cycles", 0xae1a, 14, 0x4ba, 93},
    {"vs_stage_duration", "cycles", 0xae1b, 15, 0x4bc, 94}
};

struct CounterSample {
    uint64_t begin = 0;
    uint64_t end = 0;
    uint64_t values[CounterCount]{};
    uint32_t validMask = 0;
    uint64_t cpuBeginNs = 0;
    uint64_t cpuEndNs = 0;
};

struct JobSample {
    uint32_t fence = 0;
    uint32_t queue = 0;
    uint32_t issue = 0;
    uint32_t cp = 0;
    uint32_t rb = 0;
    uint32_t queueValid = 0;
    uint32_t rbValid = 0;
    uint64_t completion = 0;
    uint64_t retiredNs = 0;
};

/* TraceProducer retains a connection's right to append a capture's completed
 * user events. End or connection close releases the right before Seal. */
struct TraceProducer {
    uint64_t owner = 0;
    uint64_t identifier = 0;
    uint32_t source = 0;
};

NativeAdrenoRingIo profileIo{};
NativeAdrenoGpuProfileAccess profileAccess{};
IOLock *profileLock = nullptr;
CounterSample samples[SampleCapacity];
JobSample jobs[JobCapacity];
NativeAdrenoTraceEvent traceEvents[NativeAdrenoTraceEventCapacity];
TraceProducer traceProducers[16];
uint64_t nextProducer = 0;
uint64_t startedNs = 0;
uint64_t stoppedNs = 0;
uint32_t traceEventCount = 0;
uint32_t traceEventDropped = 0;
uint32_t producerDropped = 0;
uint32_t producerCount = 0;
uint32_t producerFailed = 0;
bool traceSealed = false;
bool traceStreaming = false;
uint64_t traceFirstSequence = 0;
uint64_t traceWriteSequence = 0;
uint64_t traceAckSequence = 0;
uint64_t traceReadSequence = 0;
uint32_t initialized = 0;
uint32_t running = 0;
uint32_t workerNeeded = 0;
uint32_t window = 0;
uint32_t sampleCount = 0;
uint32_t jobCount = 0;
uint32_t pendingJobs = 0;
uint32_t sampleDropped = 0;
uint32_t jobDropped = 0;
uint32_t requestedDurationMs = 0;
uint32_t accessError = 0;
uint64_t deadlineUs = 0;
uint64_t nextSampleUs = 0;
bool requested = false;
bool acquiring = false;
bool releasing = false;
bool hardwareOwned = false;
bool stopRequested = false;
bool sampling = false;
bool dumping = false;
bool dumpBegun = false;
uint32_t dumpSampleAt = 0;
uint32_t dumpJobAt = 0;

uint64_t nowUs()
{
    uint64_t value = 0;
    NativeAdrenoKernelTime::now_us(nullptr, &value);

    return value;
}

uint64_t nowNs()
{
    uint64_t value = 0;
    absolutetime_to_nanoseconds(mach_absolute_time(), &value);

    return value;
}

bool hardwareComplete()
{
    return window && !running && !requested && !acquiring && !releasing &&
        !hardwareOwned && !sampling && !pendingJobs;
}

void fillTraceStatus(NativeAdrenoTraceStatus &status)
{
    status = {};
    status.version = NativeAdrenoTraceVersion;
    status.generation = window;
    status.state = !window ? NativeAdrenoTraceIdle :
        traceSealed ? NativeAdrenoTraceSealed :
        requested || acquiring ? NativeAdrenoTraceRequested :
        running ? NativeAdrenoTraceRunning :
        hardwareComplete() ? NativeAdrenoTraceComplete : NativeAdrenoTraceDraining;
    status.access_error = accessError;
    status.started_ns = startedNs;
    status.stopped_ns = stoppedNs;
    status.counter_count = CounterCount;
    status.sample_count = sampleCount;
    status.job_count = jobCount;
    status.event_count = traceEventCount;
    status.sample_dropped = sampleDropped;
    status.job_dropped = jobDropped;
    status.event_dropped = traceEventDropped;
    status.producer_dropped = producerDropped;
    status.pending_jobs = pendingJobs;
    status.hardware_owned = hardwareOwned;
    status.gpu_clock_hz = NativeAdrenoTraceGpuClockHz;
    status.sample_interval_us = SampleIntervalUs;
    status.sample_capacity = SampleCapacity;
    status.job_capacity = JobCapacity;
    status.event_capacity = NativeAdrenoTraceEventCapacity;
    status.event_bytes = sizeof(NativeAdrenoTraceEvent);
    status.sample_bytes = sizeof(NativeAdrenoTraceCounterSample);
    status.job_bytes = sizeof(NativeAdrenoTraceJob);
    status.definition_bytes = sizeof(NativeAdrenoTraceCounterDefinition);
    status.producer_count = producerCount;
    status.producer_failed = producerFailed;
    status.streaming = traceStreaming;
    status.retained_events = traceStreaming ? uint32_t(traceWriteSequence - traceFirstSequence) : traceEventCount;
}

void addDropped(uint32_t &total, uint32_t count)
{
    total = count > UINT32_MAX - total ? UINT32_MAX : total + count;
}

bool read64(uint32_t reg, uint64_t *value)
{
    for (unsigned attempt = 0; attempt < 4; ++attempt) {
        uint32_t before = 0;
        uint32_t low = 0;
        uint32_t after = 0;
        if (profileIo.read32(profileIo.cookie, reg + 1, &before) ||
            profileIo.read32(profileIo.cookie, reg, &low) ||
            profileIo.read32(profileIo.cookie, reg + 1, &after)) {
            return false;
        }

        if (before == after) {
            *value = (uint64_t(after) << 32) | low;

            return true;
        }
    }

    return false;
}

IOReturn configureCounters()
{
    // CP is stopped; only the listed counters receive the zero load value.
    if (!profileAccess.accessible(profileIo.cookie) ||
        profileIo.write32(profileIo.cookie, 0x505, 0) ||
        profileIo.write32(profileIo.cookie, 0x506, 0)) {
        return kIOReturnIOError;
    }
    for (const Counter &counter : counters) {
        if (profileIo.write32(profileIo.cookie, 0x501 + counter.loadBit / 32,
                1u << (counter.loadBit % 32)) ||
            profileIo.write32(profileIo.cookie, counter.selector, counter.event)) {
            return kIOReturnIOError;
        }
    }
    if (profileIo.write32(profileIo.cookie, 0x500, 1) || profileIo.barrier(profileIo.cookie)) {
        return kIOReturnIOError;
    }

    return kIOReturnSuccess;
}

IOReturn releaseFinishedWindow()
{
    IOLockLock(profileLock);
    const uint32_t capturedWindow = window;
    const bool release = hardwareOwned && !__atomic_load_n(&running, __ATOMIC_ACQUIRE) &&
        !sampling && !pendingJobs;
    releasing = release;
    if (!hardwareOwned && !requested && !acquiring) {
        __atomic_store_n(&workerNeeded, 0, __ATOMIC_RELEASE);
    }
    IOLockUnlock(profileLock);
    if (!release) {
        return kIOReturnSuccess;
    }

    const IOReturn status = profileAccess.release(profileIo.cookie);
    IOLockLock(profileLock);
    releasing = false;
    if (!status) {
        hardwareOwned = false;
        if (!stoppedNs) {
            stoppedNs = nowNs();
        }
        __atomic_store_n(&workerNeeded, 0, __ATOMIC_RELEASE);
    } else {
        accessError = uint32_t(status);
    }
    IOLockUnlock(profileLock);

    IOLog("NativeAdrenoGpuProfile: kind=access window=%u phase=release status=%08x owned=%u\n",
        capturedWindow, uint32_t(status), status ? 1u : 0u);

    return status;
}
}

IOReturn NativeAdrenoGpuProfile::initialize(const NativeAdrenoRingIo *io,
    const NativeAdrenoGpuProfileAccess &access)
{
    if (!io || !io->read32 || !io->write32 || !io->barrier || !io->readAlwaysOn ||
        !access.acquire || !access.release || !access.accessible || initialized) {
        return kIOReturnBadArgument;
    }

    profileLock = IOLockAlloc();
    if (!profileLock) {
        return kIOReturnNoMemory;
    }
    profileIo = *io;
    profileAccess = access;

    IOReturn status = profileAccess.acquire(profileIo.cookie);
    if (status) {
        IOLog("NativeAdrenoGpuProfile: kind=error phase=initialize_acquire status=%08x\n",
            uint32_t(status));
        return status;
    }
    status = configureCounters();
    const IOReturn releaseStatus = profileAccess.release(profileIo.cookie);
    if (!status) {
        status = releaseStatus;
    }
    if (status) {
        IOLog("NativeAdrenoGpuProfile: kind=error phase=initialize_configure status=%08x\n",
            uint32_t(status));
        return status;
    }

    __atomic_store_n(&initialized, 1, __ATOMIC_RELEASE);
    IOLog("NativeAdrenoGpuProfile: kind=ready counters=%u clock_hz=19200000 interval_us=20000 sample_capacity=%u job_capacity=%u\n",
        CounterCount, SampleCapacity, JobCapacity);

    return kIOReturnSuccess;
}

IOReturn NativeAdrenoGpuProfile::poll()
{
    if (!__atomic_load_n(&workerNeeded, __ATOMIC_ACQUIRE)) {
        return kIOReturnSuccess;
    }

    IOLockLock(profileLock);
    const uint32_t capturedWindow = window;
    const bool acquire = requested;
    if (acquire) {
        requested = false;
        acquiring = true;
    }
    IOLockUnlock(profileLock);

    // Only the service worker changes GMU ownership; no recorder lock spans MMIO.
    if (acquire) {
        const IOReturn status = profileAccess.acquire(profileIo.cookie);
        const uint64_t started = nowUs();
        IOLockLock(profileLock);
        acquiring = false;
        hardwareOwned = status == kIOReturnSuccess;
        accessError = uint32_t(status);
        if (!status && !stopRequested) {
            startedNs = nowNs();
            nextSampleUs = started;
            deadlineUs = started + uint64_t(requestedDurationMs) * 1000;
            __atomic_store_n(&running, 1, __ATOMIC_RELEASE);
        }
        if (status) {
            __atomic_store_n(&initialized, 0, __ATOMIC_RELEASE);
            __atomic_store_n(&workerNeeded, 0, __ATOMIC_RELEASE);
        }
        IOLockUnlock(profileLock);

        IOLog("NativeAdrenoGpuProfile: kind=access window=%u phase=acquire status=%08x owned=%u\n",
            capturedWindow, uint32_t(status), status ? 0u : 1u);
        if (status) {
            return status;
        }
    }

    const uint64_t current = nowUs();
    IOLockLock(profileLock);
    if (__atomic_load_n(&running, __ATOMIC_ACQUIRE) && current >= deadlineUs) {
        __atomic_store_n(&running, 0, __ATOMIC_RELEASE);
        stoppedNs = nowNs();
    }
    const bool takeSample = __atomic_load_n(&running, __ATOMIC_ACQUIRE) && hardwareOwned &&
        current >= nextSampleUs && !sampling;
    if (takeSample) {
        sampling = true;
        nextSampleUs = current + SampleIntervalUs;
    }
    IOLockUnlock(profileLock);
    if (!takeSample) {
        return releaseFinishedWindow();
    }

    CounterSample sample{};
    sample.cpuBeginNs = nowNs();
    IOReturn sampleStatus = kIOReturnSuccess;
    if (!profileAccess.accessible(profileIo.cookie) ||
        profileIo.readAlwaysOn(profileIo.cookie, &sample.begin)) {
        sampleStatus = kIOReturnNotReady;
    } else {
        // The stock read path reads RBBM values, without rereading unit selectors.
        for (unsigned index = 0; index < CounterCount; ++index) {
            const Counter &counter = counters[index];
            if (!read64(counter.low, &sample.values[index])) {
                sampleStatus = kIOReturnIOError;
                break;
            }

            sample.validMask |= 1u << index;
        }

        if (profileIo.readAlwaysOn(profileIo.cookie, &sample.end)) {
            sample.validMask = 0;
            sampleStatus = kIOReturnIOError;
        }
    }

    sample.cpuEndNs = nowNs();
    IOLockLock(profileLock);
    if (capturedWindow == window) {
        if (sampleCount < SampleCapacity) {
            samples[sampleCount++] = sample;
        } else {
            ++sampleDropped;
            __atomic_store_n(&running, 0, __ATOMIC_RELEASE);
            if (!stoppedNs) {
                stoppedNs = nowNs();
            }
        }
        if (sampleStatus) {
            accessError = uint32_t(sampleStatus);
            __atomic_store_n(&running, 0, __ATOMIC_RELEASE);
            if (!stoppedNs) {
                stoppedNs = nowNs();
            }
        }
    }
    sampling = false;
    IOLockUnlock(profileLock);

    if (sampleStatus) {
        IOLog("NativeAdrenoGpuProfile: kind=error window=%u phase=sample status=%08x\n",
            capturedWindow, uint32_t(sampleStatus));
        return sampleStatus;
    }

    return releaseFinishedWindow();
}

void NativeAdrenoGpuProfile::fault()
{
    if (profileLock) {
        IOLockLock(profileLock);
    }

    __atomic_store_n(&running, 0, __ATOMIC_RELEASE);
    __atomic_store_n(&initialized, 0, __ATOMIC_RELEASE);
    requested = false;
    stopRequested = true;
    if (!stoppedNs) {
        stoppedNs = nowNs();
    }
    if (!accessError) {
        accessError = uint32_t(kIOReturnIOError);
    }

    if (profileLock) {
        IOLockUnlock(profileLock);
    }
}

void NativeAdrenoGpuProfile::hardwareStopped()
{
    if (!profileLock) {
        return;
    }

    IOLockLock(profileLock);
    const uint32_t capturedWindow = window;
    hardwareOwned = false;
    acquiring = false;
    releasing = false;
    __atomic_store_n(&workerNeeded, 0, __ATOMIC_RELEASE);
    IOLockUnlock(profileLock);

    if (capturedWindow) {
        IOLog("NativeAdrenoGpuProfile: kind=access window=%u phase=hardware_stopped owned=0\n",
            capturedWindow);
    }
}

bool NativeAdrenoGpuProfile::active()
{
    return __atomic_load_n(&running, __ATOMIC_ACQUIRE) != 0;
}

uint32_t NativeAdrenoGpuProfile::beginJob()
{
    if (!__atomic_load_n(&running, __ATOMIC_ACQUIRE)) {
        return 0;
    }

    IOLockLock(profileLock);
    uint32_t capturedWindow = 0;
    if (__atomic_load_n(&running, __ATOMIC_ACQUIRE) && hardwareOwned &&
        jobCount + pendingJobs < JobCapacity) {
        capturedWindow = window;
        ++pendingJobs;
    } else if (__atomic_load_n(&running, __ATOMIC_ACQUIRE)) {
        ++jobDropped;
        __atomic_store_n(&running, 0, __ATOMIC_RELEASE);
        stoppedNs = nowNs();
    }
    IOLockUnlock(profileLock);

    return capturedWindow;
}

void NativeAdrenoGpuProfile::finishJob(uint32_t capturedWindow, uint32_t fence, uint32_t queue,
    uint32_t issue, uint32_t cp, uint32_t rb, bool valid, bool issueValid,
    uint64_t completion)
{
    if (!capturedWindow) {
        return;
    }

    const uint32_t queueTicks = cp - issue;
    const uint32_t rbTicks = rb - cp;
    // Stock adreno_dispatch subtracts GMU AO submit_ticks from CP retired ticks.
    const bool rbValid = valid && rbTicks < 0x80000000u;
    const bool queueValid = rbValid && issueValid && queueTicks < 0x80000000u;

    IOLockLock(profileLock);
    if (capturedWindow == window) {
        if (jobCount < JobCapacity) {
            jobs[jobCount++] = {fence, queue, issue, cp, rb,
                queueValid ? 1u : 0u, rbValid ? 1u : 0u, completion, nowNs()};
        } else {
            ++jobDropped;
        }

        if (pendingJobs) {
            --pendingJobs;
        }
    }
    IOLockUnlock(profileLock);
}

static uint32_t startTraceCapture(uint32_t duration_ms, bool streaming)
{
    if (duration_ms < 100 || duration_ms > 60000) {
        return uint32_t(kIOReturnBadArgument);
    }
    if (!__atomic_load_n(&initialized, __ATOMIC_ACQUIRE)) {
        return uint32_t(kIOReturnNotReady);
    }

    IOLockLock(profileLock);
    if (!__atomic_load_n(&initialized, __ATOMIC_ACQUIRE)) {
        IOLockUnlock(profileLock);
        return uint32_t(kIOReturnNotReady);
    }
    if (__atomic_load_n(&workerNeeded, __ATOMIC_ACQUIRE) ||
        __atomic_load_n(&running, __ATOMIC_ACQUIRE) || sampling || dumping || pendingJobs || producerCount ||
        (dumpBegun && (dumpSampleAt < sampleCount || dumpJobAt < jobCount))) {
        IOLockUnlock(profileLock);
        return uint32_t(kIOReturnBusy);
    }

    ++window;
    if (!window) {
        ++window;
    }

    sampleCount = 0;
    jobCount = 0;
    sampleDropped = 0;
    jobDropped = 0;
    traceEventCount = 0;
    traceEventDropped = 0;
    traceStreaming = streaming;
    traceFirstSequence = 0;
    traceWriteSequence = 0;
    traceAckSequence = 0;
    traceReadSequence = 0;
    producerDropped = 0;
    producerFailed = 0;
    startedNs = 0;
    stoppedNs = 0;
    traceSealed = false;
    dumpBegun = false;
    dumpSampleAt = 0;
    dumpJobAt = 0;
    accessError = 0;
    stopRequested = false;
    requestedDurationMs = duration_ms;
    requested = true;
    const uint32_t capturedWindow = window;
    __atomic_store_n(&workerNeeded, 1, __ATOMIC_RELEASE);
    IOLockUnlock(profileLock);

    IOLog("NativeAdrenoGpuProfile: kind=start window=%u duration_ms=%u interval_us=20000\n",
        capturedWindow, duration_ms);

    return uint32_t(kIOReturnSuccess);
}

extern "C" uint32_t native_adreno_gpu_profile_start(uint32_t duration_ms)
{
    return startTraceCapture(duration_ms, false);
}

extern "C" void native_adreno_gpu_profile_stop(void)
{
    if (profileLock) {
        IOLockLock(profileLock);
    }

    __atomic_store_n(&running, 0, __ATOMIC_RELEASE);
    requested = false;
    stopRequested = true;
    if (!stoppedNs) {
        stoppedNs = nowNs();
    }

    if (profileLock) {
        IOLockUnlock(profileLock);
    }
}

extern "C" void native_adreno_gpu_profile_dump(void)
{
    if (!profileLock) {
        IOLog("NativeAdrenoGpuProfile: kind=status window=0 running=0 complete=0 samples=0 jobs=0 pending_jobs=0 sample_dropped=0 job_dropped=0\n");
        return;
    }

    IOLockLock(profileLock);
    const uint32_t capturedWindow = window;
    const uint32_t capturedSamples = sampleCount;
    const uint32_t capturedJobs = jobCount;
    const uint32_t pending = pendingJobs;
    const uint32_t active = __atomic_load_n(&running, __ATOMIC_ACQUIRE);
    const bool requestPending = requested;
    const bool requestAcquiring = acquiring;
    const bool requestReleasing = releasing;
    const bool owned = hardwareOwned;
    const uint32_t error = accessError;
    const uint32_t droppedSamples = sampleDropped;
    const uint32_t droppedJobs = jobDropped;
    const uint32_t firstSample = dumpSampleAt;
    const uint32_t firstJob = dumpJobAt;
    const bool begin = !dumpBegun;
    const bool complete = window && !active && !sampling && !pending && !dumping &&
        !requested && !acquiring && !releasing && !hardwareOwned;
    if (complete) {
        dumping = true;
        dumpBegun = true;
    }
    IOLockUnlock(profileLock);

    IOLog("NativeAdrenoGpuProfile: kind=status window=%u running=%u complete=%u samples=%u jobs=%u pending_jobs=%u sample_dropped=%u job_dropped=%u\n",
        capturedWindow, active || requestPending || requestAcquiring ? 1u : 0u,
        complete ? 1u : 0u, capturedSamples, capturedJobs,
        pending, droppedSamples, droppedJobs);
    IOLog("NativeAdrenoGpuProfile: kind=access window=%u phase=status requested=%u acquiring=%u releasing=%u owned=%u error=%08x\n",
        capturedWindow, requestPending ? 1u : 0u, requestAcquiring ? 1u : 0u,
        requestReleasing ? 1u : 0u, owned ? 1u : 0u, error);
    if (!complete) {
        return;
    }

    if (begin) {
        IOLog("NativeAdrenoGpuProfile: kind=begin window=%u samples=%u jobs=%u counters=%u clock_hz=19200000\n",
            capturedWindow, capturedSamples, capturedJobs, CounterCount);

        for (unsigned index = 0; index < CounterCount; ++index) {
            const Counter &counter = counters[index];
            IOLog("NativeAdrenoGpuProfile: kind=definition window=%u index=%u name=%s selector=%u event=%u low=%u unit=%s\n",
                capturedWindow, index, counter.name, counter.selector, counter.event,
                counter.low, counter.unit);
        }
    }

    const uint32_t untilSample = capturedSamples - firstSample > 64 ? firstSample + 64 : capturedSamples;
    const uint32_t untilJob = capturedJobs - firstJob > 128 ? firstJob + 128 : capturedJobs;
    for (unsigned index = firstSample; index < untilSample; ++index) {
        const CounterSample &sample = samples[index];
        IOLog("NativeAdrenoGpuProfile: kind=sample window=%u seq=%u tick_begin=%llu tick_end=%llu valid_mask=%x\n",
            capturedWindow, index + 1, sample.begin, sample.end, sample.validMask);

        for (unsigned part = 0; part < (CounterCount + 3) / 4; ++part) {
            uint64_t values[4]{};
            for (unsigned at = 0; at < 4; ++at) {
                if (part * 4 + at < CounterCount) {
                    values[at] = sample.values[part * 4 + at];
                }
            }

            IOLog("NativeAdrenoGpuProfile: kind=values window=%u seq=%u part=%u v0=%llu v1=%llu v2=%llu v3=%llu\n",
                capturedWindow, index + 1, part, values[0], values[1], values[2], values[3]);
        }
    }

    for (unsigned index = firstJob; index < untilJob; ++index) {
        const JobSample &job = jobs[index];
        IOLog("NativeAdrenoGpuProfile: kind=job window=%u seq=%u fence=%u queue=%u issue=%u cp=%u rb=%u queue_ticks=%u rb_ticks=%u valid=%u queue_valid=%u rb_valid=%u\n",
            capturedWindow, index + 1, job.fence, job.queue, job.issue, job.cp, job.rb,
            job.queueValid ? job.cp - job.issue : 0, job.rbValid ? job.rb - job.cp : 0,
            job.queueValid && job.rbValid ? 1u : 0u, job.queueValid, job.rbValid);
    }

    if (untilSample == capturedSamples && untilJob == capturedJobs) {
        IOLog("NativeAdrenoGpuProfile: kind=end window=%u samples=%u jobs=%u sample_dropped=%u job_dropped=%u\n",
            capturedWindow, capturedSamples, capturedJobs, droppedSamples, droppedJobs);
    }

    IOLockLock(profileLock);
    dumpSampleAt = untilSample;
    dumpJobAt = untilJob;
    dumping = false;
    IOLockUnlock(profileLock);
}

IOReturn NativeAdrenoGpuProfile::queryTrace(NativeAdrenoTraceStatus *status)
{
    if (!status || !profileLock) {
        return kIOReturnNotReady;
    }

    IOLockLock(profileLock);
    fillTraceStatus(*status);
    IOLockUnlock(profileLock);

    return kIOReturnSuccess;
}

IOReturn NativeAdrenoGpuProfile::controlTrace(const NativeAdrenoTraceControl *request,
    NativeAdrenoTraceStatus *status)
{
    if (!request || !status || request->version != NativeAdrenoTraceVersion || !profileLock) {
        return kIOReturnBadArgument;
    }

    if (request->operation == NativeAdrenoTraceStart ||
        request->operation == NativeAdrenoTraceStartStream) {
        if (request->generation) {
            return kIOReturnBadArgument;
        }

        const IOReturn result = startTraceCapture(request->duration_ms,
            request->operation == NativeAdrenoTraceStartStream);
        queryTrace(status);

        return result;
    }

    IOLockLock(profileLock);
    IOReturn result = kIOReturnSuccess;
    if (!window || request->generation != window || request->duration_ms) {
        result = kIOReturnBadArgument;
    } else if (request->operation == NativeAdrenoTraceStop) {
        __atomic_store_n(&running, 0, __ATOMIC_RELEASE);
        requested = false;
        stopRequested = true;
        if (!stoppedNs) {
            stoppedNs = nowNs();
        }
    } else if (request->operation == NativeAdrenoTraceSeal) {
        if (!hardwareComplete() || producerCount) {
            result = kIOReturnBusy;
        } else {
            traceSealed = true;
        }
    } else {
        result = kIOReturnBadArgument;
    }
    fillTraceStatus(*status);
    IOLockUnlock(profileLock);

    return result;
}

IOReturn NativeAdrenoGpuProfile::registerProducer(uint64_t owner,
    const NativeAdrenoTraceProducer *request, NativeAdrenoTraceProducer *reply)
{
    if (!owner || !request || !reply || !profileLock ||
        request->version != NativeAdrenoTraceVersion || request->producer_id ||
        request->flags || request->dropped || request->reserved ||
        (request->source != NativeAdrenoTraceSourceMetal &&
         request->source != NativeAdrenoTraceSourceTurnip)) {
        return kIOReturnBadArgument;
    }

    IOLockLock(profileLock);
    IOReturn result = kIOReturnNotReady;
    if (request->generation == window && !traceSealed &&
        (running || requested || acquiring)) {
        result = kIOReturnNoResources;
        for (TraceProducer &producer : traceProducers) {
            if (producer.identifier) {
                continue;
            }

            ++nextProducer;
            if (!nextProducer) {
                ++nextProducer;
            }
            producer = {owner, nextProducer, request->source};
            ++producerCount;
            *reply = *request;
            reply->producer_id = nextProducer;
            result = kIOReturnSuccess;
            break;
        }
    }
    IOLockUnlock(profileLock);

    return result;
}

IOReturn NativeAdrenoGpuProfile::appendTrace(uint64_t owner, uint32_t process,
    const NativeAdrenoTraceBatch *batch)
{
    if (!owner || !batch || !profileLock || batch->version != NativeAdrenoTraceVersion ||
        batch->count > NativeAdrenoTraceBatchEvents || batch->reserved) {
        return kIOReturnBadArgument;
    }
    for (uint32_t index = 0; index < batch->count; ++index) {
        const NativeAdrenoTraceEvent &event = batch->events[index];
        if ((event.source != NativeAdrenoTraceSourceMetal &&
             event.source != NativeAdrenoTraceSourceTurnip) ||
            event.phase < NativeAdrenoTracePhaseBegin || event.phase > NativeAdrenoTracePhaseInstant ||
            event.clock_domain < NativeAdrenoTraceClockGpuNanoseconds ||
            event.clock_domain > NativeAdrenoTraceClockMonotonicNanoseconds ||
            event.argument_count > 4 ||
            (event.flags & ~(NativeAdrenoTraceEventInvalid | NativeAdrenoTraceEventSourceFlagsMask))) {
            return kIOReturnBadArgument;
        }
        for (uint32_t argument = 0; argument < event.argument_count; ++argument) {
            if (event.args[argument].type < NativeAdrenoTraceArgumentUnsigned ||
                event.args[argument].type > NativeAdrenoTraceArgumentFloatBits) {
                return kIOReturnBadArgument;
            }
        }
    }

    IOLockLock(profileLock);
    bool ownsProducer = false;
    for (const TraceProducer &producer : traceProducers) {
        if (producer.owner == owner && producer.identifier == batch->producer_id) {
            ownsProducer = true;
            break;
        }
    }
    if (batch->generation != window || traceSealed || !ownsProducer) {
        IOLockUnlock(profileLock);

        return kIOReturnNotReady;
    }

    addDropped(producerDropped, batch->dropped);
    for (uint32_t index = 0; index < batch->count; ++index) {
        if (!traceStreaming && traceEventCount == NativeAdrenoTraceEventCapacity) {
            addDropped(traceEventDropped, batch->count - index);
            break;
        }

        uint32_t destinationIndex = traceEventCount;
        if (traceStreaming) {
            if (traceWriteSequence - traceFirstSequence == NativeAdrenoTraceEventCapacity) {
                ++traceFirstSequence;
                addDropped(traceEventDropped, 1);
            }
            destinationIndex = uint32_t(traceWriteSequence % NativeAdrenoTraceEventCapacity);
            ++traceWriteSequence;
        }
        addDropped(traceEventCount, 1);
        NativeAdrenoTraceEvent &destination = traceEvents[destinationIndex];
        destination = batch->events[index];
        destination.process_id = process;
        destination.name[sizeof(destination.name) - 1] = 0;
    }
    IOLockUnlock(profileLock);

    return kIOReturnSuccess;
}

IOReturn NativeAdrenoGpuProfile::endProducer(uint64_t owner,
    const NativeAdrenoTraceProducer *request)
{
    if (!owner || !request || !profileLock || request->version != NativeAdrenoTraceVersion ||
        !request->producer_id || request->reserved ||
        (request->flags & ~NativeAdrenoTraceProducerIncomplete)) {
        return kIOReturnBadArgument;
    }

    IOLockLock(profileLock);
    IOReturn result = kIOReturnNotFound;
    if (request->generation == window && !traceSealed) {
        for (TraceProducer &producer : traceProducers) {
            if (producer.owner != owner || producer.identifier != request->producer_id) {
                continue;
            }

            addDropped(producerDropped, request->dropped);
            if (request->flags & NativeAdrenoTraceProducerIncomplete) {
                ++producerFailed;
            }
            producer = {};
            --producerCount;
            result = kIOReturnSuccess;
            break;
        }
    }
    IOLockUnlock(profileLock);

    return result;
}

void NativeAdrenoGpuProfile::closeProducers(uint64_t owner)
{
    if (!profileLock) {
        return;
    }

    IOLockLock(profileLock);
    for (TraceProducer &producer : traceProducers) {
        if (producer.owner == owner && producer.identifier) {
            producer = {};
            --producerCount;
            ++producerFailed;
        }
    }
    IOLockUnlock(profileLock);
}

IOReturn NativeAdrenoGpuProfile::readTraceStream(const NativeAdrenoTraceStreamRead *request,
    NativeAdrenoTraceStreamChunk *reply)
{
    if (!request || !reply || !profileLock || request->version != NativeAdrenoTraceVersion) {
        return kIOReturnBadArgument;
    }

    IOLockLock(profileLock);
    if (!traceStreaming || request->generation != window) {
        IOLockUnlock(profileLock);

        return kIOReturnNotReady;
    }
    if (request->next_sequence > traceWriteSequence) {
        IOLockUnlock(profileLock);

        return kIOReturnBadArgument;
    }

    *reply = {};
    reply->version = NativeAdrenoTraceVersion;
    reply->generation = window;
    reply->flags = (traceSealed ? NativeAdrenoTraceStreamSealed : 0) |
        (hardwareComplete() ? NativeAdrenoTraceStreamHardwareComplete : 0) |
        (!producerCount ? NativeAdrenoTraceStreamProducersEnded : 0);
    reply->first_sequence = request->next_sequence < traceFirstSequence ?
        traceFirstSequence : request->next_sequence;
    reply->write_sequence = traceWriteSequence;
    reply->ack_sequence = traceAckSequence;
    const uint64_t missing = request->next_sequence > traceAckSequence ?
        request->next_sequence : traceAckSequence;
    if (missing < traceFirstSequence) {
        reply->lost_begin = missing;
        reply->lost_end = traceFirstSequence;
    }
    const uint64_t available = traceWriteSequence - reply->first_sequence;
    reply->count = available > NativeAdrenoTraceBatchEvents ?
        NativeAdrenoTraceBatchEvents : uint32_t(available);
    reply->next_sequence = reply->first_sequence + reply->count;
    for (uint32_t index = 0; index < reply->count; ++index) {
        const uint64_t sequence = reply->first_sequence + index;
        reply->events[index] = traceEvents[sequence % NativeAdrenoTraceEventCapacity];
    }
    if (reply->next_sequence > traceReadSequence) {
        traceReadSequence = reply->next_sequence;
    }
    IOLockUnlock(profileLock);

    return kIOReturnSuccess;
}

IOReturn NativeAdrenoGpuProfile::ackTraceStream(const NativeAdrenoTraceStreamRead *request)
{
    if (!request || !profileLock || request->version != NativeAdrenoTraceVersion) {
        return kIOReturnBadArgument;
    }

    IOLockLock(profileLock);
    if (!traceStreaming || request->generation != window) {
        IOLockUnlock(profileLock);

        return kIOReturnNotReady;
    }
    if (request->next_sequence > traceReadSequence) {
        IOLockUnlock(profileLock);

        return kIOReturnBadArgument;
    }
    if (request->next_sequence > traceAckSequence) {
        traceAckSequence = request->next_sequence;
        if (traceFirstSequence < traceAckSequence) {
            traceFirstSequence = traceAckSequence;
        }
    }
    IOLockUnlock(profileLock);

    return kIOReturnSuccess;
}

IOReturn NativeAdrenoGpuProfile::readTrace(const NativeAdrenoTraceRead *request,
    NativeAdrenoTraceChunk *reply)
{
    if (!request || !reply || !profileLock || request->version != NativeAdrenoTraceVersion ||
        request->section > NativeAdrenoTraceSectionEvents) {
        return kIOReturnBadArgument;
    }

    IOLockLock(profileLock);
    if ((!traceSealed && !traceStreaming) || request->generation != window) {
        IOLockUnlock(profileLock);

        return kIOReturnNotReady;
    }
    if (traceStreaming && request->section == NativeAdrenoTraceSectionEvents) {
        IOLockUnlock(profileLock);

        return kIOReturnUnsupported;
    }

    const uint32_t sizes[] = {sizeof(NativeAdrenoTraceStatus),
        sizeof(NativeAdrenoTraceCounterDefinition), sizeof(NativeAdrenoTraceCounterSample),
        sizeof(NativeAdrenoTraceJob), sizeof(NativeAdrenoTraceEvent)};
    const uint32_t counts[] = {1, CounterCount, sampleCount, jobCount, traceEventCount};
    const uint32_t recordBytes = sizes[request->section];
    const uint32_t total = recordBytes * counts[request->section];
    if (request->offset > total) {
        IOLockUnlock(profileLock);

        return kIOReturnBadArgument;
    }

    *reply = {};
    reply->version = NativeAdrenoTraceVersion;
    reply->generation = window;
    reply->section = request->section;
    reply->offset = request->offset;
    reply->total_bytes = total;
    reply->bytes = total - request->offset;
    if (reply->bytes > NativeAdrenoTraceChunkBytes) {
        reply->bytes = NativeAdrenoTraceChunkBytes;
    }

    uint32_t written = 0;
    while (written < reply->bytes) {
        const uint32_t position = request->offset + written;
        const uint32_t index = position / recordBytes;
        const uint32_t within = position % recordBytes;
        uint32_t bytes = recordBytes - within;
        if (bytes > reply->bytes - written) {
            bytes = reply->bytes - written;
        }

        union {
            NativeAdrenoTraceStatus status;
            NativeAdrenoTraceCounterDefinition definition;
            NativeAdrenoTraceCounterSample sample;
            NativeAdrenoTraceJob job;
        } record{};
        const uint8_t *source = reinterpret_cast<const uint8_t *>(&record);
        switch (request->section) {
            case NativeAdrenoTraceSectionStatus:
                fillTraceStatus(record.status);
                break;
            case NativeAdrenoTraceSectionDefinitions: {
                const Counter &counter = counters[index];
                strlcpy(record.definition.name, counter.name, sizeof(record.definition.name));
                strlcpy(record.definition.unit, counter.unit, sizeof(record.definition.unit));
                record.definition.index = index;
                record.definition.selector = counter.selector;
                record.definition.event = counter.event;
                record.definition.low_register = counter.low;
                record.definition.load_bit = counter.loadBit;
                break;
            }
            case NativeAdrenoTraceSectionCounters: {
                const CounterSample &sample = samples[index];
                record.sample.cpu_begin_ns = sample.cpuBeginNs;
                record.sample.cpu_end_ns = sample.cpuEndNs;
                record.sample.gpu_begin_ticks = sample.begin;
                record.sample.gpu_end_ticks = sample.end;
                memcpy(record.sample.values, sample.values, sizeof(sample.values));
                record.sample.valid_mask = sample.validMask;
                break;
            }
            case NativeAdrenoTraceSectionJobs: {
                const JobSample &job = jobs[index];
                record.job.completion = job.completion;
                record.job.retired_ns = job.retiredNs;
                record.job.fence = job.fence;
                record.job.queue = job.queue;
                record.job.issue_ticks = job.issue;
                record.job.cp_ticks = job.cp;
                record.job.rb_ticks = job.rb;
                record.job.valid_flags = (job.queueValid ? NativeAdrenoTraceJobQueueValid : 0) |
                    (job.rbValid ? NativeAdrenoTraceJobRbValid : 0);
                record.job.generation = window;
                break;
            }
            case NativeAdrenoTraceSectionEvents:
                source = reinterpret_cast<const uint8_t *>(&traceEvents[index]);
                break;
        }
        memcpy(reply->data + written, source + within, bytes);
        written += bytes;
    }
    IOLockUnlock(profileLock);

    return kIOReturnSuccess;
}
