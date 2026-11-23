// SPDX-License-Identifier: GPL-3.0-or-later
// Exercise the observed console device ABI without launching a game or using its data.
#include <windows.h>
#include <d3d12.h>
#include <dxgi1_4.h>

#include <cstdint>
#include <cstdio>
#include <cstring>
#include <memory>

extern "C" HRESULT WINAPI D3D12XboxCreateDevice(IUnknown*, void*, REFIID, void**);

namespace {

constexpr GUID kDevice11 = {0x5405c344, 0xd457, 0x444e,
                            {0xb4, 0xdd, 0x23, 0x66, 0xe4, 0x5a, 0xee, 0x39}};

template <class T>
struct ReleaseCom {
    void operator()(T* p) const { p->Release(); }
};
template <class T>
using Com = std::unique_ptr<T, ReleaseCom<T>>;

int fail(const char* check, HRESULT hr = E_FAIL) {
    std::fprintf(stderr, "[t41] FAIL: %s (hr=0x%08lx)\n", check,
                 static_cast<unsigned long>(hr));
    return 1;
}

bool equal(const D3D12_HEAP_PROPERTIES& a, const D3D12_HEAP_PROPERTIES& b) {
    return a.Type == b.Type && a.CPUPageProperty == b.CPUPageProperty &&
           a.MemoryPoolPreference == b.MemoryPoolPreference &&
           a.CreationNodeMask == b.CreationNodeMask && a.VisibleNodeMask == b.VisibleNodeMask;
}

using XboxListCall = HRESULT (WINAPI*)(ID3D12Device*, const void*, ID3D12CommandAllocator*,
                                       ID3D12PipelineState*, const GUID*, void**);

struct CloseEvent {
    void operator()(void* h) const { CloseHandle(h); }
};

int check_desktop_list(ID3D12Device* device, ID3D12CommandQueue* queue,
    ID3D12CommandAllocator* allocator, ID3D12GraphicsCommandList* list,
    D3D12_COMMAND_LIST_TYPE type) {
    if (list->GetType() != type) return fail("slot 73 desktop list has wrong type");
    D3D12_RESOURCE_DESC desc{};
    desc.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER;
    desc.Width = 64;
    desc.Height = desc.DepthOrArraySize = desc.MipLevels = desc.SampleDesc.Count = 1;
    desc.Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;
    D3D12_HEAP_PROPERTIES upload_heap{D3D12_HEAP_TYPE_UPLOAD, D3D12_CPU_PAGE_PROPERTY_UNKNOWN,
                                       D3D12_MEMORY_POOL_UNKNOWN, 1, 1};
    auto readback_heap = upload_heap;
    readback_heap.Type = D3D12_HEAP_TYPE_READBACK;
    void* raw = nullptr;
    auto hr = device->CreateCommittedResource(&upload_heap, D3D12_HEAP_FLAG_NONE, &desc,
        D3D12_RESOURCE_STATE_GENERIC_READ, nullptr, __uuidof(ID3D12Resource), &raw);
    if (FAILED(hr) || !raw) return fail("create upload buffer for slot 73", hr);
    Com<ID3D12Resource> upload(static_cast<ID3D12Resource*>(raw));
    raw = nullptr;
    hr = device->CreateCommittedResource(&readback_heap, D3D12_HEAP_FLAG_NONE, &desc,
        D3D12_RESOURCE_STATE_COPY_DEST, nullptr, __uuidof(ID3D12Resource), &raw);
    if (FAILED(hr) || !raw) return fail("create readback buffer for slot 73", hr);
    Com<ID3D12Resource> readback(static_cast<ID3D12Resource*>(raw));
    raw = nullptr;
    hr = device->CreateFence(0, D3D12_FENCE_FLAG_NONE, __uuidof(ID3D12Fence), &raw);
    if (FAILED(hr) || !raw) return fail("create slot 73 fence", hr);
    Com<ID3D12Fence> fence(static_cast<ID3D12Fence*>(raw));
    std::unique_ptr<void, CloseEvent> completed(CreateEventW(nullptr, FALSE, FALSE, nullptr));
    if (!completed) return fail("create slot 73 fence event", HRESULT_FROM_WIN32(GetLastError()));

    // Submit one copy on the initially open list, then reuse both allocator and
    // list after the fence. Read the bytes back to prove each copy ran on the queue.
    constexpr D3D12_RANGE no_range{0, 0};
    for (UINT64 iteration : {1u, 2u}) {
        if (iteration == 2) {
            hr = allocator->Reset();
            if (FAILED(hr)) return fail("reset slot 73 allocator after fence", hr);
            hr = list->Reset(allocator, nullptr);
            if (FAILED(hr)) return fail("reopen slot 73 list", hr);
        }
        const UINT payload = 0x8e930100u | (static_cast<UINT>(type) << 4) |
                             static_cast<UINT>(iteration);
        constexpr D3D12_RANGE word_range{0, sizeof(payload)};
        void* mapped = nullptr;
        hr = upload->Map(0, &no_range, &mapped);
        if (FAILED(hr) || !mapped) return fail("map slot 73 upload buffer", hr);
        std::memcpy(mapped, &payload, sizeof(payload));
        upload->Unmap(0, &word_range);
        list->CopyBufferRegion(readback.get(), 0, upload.get(), 0, sizeof(payload));
        hr = list->Close();
        if (FAILED(hr)) return fail("close slot 73 desktop list", hr);
        ID3D12CommandList* lists[]{list};
        queue->ExecuteCommandLists(1, lists);
        hr = queue->Signal(fence.get(), iteration);
        if (FAILED(hr)) return fail("signal slot 73 submission fence", hr);
        hr = fence->SetEventOnCompletion(iteration, completed.get());
        if (FAILED(hr)) return fail("arm slot 73 fence event", hr);
        if (WaitForSingleObject(completed.get(), 10000) != WAIT_OBJECT_0)
            return fail("slot 73 submission fence wait failed");
        const auto value = fence->GetCompletedValue();
        if (value == UINT64_MAX || value < iteration) return fail("slot 73 submission did not complete");
        mapped = nullptr;
        hr = readback->Map(0, &word_range, &mapped);
        if (FAILED(hr) || !mapped) return fail("map slot 73 readback buffer", hr);
        UINT seen = 0;
        std::memcpy(&seen, mapped, sizeof(seen));
        readback->Unmap(0, &no_range);
        if (seen != payload) return fail("slot 73 queue did not copy the expected payload");
    }
    std::printf("[t41] slot 73 %s: two GPU copies matched after fence (including Reset)\n",
        type == D3D12_COMMAND_LIST_TYPE_DIRECT ? "DIRECT" : "COMPUTE");
    return 0;
}

}  // namespace

int main(int argc, char** argv) {
    const bool hardware = argc == 2 && std::strcmp(argv[1], "--hardware") == 0;
    if (argc != 1 && !hardware) return fail("usage: t41_d3d12_device_abi [--hardware]");
    void* raw = nullptr;
    HRESULT hr = CreateDXGIFactory1(__uuidof(IDXGIFactory4), &raw);
    if (FAILED(hr)) {
        std::printf("[t41] SKIP: DXGI factory4 unavailable\n");
        return 2;
    }
    Com<IDXGIFactory4> factory(static_cast<IDXGIFactory4*>(raw));
    Com<IDXGIAdapter> adapter;
    if (hardware) {
        // Match the shim's default preference: hardware with most dedicated VRAM.
        SIZE_T best_vram = 0;
        DXGI_ADAPTER_DESC1 best{};
        for (UINT i = 0;; ++i) {
            IDXGIAdapter1* item = nullptr;
            hr = factory->EnumAdapters1(i, &item);
            if (hr == DXGI_ERROR_NOT_FOUND) break;
            if (FAILED(hr)) return fail("enumerate hardware adapters", hr);
            Com<IDXGIAdapter1> candidate(item);
            DXGI_ADAPTER_DESC1 desc{};
            if (FAILED(candidate->GetDesc1(&desc)) || (desc.Flags & DXGI_ADAPTER_FLAG_SOFTWARE))
                continue;
            if (!adapter || desc.DedicatedVideoMemory > best_vram) {
                best_vram = desc.DedicatedVideoMemory;
                best = desc;
                adapter.reset(candidate.release());
            }
        }
        if (!adapter) {
            std::printf("[t41] SKIP: no hardware DXGI adapter\n");
            return 2;
        }
        std::printf("[t41] adapter: %ls\n", best.Description);
    } else {
        hr = factory->EnumWarpAdapter(__uuidof(IDXGIAdapter), &raw);
        if (FAILED(hr)) {
            std::printf("[t41] SKIP: WARP unavailable\n");
            return 2;
        }
        adapter.reset(static_cast<IDXGIAdapter*>(raw));
    }

    // Use the exact version observed in the game if the desktop runtime offers it.
    const GUID* iid = &kDevice11;
    hr = D3D12CreateDevice(adapter.get(), D3D_FEATURE_LEVEL_11_0, *iid, &raw);
    if (hr == E_NOINTERFACE) {
        iid = &__uuidof(ID3D12Device);
        hr = D3D12CreateDevice(adapter.get(), D3D_FEATURE_LEVEL_11_0, *iid, &raw);
    }
    if (FAILED(hr)) return fail("create reference desktop device", hr);
    Com<ID3D12Device> native(static_cast<ID3D12Device*>(raw));
    raw = nullptr;
    hr = D3D12XboxCreateDevice(adapter.get(), nullptr, *iid, &raw);
    if (FAILED(hr) || !raw) return fail("create Xbox device proxy", hr);
    Com<ID3D12Device> xbox(static_cast<ID3D12Device*>(raw));
    if (xbox.get() == native.get()) return fail("Xbox device must not be the native COM pointer");

    // The ordering here deliberately differs from the C++ PC signature.
    using XboxHeapCall = D3D12_HEAP_PROPERTIES* (WINAPI*)(D3D12_HEAP_PROPERTIES*,
                                                         ID3D12Device*, UINT, D3D12_HEAP_TYPE);
    using Slot = void (WINAPI*)();
    const Slot* vtable;
    std::memcpy(&vtable, static_cast<void*>(xbox.get()), sizeof(vtable));
    auto heap_props = reinterpret_cast<XboxHeapCall>(vtable[26]);
    for (const auto type : {D3D12_HEAP_TYPE_DEFAULT, D3D12_HEAP_TYPE_UPLOAD, D3D12_HEAP_TYPE_READBACK}) {
        struct {
            UINT begin = 0x11223344;
            D3D12_HEAP_PROPERTIES out{};
            UINT end = 0xaabbccdd;
        } result;
        const auto expected = native->GetCustomHeapProperties(1, type);
        if (heap_props(&result.out, xbox.get(), 1, type) != &result.out ||
            result.begin != 0x11223344 || result.end != 0xaabbccdd || !equal(result.out, expected)) {
            return fail("slot 26 must write desktop-equivalent heap properties to the Xbox buffer");
        }
    }

    // The four observed Xbox queue descriptors. Slot 48 on desktop is
    // OpenExistingHeapFromAddress, not a queue creation method.
    using XboxQueueCall = HRESULT (WINAPI*)(ID3D12Device*, const void*, REFIID, void**);
    const auto queue_call = reinterpret_cast<XboxQueueCall>(vtable[48]);
    const UINT queue_descs[][4] = {{2, 0, 1, 0}, {2, 0, 2, 0}, {2, 0, 3, 0}, {2, 0, 0, 1}};
    for (const auto& desc : queue_descs) {
        raw = nullptr;
        hr = queue_call(xbox.get(), desc, __uuidof(ID3D12CommandQueue), &raw);
        if (FAILED(hr) || !raw) return fail("slot 48 must create a real compute queue", hr);
        Com<ID3D12CommandQueue> queue(static_cast<ID3D12CommandQueue*>(raw));
        const D3D12_COMMAND_QUEUE_DESC expected{D3D12_COMMAND_LIST_TYPE_COMPUTE, 0,
            static_cast<D3D12_COMMAND_QUEUE_FLAGS>(desc[2] & 1u), desc[3]};
        hr = native->CreateCommandQueue(&expected, __uuidof(ID3D12CommandQueue), &raw);
        if (FAILED(hr) || !raw) return fail("reference compute queue", hr);
        Com<ID3D12CommandQueue> reference(static_cast<ID3D12CommandQueue*>(raw));
        const auto actual = queue->GetDesc();
        const auto native_desc = reference->GetDesc();
        if (actual.Type != native_desc.Type || actual.Priority != native_desc.Priority ||
            actual.Flags != native_desc.Flags || actual.NodeMask != native_desc.NodeMask) {
            return fail("slot 48 queue descriptor must match the translated desktop queue");
        }
        hr = xbox->CreateFence(0, D3D12_FENCE_FLAG_NONE, __uuidof(ID3D12Fence), &raw);
        if (FAILED(hr) || !raw) return fail("create queue test fence", hr);
        Com<ID3D12Fence> fence(static_cast<ID3D12Fence*>(raw));
        hr = queue->Signal(fence.get(), 1);
        if (FAILED(hr)) return fail("slot 48 queue must accept a fence signal", hr);
    }
    const UINT unsupported_desc[4] = {2, 0, 0x80000000, 0};
    raw = xbox.get();
    hr = queue_call(xbox.get(), unsupported_desc, __uuidof(ID3D12CommandQueue), &raw);
    if (hr != E_INVALIDARG || raw) return fail("slot 48 must reject unknown flags", hr);
    raw = xbox.get();
    hr = queue_call(xbox.get(), nullptr, __uuidof(ID3D12CommandQueue), &raw);
    if (hr != E_INVALIDARG || raw) return fail("slot 48 must reject a null descriptor", hr);
    const D3D12_QUERY_HEAP_DESC query_desc{D3D12_QUERY_HEAP_TYPE_TIMESTAMP, 2, 1};
    hr = xbox->CreateQueryHeap(&query_desc, __uuidof(ID3D12QueryHeap), &raw);
    if (FAILED(hr) || !raw) return fail("regular CreateQueryHeap forwarding", hr);
    static_cast<ID3D12QueryHeap*>(raw)->Release();

    if (iid == &kDevice11) {
        void* address = VirtualAlloc(nullptr, 65536, MEM_RESERVE | MEM_COMMIT, PAGE_READWRITE);
        if (!address) return fail("allocate desktop slot 48 test region");
        const auto heap_iid = __uuidof(ID3D12Heap);
        void* reference_heap = nullptr;
        const auto native_hr = static_cast<ID3D12Device3*>(native.get())->OpenExistingHeapFromAddress(
            address, heap_iid, &reference_heap);
        if (reference_heap) static_cast<ID3D12Heap*>(reference_heap)->Release();
        raw = nullptr;
        hr = queue_call(xbox.get(), address, heap_iid, &raw);
        const bool same_result = hr == native_hr && (raw != nullptr) == (reference_heap != nullptr);
        if (raw) static_cast<ID3D12Heap*>(raw)->Release();
        VirtualFree(address, 0, MEM_RELEASE);
        if (!same_result) return fail("slot 48 must preserve OpenExistingHeapFromAddress", hr);
    }

    if (xbox->GetNodeCount() != native->GetNodeCount()) return fail("pass through GetNodeCount");
    raw = nullptr;
    hr = xbox->QueryInterface(__uuidof(IUnknown), &raw);
    if (FAILED(hr) || raw != xbox.get()) return fail("proxy IUnknown identity", hr);
    static_cast<IUnknown*>(raw)->Release();
    hr = xbox->QueryInterface(*iid, &raw);
    if (FAILED(hr) || raw != xbox.get()) return fail("proxy device interface identity", hr);
    static_cast<IUnknown*>(raw)->Release();
    raw = xbox.get();
    hr = xbox->QueryInterface(__uuidof(ID3D12CommandQueue), &raw);
    if (hr != E_NOINTERFACE || raw) return fail("proxy must reject unrelated interfaces", hr);

    // Slot 73 uses the Xbox descriptor ABI and returns a command-list proxy for
    // the console-only IID. The proxy must keep the native list alive while its
    // console extension slots are called.
    const auto make_list = reinterpret_cast<XboxListCall>(vtable[73]);
    constexpr GUID xbox_list_iid{0xc3827890, 0x4024, 0x474c,
        {0x98, 0xcf, 0x64, 0x5b, 0x51, 0xdc, 0x48, 0x74}};
    for (const auto type : {D3D12_COMMAND_LIST_TYPE_DIRECT, D3D12_COMMAND_LIST_TYPE_COMPUTE}) {
        raw = nullptr;
        hr = xbox->CreateCommandAllocator(type, __uuidof(ID3D12CommandAllocator), &raw);
        if (FAILED(hr) || !raw) return fail("create slot 73 test allocator", hr);
        Com<ID3D12CommandAllocator> allocator(static_cast<ID3D12CommandAllocator*>(raw));
        const UINT desc[] = {static_cast<UINT>(type), 2, 1, 0};
        struct {
            UINT_PTR before = 0x11223344;
            void* out;
            UINT_PTR after = 0xaabbccdd;
        } result{0x11223344, xbox.get(), 0xaabbccdd};
        hr = make_list(xbox.get(), desc, allocator.get(), nullptr, &xbox_list_iid, &result.out);
        if (FAILED(hr) || !result.out || result.before != 0x11223344 || result.after != 0xaabbccdd)
            return fail("slot 73 must create the Xbox command-list proxy", hr);
        Com<ID3D12GraphicsCommandList> xbox_list(
            static_cast<ID3D12GraphicsCommandList*>(result.out));
        const Slot* list_vtable = nullptr;
        std::memcpy(&list_vtable, xbox_list.get(), sizeof(list_vtable));
        using XboxListStateCall = void (WINAPI*)(ID3D12GraphicsCommandList*, UINT, UINT, UINT, UINT64);
        const GUID xbox_list5_iid{0x55050859, 0x4024, 0x474c,
            {0x98, 0xcf, 0x64, 0x5b, 0x51, 0xdc, 0x48, 0x74}};
        void* queried = nullptr;
        hr = xbox_list->QueryInterface(xbox_list5_iid, &queried);
        if (FAILED(hr) || queried != xbox_list.get()) return fail("Xbox command-list QI for list5", hr);
        static_cast<IUnknown*>(queried)->Release();
        auto list131 = reinterpret_cast<XboxListStateCall>(list_vtable[131]);
        auto list132 = reinterpret_cast<XboxListStateCall>(list_vtable[132]);
        list131(xbox_list.get(), 0xc3c0, 0x80020000, 0, 0xffffffffff00ull);
        list132(xbox_list.get(), 0x700000, 0, 0, 0xffffffffff00ull);
        if (xbox_list->Close() != S_OK) return fail("Xbox command-list proxy Close");
        hr = make_list(xbox.get(), desc, allocator.get(), nullptr, &__uuidof(ID3D12CommandQueue),
                       &result.out);
        if (hr != E_NOINTERFACE || result.out) return fail("slot 73 must reject an unrelated IID", hr);
        hr = make_list(xbox.get(), desc, allocator.get(), nullptr, &xbox_list_iid, nullptr);
        if (hr != E_POINTER) return fail("slot 73 must reject a null output", hr);

        const auto& desktop_list_iid = __uuidof(ID3D12GraphicsCommandList);
        const UINT invalid_descs[][4] = {
            {99, 2, 1, 0}, // unsupported list type
            {static_cast<UINT>(type), 2, 0, 0}, // node mask zero is not a console descriptor
        };
        for (auto& invalid : invalid_descs) {
            result.out = xbox.get();
            hr = make_list(xbox.get(), invalid, allocator.get(), nullptr, &desktop_list_iid,
                           &result.out);
            if (hr != E_INVALIDARG || result.out) return fail("slot 73 must reject unknown descriptors", hr);
        }
        hr = make_list(xbox.get(), desc, allocator.get(), nullptr, &desktop_list_iid, &result.out);
        if (FAILED(hr) || !result.out || result.before != 0x11223344 || result.after != 0xaabbccdd)
            return fail("slot 73 desktop IID must create a real command list", hr);
        Com<ID3D12GraphicsCommandList> list(static_cast<ID3D12GraphicsCommandList*>(result.out));

        const UINT queue_desc[]{static_cast<UINT>(type), 0, 0, xbox->GetNodeCount() == 1 ? 0u : 1u};
        raw = nullptr;
        hr = queue_call(xbox.get(), queue_desc, __uuidof(ID3D12CommandQueue), &raw);
        if (FAILED(hr) || !raw) return fail("create matching slot 48 queue for slot 73", hr);
        Com<ID3D12CommandQueue> queue(static_cast<ID3D12CommandQueue*>(raw));
        if (check_desktop_list(xbox.get(), queue.get(), allocator.get(), list.get(), type)) return 1;
    }

    // Arguments beyond the four Windows x64 argument registers must also survive the thunk.
    const D3D12_HEAP_PROPERTIES upload{D3D12_HEAP_TYPE_UPLOAD, D3D12_CPU_PAGE_PROPERTY_UNKNOWN,
                                       D3D12_MEMORY_POOL_UNKNOWN, 1, 1};
    D3D12_RESOURCE_DESC buffer{};
    buffer.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER;
    buffer.Width = 64;
    buffer.Height = 1;
    buffer.DepthOrArraySize = 1;
    buffer.MipLevels = 1;
    buffer.SampleDesc.Count = 1;
    buffer.Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;
    hr = xbox->CreateCommittedResource(&upload, D3D12_HEAP_FLAG_NONE, &buffer,
                                     D3D12_RESOURCE_STATE_GENERIC_READ, nullptr,
                                     __uuidof(ID3D12Resource), &raw);
    if (FAILED(hr) || !raw) return fail("pass through CreateCommittedResource", hr);
    static_cast<ID3D12Resource*>(raw)->Release();

    std::printf("[t41] PASS: %s, heap types 1/2/3, slot 48 queue/native heap paths, slot 73 desktop GPU copies and Xbox IID guard, IUnknown identity, native passthrough\n",
                iid == &kDevice11 ? "ID3D12Device11" : "ID3D12Device");
    return 0;
}
