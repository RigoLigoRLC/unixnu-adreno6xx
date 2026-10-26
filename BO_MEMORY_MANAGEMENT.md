# BO memory management

The driver separates a BO's backing allocation, GPU address range and user CPU
view. NativeAdrenoUserClient owns the BO. NativeAdrenoVm retains its memory
while GPU page-table entries exist. NativeAdrenoSubmit retains job references
until the ring fence completes. AGXDeviceUserClient removes CPU views before
the user client releases the backing allocation.

## GPU address records

NativeAdrenoVm embeds XNU's `libkern/tree.h` red-black-tree node in each BO
mapping. The tree orders GPU addresses; the adjacent BOs determine whether a
new interval overlaps an existing mapping. Insertion, removal and containing
address lookup are logarithmic in the number of mapped BOs. A linked list
supports iteration over live objects and gap searches for kernel allocations.
Turnip selects user BO addresses with its existing Mesa VMA allocator.

Each mapping keeps its own ArmSmmu4KRange array and a sorted physical-address
index. Attribute-class counts avoid physical comparisons when all objects use
the same cache, shareability and privilege attributes. When classes differ,
the smaller object's physical ranges query the other object's physical index.
No operation moves another BO's physical ranges. The context's actual leaf
budget bounds mapped bytes; the service separately bounds live BO records.

VM and user-client handles contain a slot and generation. Slot lookup and the
free list take constant time. Reusing a record changes its generation, so an
old handle cannot name the new object. User handles remain opaque ABI values.
CPU view teardown checks both the current handle and its mapping token.

## Page-table updates

The VM audits the empty context during attach. For each map it checks GPU
address overlap, physical attributes, physical bounds and both table pools.
It passes only the new BO's physical ranges to
`ArmSmmu4KLiveUpdateContextRanges`. That entry point checks the actual old leaf
descriptors, writes and publishes the changed table pages, and checks the new
descriptors. An unmap completes ASID invalidation and TLB synchronization
before the VM retires its memory reference.

The original SMMU entry points remain available to callers supplying complete
range lists. The BO path uses the incremental entry point. Its recurring work
depends on the BO being mapped, with no periodic scan of the full aperture.
The submission path retains its existing context TLB invalidation and cache
maintenance policy.

## Backing allocation reuse

NativeAdrenoBackingStorage owns a prepared IOBufferMemoryDescriptor, CPU
address, physical spans and cache attributes. NativeAdrenoMemory can transfer
ordinary user storage into a cache after all device references and CPU views
have been released. Imports, reserved-pool extents and quarantined objects are
excluded. Matching requires exact bytes, physical address width and physical
contiguity policy. Reuse reasserts WC attributes and clears the entire object
before exposing it to a new owner.

The cache holds at most 64 allocations and 64 MiB, with a 16 MiB per-object
limit. Low available memory, allocation failure and client close drain it.
Failed teardown retains the affected storage in quarantine. The reserved
contiguous pool keeps its existing ownership and cache policy.

## Measurements

The USB `gpu` query snapshots counters under the GPU lock and prints after
unlocking. `vm_map` and `vm_unmap` separate metadata, cache maintenance,
page-table updates, address-index changes and preflight/release. `pte_map` and
`pte_unmap` separate validation, old leaf checks, writes/publication, TLB
synchronization and new leaf checks. These phase totals count successful
transactions. Existing method counters also count failures.

The memory-stage rows report allocation, prepare, physical-span discovery,
zeroing, initial cleaning, attribute setup/reassertion/restoration, complete
and descriptor release. Backing-cache rows report hits, misses, retained bytes,
evictions, pressure drains and pages whose attributes required restoration.
All values accumulate in bounded memory; ordinary BO operations print no rows.

## Implementation references

- [Linux DRM memory management](https://docs.kernel.org/gpu/drm-mm.html):
  GEM object ownership, GPU address ranges and TTM page pools.
- [MSM GEM](https://github.com/torvalds/linux/blob/master/drivers/gpu/drm/msm/msm_gem.c):
  separate backing pages, CPU mapping and GPU virtual-address lifetime.
- [Mesa Freedreno BO cache](https://gitlab.freedesktop.org/mesa/mesa/-/blob/main/src/freedreno/drm/freedreno_bo_cache.c):
  reuse completed private BOs by compatible allocation properties.
- [Turnip suballocator](https://gitlab.freedesktop.org/mesa/mesa/-/blob/main/src/freedreno/vulkan/tu_suballoc.cc):
  place small aligned allocations inside an existing mapped BO.
- [Linux ARM LPAE page tables](https://github.com/torvalds/linux/blob/master/drivers/iommu/io-pgtable-arm.c):
  update affected descriptors and synchronize removed translations.

The XNU tree code is included from the kernel headers and retains its existing
license. These references informed the ownership and data-structure choices;
their hardware-specific cache and synchronization procedures remain platform
specific.
