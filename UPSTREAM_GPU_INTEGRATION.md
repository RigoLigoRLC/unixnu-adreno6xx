# SM7325 backend integration

This branch incorporates official Adreno740 `a9a2cbb` and ArmSmmu4K `83969cd`.
The device's prior GPU history remains on
`backup/unixnu-sm-t733-before-upstream-20261101`; the port commit applies the
SM7325 changes to the official ancestry. ArmSmmu4K `b49b0ea` supplies the
required coherent stream policy and retained-range update API.

NativeAdrenoGpuContext creates one NativeAdrenoProcessVm per rendering
connection. Its root and intermediate table pages come from the upstream
NativeAdrenoTablePages provider. Each VM owns its GPU address tree and ASID.
The shared kernel mapping registry records physical aliases across processes,
so asynchronous job dependencies and DPU read ownership cover the same pages
even when clients choose different GPU addresses. Up to 32 clients share the
256 software queues and eight outstanding hardware jobs.

NativeAdrenoSubmit chooses the VM from the IB handles. The A642L ring emits
the stock four-word CP_SMMU_TABLE_UPDATE payload, containing TTBR0 with ASID,
the zero context ID and the configured context bank. The CP uses APRIV enabled
by NativeAdrenoA6xxHwInit. Closing a client releases its jobs and BOs, emits a
kernel-table fence, waits for its completion, and invalidates the process ASID
before releasing the page provider and reusing the ASID.

Ordinary BO allocation uses the upstream system VM procedure: the descriptor
is created without a task or permanent kernel mapping and wired outside the
resource lock. User mappings retain that descriptor. A642L's established WB
coherence supplies visibility after barriers. Explicit contiguous BO requests
retain the local cleared-backing cache; Turnip supplies regular BO reuse.

ABI v1 selectors 1 through 42 retain their layout. SetPriority is selector 43
with two uint32 fields, foreground and background. Note is selector 44 with
kind, count and 30 uint32 words; pass kind is 1 and pipeline kind is 2. Each
kind retains 512 rows. Query flags add OwnedCoherent=64 and MemoryPressure=128.
The scheduler reads the submitting task's effective role and Darwin background
state through XNU's `proc_get_effective_task_policy` in NativeAdrenoTaskPolicy.c.
NativeAdrenoUserClient uses its C-compatible declaration. Background, non-UI,
throttled and Darwin-background applications select the background priority;
the other roles select the foreground priority. It preserves queue order and
promotes ready work waiting 250 ms. Priority updates apply to accepted work
still waiting in those queues and immediately reconsider runnable queue heads.

The independent trace selector is 0x7102. NativeGpuTrace.h defines its 120-byte
record and the shared Metal/Turnip event numbers. The
native-adreno-gpu-trace=1 boot argument enables bounded storage; sheng-gpu-trace
is also accepted. The service records actual dispatch and completed hardware
fences with the connection PID, Metal priority and XNU effective task role.
This trace's retirement event leaves GPU timestamp validity clear: the separate
A642L hardware capture owns CP/RB timestamps and GMU performance OOB. The
native_gpu_trace_snapshot entry point copies committed records for the caller's
export mechanism. No automatic console or disk output is added.

native_adreno_fences_detail exposes the single ring's issued fence and completed
prefix under a nonblocking service lock. A busy or failed query returns false
with its reason. DPU presentation continues to use per-surface read reservations.

The A642L runtime uses one ring. The upstream optional dual-ring preemption
controller remains unselected: its A7xx SMMU save record and restore procedure
are not installed by the SM7325 SQE startup path. Priority takes effect when
the scheduler issues runnable work. A642L GMU HFI, performance OOB ownership,
Normal NC firmware mappings, power transitions, DVFS and bounded hardware
capture retain their SM7325 implementations. The service uses chip ID
0x06030500 and the A660 firmware inputs.

The device build recipe includes NativeAdrenoTaskPolicy.c with the C compiler
recipe; dynamic-table and process-VM code resides in the existing objects.
The consuming device must pin
the matching ArmSmmu4K revision and install this NativeAdrenoABI.h for Metal
and Turnip. Build, deployment and device observations are recorded by the
device repository for each fixed build tag.
