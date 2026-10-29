# Adreno6xx

This driver contains the SM7325 backend for the Samsung Galaxy Tab S7 FE Wi-Fi
(SM-T733, `gts7fewifi`). Its Adreno 642L reports chip ID `0x06030500`, has 512 KiB
GMEM, and starts with a 315 MHz GPU request. The firmware inputs are
`a660_gmu.bin`, `a660_sqe.fw`, and the A660 ZAP metadata and shader segments.

The driver retains NativeAdreno ABI v1 in [NativeAdrenoABI.h](NativeAdrenoABI.h).
The existing BO allocation/import, SMMU mappings, queues, submission, fences and
IOKit transport connect to the A6xx power, GMU, CP and interrupt implementations.
The startup sequence requires actual firmware replies and a GPU memory write
before publishing the GPU service.

BO ownership, the GPU address tree, incremental page-table updates, reusable
backing allocations and USB timing counters are described in
[BO memory management](BO_MEMORY_MANAGEMENT.md).

## Submission and completion

`QueryAsync` exposes version 1 of the asynchronous extension independently of
ABI v1. The driver copies up to 64 accepted jobs and retains their BO ranges,
input timeline points and output signals. Eight jobs can occupy the hardware
ring. Each software queue preserves submission order; the oldest runnable queue
head is issued when ring space is available. A producer already issued on this
ring satisfies a GPU scheduling dependency. Host signals, future timeline values
and DPU readers resume scheduling through their completion events.

The GPU SPI is registered from the board's `gpu` interrupt entry. Its handler
acknowledges RBBM status and schedules the completion worker. That worker reads
the CP-written fence, retires matching jobs, updates timeline values and wakes
sleeping CPU waiters. The 10 ms service poll checks faults and the 500 ms
hardware timeout. Empty submissions emit a fence after their input dependencies
and prior queue work. ABI v1 submissions retain their hardware-fence replies and
report Busy when their dependencies cannot be issued immediately.

`NativeAdrenoSurfaceRead` retains the source descriptor and the mapped physical
ranges used by DPU. A bounded backing cache keeps the descriptor prepared and
stores its physical segments and mapping intervals. Repeated acquisition reads
those intervals, records the outstanding writers and inserts the display read
in the same locked operation. Mapping creation and removal maintain the cached
intervals; subsequent submissions follow those stored ranges.
The callback runs outside the GPU lock. Release synchronously cancels or joins
the callback and removes the read after DPU has stopped using the memory.
Kernel BO-idle waits cover accepted jobs, issued jobs and display reads before
the Darwin allocator reuses a BO. A fault completes waits with an error and
retains hardware references until CP and GMU DMA have stopped.

The `gpu` dump includes 256 bounded `NativeAdrenoAsync` records connecting each
software completion to its hardware fence and accepted, issued and completed
timestamps. Resource records count actual submitted ranges, alias queries,
matched intervals, GPU dependencies and display readers. Submission performs
zero physical-page walks. Detailed descriptor matching happens at display
acquisition and mapping creation.

Ordinary BO allocations, the command ring, fences, SQE firmware and scratch
memory use CPU write-back mappings and coherent GPU WB/WA mappings. Imports
retain their CPU attributes: pinned cached pages use GPU WB/WA; Normal NC
pages use GPU NC. Physical spans split where that policy changes. GPU and GMU
SMMU tables use CPU WB and coherent Outer Shareable table walks. Submission,
completion and table publication use ordering barriers without CPU cache-line
clean or invalidate operations. GPU cache commands and fences remain intact.

GMU payload/HFI buffers and ZAP SCM metadata are allocated directly with Normal
NC CPU mappings for their firmware interfaces. XNU owns their cache-attribute
transitions during allocation and release. BO allocation has no WC option.
Imported Device or unrecognized page attributes are refused at import; RT pages
are accepted as Normal NC only under the board's managed-RT boot policy.

## Bounded GPU hardware capture

`native_adreno_gpu_profile_start(20000)` requests a 20-second window.
The service worker acquires the GMU performance-counter OOB request before
starting the window or accepting profiled jobs. A642L uses request bit 28 at
`0x1f994`, acknowledgement bit 30 at `0x1f992`, and acknowledgement clear bit
30 at `0x1f991`. The worker polls for at most 100 ms at 100 us intervals. A
failed acknowledgement stops capture before any counter read and enters the
service's existing hardware fault handling. The request remains held across
samples and jobs, then release writes bit 29 at `0x1f994` after all registered
jobs have retired. `native_adreno_gpu_profile_stop()` closes the window early;
the worker performs its release. Array capacity also closes a window.

The service reads 29 fixed
A6xx counters every 20 ms while the window is active, retaining at most 4096
samples. Each sample contains the GMU always-on clock before and after the
reads, the raw 64-bit values and a measurement-validity mask. Definitions name
the selected events and their units. Adjacent valid samples supply the event
increments; the first sample establishes each window's starting values.
The selectors are configured once before SQE starts, while the worker holds
the same OOB request. During capture it reads only the RBBM counter values;
SP, TP, RB and other unit selectors are not reread. The GX power and AHB fence
checks reject inaccessible hardware while the acknowledged request remains
held. This window prevents autonomous GPU power collapse; it retains the
existing fixed GPU frequency.

USB and submission callbacks only update the recorder's bounded state under
its lock. That lock is released before GMU handshakes, sampling, hardware
fault handling or logging. The service worker also owns hardware shutdown.
On a fault it first disables new capture, synchronizes with the submission
owner and stops CP before clearing the OOB request. Successful GMU reset
revokes any remaining request; the normal DMA-stopped path retires pending
jobs as invalid. Failed hardware shutdown retains pending resources and keeps
capture disabled.

Each submitted job in the window receives a CP_REG_TO_MEM read of the GPU
clock before its first IB, followed by a Turnip-style RB_DONE_TS timestamp
after its final IB. The ordinary CACHE_FLUSH_TS fence follows both writes.
The submission retirement path copies completed values from the existing
coherent fence allocation into a second fixed array. The CPU reads the GMU AO
clock at `0x1f888/0x1f889` immediately before the WPTR write; GPU packets read
the CP clock at `0x980`. Queue ticks measure issue to CP arrival;
RB ticks measure CP arrival to RB completion, including overlap with other
pipeline work. These values use the common 19.2 MHz clock and its low 32 bits,
with modular differences below 2^31 ticks. The capture inserts no WFI.
Inactive capture adds no GPU packets or periodic register reads.

These operations follow the Samsung SM7325 kernel's
`drivers/gpu/msm/adreno_perfcounter.c:adreno_perfcounter_read_group`,
`adreno.h:adreno_perfcntr_active_oob_get`, and
`adreno_a6xx_gmu.c:a6xx_gmu_oob_set/a6xx_gmu_oob_clear`. The A642L device entry
selects `adreno_a6xx_gmu_gpudev`, whose probe installs `a6xx_gmudev` and this
OOB implementation. CPU clock selection follows
`adreno_a6xx.c:a6xx_read_alwayson`. The clock domains are paired in the stock
`adreno_ringbuffer.c:adreno_get_submit_time` and `_get_alwayson_counter`:
the former reads GMU AO, the latter emits the GPU CP read, and
`adreno_dispatch.c` records `end - cmdobj->submit_ticks` on retirement.

`native_adreno_gpu_profile_dump()` reports status while work remains, then
exports at most 64 counter samples and 128 jobs per call. Repeated calls
produce one `kind=begin`, counter definitions, numbered records and one
`kind=end`. The arrays remain stable during export and logging occurs outside
the profile lock. Capture retains raw event counts: SP counters aggregate
activity across shader processors, and ALU/TEX events count instructions for
their named shader stage. These records do not assign events to individual
shader programs or sample shader instruction addresses.
`kind=access` records acquisition, release and status with the window number,
ownership and error state. Jobs export independent `queue_valid` and
`rb_valid` fields; a failed CPU issue-clock read preserves valid GPU CP-to-RB
measurements. The compatibility `valid` field requires both values to be
valid. The finite rollover-read attempts establish a coherent counter value;
they do not time out a stalled Device MMIO transaction.

## Binary capture and user events

`NativeAdrenoTrace.h` defines the fixed little-endian trace records and methods
33 through 38. Query reports the hardware window generation and state. Control
starts or stops that same window, and Seal freezes it after captured GPU jobs
have retired, the worker has released the performance OOB, and every registered
user producer has ended. The recorder retains 32768 user events in addition to
4096 hardware samples and 4096 jobs. Full arrays preserve earlier records and
increment explicit loss counts.

Metal and Turnip register a producer on their IOKit connection for each capture.
They append up to eight 176-byte events per call, including real tracepoint
names, typed arguments and asynchronous completion receipts. Stop still permits
that generation's registered producers to append completed events. Each producer
flushes its local records and ends its registration; connection close releases
remaining registrations and records an incomplete producer. Read returns at most
1536 bytes from one frozen section, without logging its contents.

The device's USB network helper exposes Query, Control and Read on
`192.168.73.2:1235`, admitting only the USB host at `192.168.73.1`. Its IOKit
handoff connection does not allocate rendering resources. Counter samples retain
CPU `mach_absolute_time` converted to nanoseconds around the GMU clock and
counter reads. GPU timestamps retain the 19.2 MHz clock; hardware job records
include both the ring fence and the scheduler's global completion receipt.

## Frame timing

The existing `gpu` query also requests NativeMobileDisplay's bounded frame
history. Display supplies the native composition-start timestamp and retains
the IOSurface read fence through DPU scanout. GPU profiling matches the complete
physical byte sequence of that prepared descriptor to imported BOs which declare
write access, then selects their submissions between composition start and the
captured shared-ring fence. The reported list names direct writers of the output
surface. It does not assign intermediate render targets to the frame.

Surface write records occupy a 4096-entry ring, with each BO linking its own
records. Capture follows those links and returns up to 64 fence/sequence pairs.
A 32-entry descriptor cache retains successful matches while their GPU BO
generations remain mapped; the VM unmap callback releases the last matching
descriptor reference. Repeated captures use those identities without another
physical-page walk. New aliases invalidate only the matching source key.
First-match work is bounded and reported by `matchCacheHit`, `matchSegments`
and `matchUs`. The profiling history uses fixed storage; console output is
produced only when the diagnostic dump is requested.

Frame timestamps use continuous-clock microseconds, including the CPU's first
observation that the last direct writer's GPU fence completed. That observation
is an upper bound on GPU completion. The display's DPU timing supplies the
presentation interval. Missing composition timestamps, overwritten history,
released backing, conservative legacy resource access, missing completion and
bounded match/list capacity are explicit profile flags. Readiness comes from the
surface's retained writer objects. The hardware fence reported with a read is
used only to delimit the diagnostic writer history.

## Board interface

The device kit implements `native_adreno_a6xx_resources` from
[NativeAdrenoA6xxService.h](NativeAdrenoA6xxService.h). It supplies the GPU, GMU,
SMMU, GPUCC, GCC, PDC, RSCC, command-db, APPS RPMh DRV2, AOP QMP, IPCC and CX_MISC
physical windows, the ZAP reservation, device identity and SMMU stream/context
assignments. The service owns the mappings and DMA allocations.

[NativeAdrenoA6xxPower.h](NativeAdrenoA6xxPower.h) defines the bounded register
read/write and delay callbacks. Power reads the live command database, builds
ARC and A660 BCM votes, disables ACD through AOP, raises the parent rails, and
enables the Yupik clocks and CX/GX domains. GMU startup loads the A660 firmware,
initializes the A6xx HFI queues, programs RSCC/PDC handoff, and waits for firmware
acknowledgements of the tables, core start and 315 MHz request. Reports retain
the failed phase, register access and firmware result; the service retains the
mapped resources after a failure.

## Build and current device status

The SM-T733 device repository owns the build and private firmware inputs.
From that repository, `bash scripts/build.sh TAG` runs the complete device
build. Its `scripts/build-adreno6xx.py --tree RESOLVED_TREE` recipe compiles
this component and the required ArmSmmu4K/AdrenoPM4 sources into
`build/adreno6xx/libAdreno6xx.a`, embeds the private A660 inputs, and links the
archive into the kernel. The recipe defers the IOKit class constructors until
the runtime publication setup.

On 2026-10-25, build `50c` completed the build, its 512 MiB FAT write and sync,
and hardware startup. Kernel scratch/fence and all four userspace CP_MEM_WRITE
readbacks passed with zero faults. Turnip completed Vulkan offscreen fill of
1024 words, compute of 1024 words, and triangle rendering of 512 pixels with
zero unexpected pixels. It reported stage=0, VkResult=0, compute_stage=0 and
render_stage=0, then exited with status 0. DeviceInfo reports flags=1, UBWC=3,
swizzle=6 and macrotile=0. System Metal composition and DPU scanout remain
pending. The workspace [GPU implementation](../../../../notes/GPU_IMPLEMENTATION.md)
records the current result and raw log
`logs/t733-usb-live/t733-20261025T081340.968881Z.raw`.

LPAC has its own context, ASID and SID1/0x401 routes. Its page tables map the
main GPU's existing startup memory objects at the same IOVAs. The provider
handles faults for all three contexts and checks every startup page through
both GPU and both LPAC SIDs before starting SQE. The ZAP reservation uses a
write-combined CPU mapping; loading copies the ELF segment's file bytes and
zeros only the remaining memory bytes. The earlier LPAC and ZAP investigation
is recorded in the workspace's
[first-fetch audit](../../../../notes/GPU_FIRST_FETCH_AUDIT_2026-10-25.md).

## Licenses and sources

The inherited NativeAdreno implementation retains its [MIT license](LICENSE).
The files marked `GPL-2.0-only` contain the Qualcomm A6xx/Yupik procedures and
register tables adapted from the Samsung SM7325 Android kernel, with The Linux
Foundation copyright notices retained. [COPYING](COPYING) contains GPL v2.
Each source file's SPDX identifier specifies its license. Vendor firmware is
a private device build input and is supplied separately.
