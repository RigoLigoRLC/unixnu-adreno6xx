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

On 2026-10-25, `49h` booted to the desktop and USB captured the GPU worker.
Startup stopped while validating command-db BCM auxiliary data, before GMU
startup. The ACV fixed vote now accepts zero unit/width fields, following the
stock A660 encoding. Build `49i` completed all eight stages and its 512 MiB FAT
was written and synced; its GPU results remain pending. CP and Vulkan execution
have not yet been recorded on this device.

## Licenses and sources

The inherited NativeAdreno implementation retains its [MIT license](LICENSE).
The files marked `GPL-2.0-only` contain the Qualcomm A6xx/Yupik procedures and
register tables adapted from the Samsung SM7325 Android kernel, with The Linux
Foundation copyright notices retained. [COPYING](COPYING) contains GPL v2.
Each source file's SPDX identifier specifies its license. Vendor firmware is
a private device build input and is supplied separately.
