// Xbox-ABI proxy around a PC ID3D12Device.
//
// Measured Xbox device deviations used here (vtable indices, x64):
//   slot 62   CreateComputePipelineState(desc, w2, w3, iid, out): two words (0 from XboxBvhBuilder, meaning unknown)
//             before the IID. The PC slot 62 is CreateStateObject.
//   slot 76   resource creation -> PC CreateCommittedResource (PC slot 27), with the Xbox layout word ignored.
// The measured slots 9/10 (GetCPU/GPUDescriptorHandleForHeapStart) are ID3D12DescriptorHeap methods, not device
// methods: the PC device's 9/10 are CreateCommandAllocator/CreateGraphicsPipelineState and are forwarded unchanged.
// On the PC heap those getters use the hidden return pointer in RDX; the Xbox ABI returns the 8-byte handle in
// RAX. Device slot 14 therefore wraps the created heap in a heap proxy that performs that conversion. The heap
// proxy holds a reference to the device proxy, and its ID3D12DeviceChild::GetDevice returns that same proxy.
// The proxy's heap slots 9/10 expose only the Xbox shape: a PC C++ caller using the PC header's return convention
// would not receive the value, while GetDesc and the other heap slots forward the PC convention unchanged. A
// direct one-argument call against a real PC heap is invalid, so the device test exercises the conversion on the
// wrapper and compares it with a hidden-pointer call against the real heap.
//
// Every other PC device slot is forwarded by a generated thunk that swaps `this` to the PC device. The proxy's
// vtable therefore follows the PC device order, with slots 62 and 76 deliberately shadowed. The full Xbox vtable
// order is not known from the spec; slots beyond the PC device's own vtable return E_NOTIMPL and log once.
//
// Command queues and fences (device slots 8, 75, 36, and 32 for a shared fence) come back wrapped in queue and fence
// proxies of the same kind: the public chain (19 queue slots, 11 or 12 fence slots in SDK 10.0.26100) is forwarded
// in PC order, and a slot past it, up to the end of the proxy's table (128 queue, 64 fence slots), is logged with its
// first four argument words and refused with E_NOTIMPL. A slot past the table faults on the vtable read inside a
// reserved, never-committed region, and the fault is logged with the slot. That is where the console's queue methods
// (PresentX, SuspendX, ResumeX) will show up; their slots are not known and are not guessed, and that the console
// keeps the public slots is an assumption (docs/d3d12_command_objects.md §5.2). Heap, queue and fence proxies are
// unwrapped wherever a device or queue method hands them to the real runtime: queue Signal/Wait (14/15), device
// CreateSharedHandle (31), MakeResident/Evict (34/35), SetEventOnMultipleFenceCompletion (45), SetResidencyPriority
// (46) and EnqueueMakeResident (50). ExecuteCommandLists (queue slot 10) replaces list proxies with their PC lists.
// QueryInterface answers only the modelled interfaces; one the real object has but the proxy does not model is logged once and refused.
//
// Command lists (docs/d3d12_command_objects.md section 3.5a): device slots 12 CreateCommandList and 51
// CreateCommandList1 (public slots, not measured on the console) wrap the PC list in a list proxy whose vtable is
// in the measured console list order, which is not the PC order past ResourceBarrier (26):
//   console 0-26  PC 0-26 (14, 15, 25, 26 measured; 7 GetDevice returns the device proxy)
//   console 32    PC 28 SetDescriptorHeaps, with heap proxies replaced by their PC heaps
//   console 33/35/41/53/61  PC 29/31/37/49/59
// Every other console slot up to 160 logs once and returns E_NOTIMPL without a PC call. The object also reserves
// the console fields past the vtable (+0x08..+0x608) with the write cursor (+0x18) at both limits (+0x28, +0x30),
// so a console-compiled caller takes the vtable for every command. A C++ caller using the PC header records on
// list_proxy_real_list(list) instead. A queue must be given the PC lists: ExecuteCommandLists replaces each list
// proxy with list_proxy_real_list (queue slot 10 does this).
#pragma once

#include <d3d12.h>

#include <cstdint>

namespace rt::d3d12_xs {

// Wraps `real` and returns the requested device interface through `out`. Takes ownership of one reference to
// `real` on success; on failure the caller keeps its reference. Returns E_NOINTERFACE if `riid` is not a D3D12
// device interface the PC device implements.
HRESULT create_device_proxy(ID3D12Device* real, REFIID riid, void** out);

// True when `device` points at a proxy created by create_device_proxy. Used by the tests.
bool is_device_proxy(const void* device);

// True when `heap` is a descriptor-heap proxy created by device slot 14.
bool is_heap_proxy(const void* heap);
ID3D12DescriptorHeap* heap_proxy_real_heap(const void* heap);

// True when `list` is a command-list proxy created by device slot 12 or 51 (a vtable-pointer check).
bool is_list_proxy(const void* list);
// The PC list behind a list proxy, borrowed (no AddRef); null when `list` is not a proxy. The proxy holds one
// reference on it and one on its device proxy until its own count reaches zero.
ID3D12CommandList* list_proxy_real_list(const void* list);
// The PC list slot a console list slot forwards to, ~0u when it is refused. 32 reports 28 (it unwraps heaps).
unsigned list_slot_pc_slot(unsigned slot);
std::uint64_t list_slot_hits(unsigned slot);
bool list_slot_first_call_seen(unsigned slot);

// Number of PC vtable slots the proxy forwards (detected from the interfaces the PC device supports).
unsigned device_proxy_slot_count(const void* device);

// The PC device behind a proxy. Null when `device` is not a proxy.
ID3D12Device* device_proxy_real_device(const void* device);

// True for slots the proxy handles itself rather than forwarding verbatim: IUnknown (0-2), 8 and 75 (queue
// proxies), 12 and 51 (list proxies), 14, 31/34/35/45/46/50 (proxies unwrapped), 32 and 36 (fence proxies),
// 62, 67 and 76.
// Slots 9 and 10 are intentionally false; the handle conversion is on the heap proxy.
bool device_slot_overridden(unsigned slot);

// Queue and fence proxies created by device slots 8/75 and 36. The real_* accessors return the wrapped object
// (no reference added), or null when the pointer is not such a proxy.
bool is_queue_proxy(const void* queue);
bool is_fence_proxy(const void* fence);
ID3D12CommandQueue* queue_proxy_real_queue(const void* queue);
ID3D12Fence* fence_proxy_real_fence(const void* fence);
// Public slots forwarded (19 for a queue; 11 or 12 for a fence), and the size of the proxy's vtable.
unsigned queue_proxy_slot_count(const void* queue);
unsigned fence_proxy_slot_count(const void* fence);
unsigned queue_proxy_table_size();
unsigned fence_proxy_table_size();
// Live references on a queue or fence proxy (0 for anything else). Test helper.
unsigned long command_object_proxy_ref_count(const void* object);
// Per-slot call counters and first-call flags, shared by all queue proxies and all fence proxies.
std::uint64_t queue_slot_hits(unsigned slot);
bool queue_slot_first_call_seen(unsigned slot);
std::uint64_t fence_slot_hits(unsigned slot);
bool fence_slot_first_call_seen(unsigned slot);
// Vtable reads past a queue or fence proxy table that faulted in its guard region (counted by the fault logger).
std::uint64_t queue_table_overruns();
std::uint64_t fence_table_overruns();
// True when both tables sit in front of their guard region (false only if the reservation failed).
bool command_object_tables_guarded();
// Heap slots 0-2, 7 (GetDevice -> device proxy) and 9/10 (handle conversion).
bool heap_slot_overridden(unsigned slot);

// Number of live references on a device proxy. Test helper for the heap's GetDevice/lifetime checks.
unsigned long device_proxy_ref_count(const void* device);

// Per-slot call counters and first-call flags for the device and heap proxies. Slot 0 also counts the
// QueryInterface call made while a proxy is created, so tests reset the counters after creation.
// reset_device_proxy_stats also resets the queue and fence counters.
std::uint64_t device_slot_hits(unsigned slot);
bool device_slot_first_call_seen(unsigned slot);
std::uint64_t heap_slot_hits(unsigned slot);
bool heap_slot_first_call_seen(unsigned slot);
void reset_device_proxy_stats();

}  // namespace rt::d3d12_xs
