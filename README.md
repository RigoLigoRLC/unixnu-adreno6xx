# Adreno6xx

This driver contains the SM7325 backend for the Samsung Galaxy Tab S7 FE Wi-Fi
(SM-T733, `gts7fewifi`). Its Adreno 642L reports chip ID `0x06030500`, has 512 KiB
GMEM, and starts with a 315 MHz GPU request. The firmware inputs are
`a660_gmu.bin`, `a660_sqe.fw`, and the A660 ZAP metadata and shader segments.

The driver advertises NativeAdreno ABI v2 in [NativeAdrenoABI.h](NativeAdrenoABI.h),
with up to 128 IBs in a 3104-byte submit record. The kernel also accepts the
416-byte version 1 record containing up to 16 IBs. Userspace queries the version
before choosing its record size; the mailbox transport uses the smaller record.
The existing BO allocation/import, SMMU mappings, queues, submission, fences and
IOKit transport connect to the A6xx power, GMU, CP and interrupt implementations.
The startup sequence requires actual firmware replies and a GPU memory write
before publishing the GPU service.

## Submission and mapping costs

BO handles use indices into 16384-entry client and shared VM tables. The sorted
IOVA range array uses binary searches for overlap and unmap locations. Dynamic
span capacity remains bounded by the configured page-table arenas. A fresh
driver allocation can use ArmSmmu4K's exclusive map transaction only while it
has no device references and has never exposed a client CPU mapping token.
Imports and exposed allocations retain physical alias checks.

ArmSmmu4K validates the changed descriptors during updates and completes unmap
TLB invalidation before returning. Submit publishes BO contents without an
additional full TLB flush. CPU cache maintenance for imports, write-combined
page attribute reassertion, and the shared submission lock remain in place.

These adaptations follow upstream Adreno740
[handle indices](https://github.com/UniXNU/Adreno740/commit/25abaa4b15e5da2b9966152899cf99389c63008d),
[VM changes](https://github.com/UniXNU/Adreno740/commit/1c4065cfb1f3e05e4bff30fbfabc6c291f472c28),
[128 IB records](https://github.com/UniXNU/Adreno740/commit/c232716ce093df4b73c9fca0e608d6fd430f5390),
and the synchronized
[ArmSmmu4K](https://github.com/UniXNU/ArmSmmu4K/commit/3ba92451bd44bb742d11758fbfcd3a446ecad685).

## Submission profiling

`native-adreno-profile=1` enables CPU duration accumulation on each successful
submission. `native-adreno-profile-every=16` is the default hardware sampling
interval; `1` samples every submission. Selected submissions emit begin/end
hardware counter snapshots, each containing one WFI and 107 command words.
The other submissions retain CPU timings without those GPU commands. Snapshot
buffers use whole-page publication and acquisition around their 512-byte slots.

`gpu-profile` prints eight cumulative aggregate lines with one shared `snapshot`
number. CPU totals cover validation including prior-job retirement, page-attribute
reassertion and optional IB audit, BO publication, and ring reserve/emission/WPTR
publication. They start after the global GPU lock and user-client dispatch have
been entered. Global lock waiting, BO allocation/import/map calls, and time after
submission completion are outside these CPU totals. GPU elapsed and queue delay
describe the selected submissions; their WFI operations still drain the pipeline.

The summary reports `sample_every`, `sampled`, `valid`, invalid sample counts,
valid counter and nonzero counter masks. Counter totals are accompanied by
per-counter valid sample counts. All-zero deltas leave `populated_mask=0`.
The 512-entry detail ring overwrites its oldest records independently of the
cumulative aggregates; `lost` counts only those overwritten detail records.
`native-adreno-profile-detail=1` additionally prints at most 32 detail records
per request. Runtime captures determine whether hardware counters are populated.

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
