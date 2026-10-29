/* SPDX-License-Identifier: MIT */
#ifndef NATIVE_ADRENO_TRACE_H
#define NATIVE_ADRENO_TRACE_H

#include <stdint.h>

enum {
    NativeAdrenoTraceVersion = 1,
    NativeAdrenoMethodQueryTrace = 33,
    NativeAdrenoMethodControlTrace = 34,
    NativeAdrenoMethodReadTrace = 35,
    NativeAdrenoMethodAppendTrace = 36,
    NativeAdrenoMethodRegisterTraceProducer = 37,
    NativeAdrenoMethodEndTraceProducer = 38,
    NativeAdrenoTraceEventCapacity = 131072,
    NativeAdrenoTraceBatchEvents = 8,
    NativeAdrenoTraceChunkBytes = 1536,
    NativeAdrenoTraceCounterCount = 29,
    NativeAdrenoTraceCounterCapacity = 4096,
    NativeAdrenoTraceJobCapacity = 4096,
    NativeAdrenoTraceGpuClockHz = 19200000,
    NativeAdrenoTraceTcpPort = 1235,
    NativeAdrenoTraceTcpMagic = 0x43525441
};

enum {
    NativeAdrenoTraceStart = 1,
    NativeAdrenoTraceStop = 2,
    NativeAdrenoTraceSeal = 3,
    NativeAdrenoTraceIdle = 0,
    NativeAdrenoTraceRequested = 1,
    NativeAdrenoTraceRunning = 2,
    NativeAdrenoTraceDraining = 3,
    NativeAdrenoTraceComplete = 4,
    NativeAdrenoTraceSealed = 5,
    NativeAdrenoTraceSourceMetal = 1,
    NativeAdrenoTraceSourceTurnip = 2,
    NativeAdrenoTracePhaseBegin = 1,
    NativeAdrenoTracePhaseEnd = 2,
    NativeAdrenoTracePhaseInstant = 3,
    NativeAdrenoTraceClockGpuNanoseconds = 1,
    NativeAdrenoTraceClockMonotonicNanoseconds = 2,
    NativeAdrenoTraceArgumentUnsigned = 1,
    NativeAdrenoTraceArgumentSigned = 2,
    NativeAdrenoTraceArgumentFloatBits = 3,
    NativeAdrenoTraceEventInvalid = 1,
    NativeAdrenoTraceEventSourceFlagsMask = 0xffff0000u,
    NativeAdrenoTraceProducerIncomplete = 1,
    NativeAdrenoTraceJobQueueValid = 1,
    NativeAdrenoTraceJobRbValid = 2,
    NativeAdrenoTraceSectionStatus = 0,
    NativeAdrenoTraceSectionDefinitions = 1,
    NativeAdrenoTraceSectionCounters = 2,
    NativeAdrenoTraceSectionJobs = 3,
    NativeAdrenoTraceSectionEvents = 4
};

/* NativeAdrenoTraceArgument carries a source-defined parameter key, its
 * numeric representation and the corresponding 64-bit value. */
typedef struct NativeAdrenoTraceArgument {
    uint32_t key;
    uint32_t type;
    uint64_t value;
} NativeAdrenoTraceArgument;

/* NativeAdrenoTraceEvent carries a completed GPU timestamp or a Metal CPU
 * boundary. Producers use one capture generation per batch and append only
 * after their local records are complete. The host interprets arguments from
 * source and point; name contains the stable tracepoint name. */
typedef struct NativeAdrenoTraceEvent {
    uint32_t source;
    uint32_t point;
    uint32_t phase;
    uint32_t clock_domain;
    uint64_t timestamp_ns;
    uint64_t submission_id;
    uint64_t completion_id;      /* Global asynchronous completion receipt. */
    uint64_t command_buffer_id;
    uint32_t process_id;
    uint32_t queue_id;
    uint32_t argument_count;
    uint32_t flags;              /* Low 16 bits are shared; high 16 bits belong to source. */
    NativeAdrenoTraceArgument args[4];
    char name[48];
} NativeAdrenoTraceEvent;

typedef struct NativeAdrenoTraceBatch {
    uint32_t version;
    uint32_t generation;
    uint32_t count;
    uint32_t dropped;
    uint64_t producer_id;
    uint64_t reserved;
    NativeAdrenoTraceEvent events[NativeAdrenoTraceBatchEvents];
} NativeAdrenoTraceBatch;

/* NativeAdrenoTraceStatus describes one retained capture. Complete means the
 * hardware window and OOB ownership have ended; producers may still append.
 * Seal freezes all records for deterministic reads. CPU times use
 * mach_absolute_time converted to nanoseconds, without suspend time. */
typedef struct NativeAdrenoTraceStatus {
    uint32_t version;
    uint32_t generation;
    uint32_t state;
    uint32_t access_error;
    uint64_t started_ns;
    uint64_t stopped_ns;
    uint32_t counter_count;
    uint32_t sample_count;
    uint32_t job_count;
    uint32_t event_count;
    uint32_t sample_dropped;
    uint32_t job_dropped;
    uint32_t event_dropped;
    uint32_t producer_dropped;
    uint32_t pending_jobs;
    uint32_t hardware_owned;
    uint32_t gpu_clock_hz;
    uint32_t sample_interval_us;
    uint32_t sample_capacity;
    uint32_t job_capacity;
    uint32_t event_capacity;
    uint32_t event_bytes;
    uint32_t sample_bytes;
    uint32_t job_bytes;
    uint32_t definition_bytes;
    uint32_t producer_count;
    uint32_t producer_failed;
    uint32_t reserved[3];
} NativeAdrenoTraceStatus;

typedef struct NativeAdrenoTraceProducer {
    uint32_t version;
    uint32_t generation;
    uint32_t source;
    uint32_t flags;
    uint64_t producer_id;        /* Register returns a token; End supplies it. */
    uint32_t dropped;
    uint32_t reserved;
} NativeAdrenoTraceProducer;

typedef struct NativeAdrenoTraceControl {
    uint32_t version;
    uint32_t operation;
    uint32_t generation;         /* Stop and Seal require the current generation. */
    uint32_t duration_ms;        /* Start accepts 100 through 60000 milliseconds. */
} NativeAdrenoTraceControl;

typedef struct NativeAdrenoTraceRead {
    uint32_t version;
    uint32_t generation;
    uint32_t section;
    uint32_t offset;             /* Byte offset within the selected section. */
} NativeAdrenoTraceRead;

typedef struct NativeAdrenoTraceChunk {
    uint32_t version;
    uint32_t generation;
    uint32_t section;
    uint32_t offset;
    uint32_t bytes;
    uint32_t total_bytes;
    uint32_t reserved[2];
    uint8_t data[NativeAdrenoTraceChunkBytes];
} NativeAdrenoTraceChunk;

typedef struct NativeAdrenoTraceCounterDefinition {
    char name[48];
    char unit[16];
    uint32_t index;
    uint32_t selector;
    uint32_t event;
    uint32_t low_register;
    uint32_t load_bit;
    uint32_t reserved[3];
} NativeAdrenoTraceCounterDefinition;

typedef struct NativeAdrenoTraceCounterSample {
    uint64_t cpu_begin_ns;
    uint64_t cpu_end_ns;
    uint64_t gpu_begin_ticks;
    uint64_t gpu_end_ticks;
    uint64_t values[NativeAdrenoTraceCounterCount];
    uint32_t valid_mask;
    uint32_t reserved;
} NativeAdrenoTraceCounterSample;

typedef struct NativeAdrenoTraceJob {
    uint64_t completion;
    uint64_t retired_ns;
    uint32_t fence;
    uint32_t queue;
    uint32_t issue_ticks;
    uint32_t cp_ticks;
    uint32_t rb_ticks;
    uint32_t valid_flags;
    uint32_t generation;
    uint32_t reserved;
} NativeAdrenoTraceJob;

/* NativeAdrenoTraceTcpHeader prefixes each request and response. All wire
 * integers are little endian. Requests use an ABI method and its exact input;
 * responses echo request_id and method and carry the IOReturn status. */
typedef struct NativeAdrenoTraceTcpHeader {
    uint32_t magic;
    uint32_t version;
    uint32_t method;
    uint32_t request_id;
    uint32_t payload_bytes;
    uint32_t status;
    uint32_t reserved[2];
} NativeAdrenoTraceTcpHeader;

enum {
    NativeAdrenoTraceTurnipRenderPass = 1,
    NativeAdrenoTraceTurnipBinning = 2,
    NativeAdrenoTraceTurnipDrawSysmem = 3,
    NativeAdrenoTraceTurnipDrawGmem = 4,
    NativeAdrenoTraceTurnipCompute = 5,
    NativeAdrenoTraceTurnipComputeIndirect = 6,
    NativeAdrenoTraceTurnipGmemLoad = 7,
    NativeAdrenoTraceTurnipGmemStore = 8,
    NativeAdrenoTraceTurnipCacheFlush = 9,
    NativeAdrenoTraceTurnipTileStore = 10,
    NativeAdrenoTraceTurnipCopy = 11,
    NativeAdrenoTraceTurnipSubmit = 65536,
    NativeAdrenoTraceTurnipExecute = 65537,
    NativeAdrenoTraceTurnipInstrumentation = 65538,
    NativeAdrenoTraceTurnipRecordCounts = 65539,
    NativeAdrenoTraceArgWidth = 1,
    NativeAdrenoTraceArgHeight = 2,
    NativeAdrenoTraceArgBins = 3,
    NativeAdrenoTraceArgSamples = 4,
    NativeAdrenoTraceArgDrawCount = 5,
    NativeAdrenoTraceArgTiled = 6,
    NativeAdrenoTraceArgBandwidth = 7,
    NativeAdrenoTraceArgLrz = 8,
    NativeAdrenoTraceArgFormat = 9,
    NativeAdrenoTraceArgForceLoad = 10,
    NativeAdrenoTraceArgFastPath = 11,
    NativeAdrenoTraceArgUnaligned = 12,
    NativeAdrenoTraceArgLocalSize = 13,
    NativeAdrenoTraceArgGroupCount = 14,
    NativeAdrenoTraceArgIndirect = 15,
    NativeAdrenoTraceArgFlushBits = 16,
    NativeAdrenoTraceArgSecondaryCommand = 17,
    NativeAdrenoTraceArgTimestampDwords = 20,
    NativeAdrenoTraceArgCopyDwords = 21,
    NativeAdrenoTraceArgWaitForMeDwords = 22,
    NativeAdrenoTraceArgExtraWaitForIdleDwords = 23,
    NativeAdrenoTraceArgBudgetDropped = 24,
    NativeAdrenoTraceArgQueueDropped = 25,
    NativeAdrenoTraceArgAppendDropped = 26,
    NativeAdrenoTraceArgUnsubmittedCommands = 27,
    NativeAdrenoTraceArgUnexecutedGmemPoints = 28,
    NativeAdrenoTraceArgSourceFormat = 29,
    NativeAdrenoTraceArgDestinationFormat = 30,
    NativeAdrenoTraceArgBytes = 31
};

#endif
