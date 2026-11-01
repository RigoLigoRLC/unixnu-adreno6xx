# Adreno6xx working instructions

- This component implements Adreno 642L on SM7325 for SM-T733. Preserve the
  real chip ID `0x06030500`, 512 KiB GMEM, 315 MHz first GPU request and A660
  firmware family. Read README.md and the affected implementation before edits.
- The component ID and resolved checkout are `Adreno6xx`. Preserve the
  NativeAdreno runtime names and ABI v1 records in `NativeAdrenoABI.h`.
- The device kit supplies physical resources and SMMU assignments through
  `native_adreno_a6xx_resources`. The service owns mappings, DMA memory and
  publication. Keep board addresses and private firmware paths in the device
  repository; keep A6xx hardware procedures in this component.
- Power and GMU receive bounded byte-offset MMIO and delay callbacks. Use the
  live command database for ARC indexes and BCM data. Follow the SM7325 stock
  GPUCC/GCC clock semantics, A6xx GX status bits 6/7, GMU fault bit 15 and RSCC
  sequencer offset `0x180`. HFI startup requires actual matching replies.
- The SM-T733 device build runs `scripts/build-adreno6xx.py --tree
  RESOLVED_TREE` after kernel preparation. It produces
  `build/adreno6xx/libAdreno6xx.a`; `bash scripts/build.sh TAG` in the device
  repository runs the complete build. Keep dependent compilation and packaging
  under the coordinating build owner.
- Ordinary BOs use CPU WB and coherent GPU mappings, including reusable
  allocations. Honor contiguous=0 directly. Imported page attributes are
  recorded once and preserved until unmap; mixed WB/NC pages receive matching
  GPU leaves. Submission and completion use barriers with no CPU cache-line
  maintenance. GPU cache commands, fences and retained mapping lifetimes remain.
  GMU payloads and ZAP metadata use direct NC allocations for their firmware
  interfaces. See [BO memory management](BO_MEMORY_MANAGEMENT.md).
- removeMappingForDescriptor returns a retained IOMemoryMap. Explicitly unmap
  and release that reference before releasing the BO's physical allocation;
  retain the backing memory if teardown fails. IOUserClient locking covers map
  creation, external methods and client close.
- The ZAP reservation requires a write-combined CPU mapping, matching the
  stock PIL ioremap_wc procedure. Copy the ELF segment's file bytes, then zero
  only its remaining memory bytes. Keep MMIO windows on their device mapping.
- Preserve the upstream MIT license and the GPL-2.0-only SPDX/copyright notices
  on adapted Qualcomm procedures and tables. COPYING contains GPL v2.
- Do not hash files. Do not add tests or run extra verification unless the user
  requests it. Complete an authorized deployment before deferred documentation.
