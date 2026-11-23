// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include <d3d12.h>
#include <windows.h>

#include <cstdio>
#include <cstring>
#include <cwchar>
#include <memory>

namespace present_test {

template <typename T>
struct Release {
    void operator()(T* value) const { value->Release(); }
};
template <typename T>
using Com = std::unique_ptr<T, Release<T>>;

inline HWND find_window() {
    HWND result = nullptr;
    EnumWindows([](HWND hwnd, LPARAM output) -> BOOL {
        DWORD process = 0;
        GetWindowThreadProcessId(hwnd, &process);
        wchar_t title[20]{};
        if (process == GetCurrentProcessId() && GetWindowTextW(hwnd, title, 20) &&
            std::wcscmp(title, L"Xbgtasix") == 0) {
            *reinterpret_cast<HWND*>(output) = hwnd;
            return FALSE;
        }
        return TRUE;
    }, reinterpret_cast<LPARAM>(&result));
    return result;
}

inline int check_present_x(ID3D12Device* xbox, ID3D12Device* native) {
    USEROBJECTFLAGS station{};
    if (!GetUserObjectInformationW(GetProcessWindowStation(), UOI_FLAGS,
            &station, sizeof(station), nullptr) || !(station.dwFlags & WSF_VISIBLE)) {
        std::puts("[t41] slot 30: SKIP (no visible window station)");
        return 0;
    }
    const auto fail = [](const char* check, HRESULT hr = E_FAIL) {
        std::fprintf(stderr, "[t41] FAIL: PresentX %s (hr=0x%08lx)\n",
                     check, static_cast<unsigned long>(hr));
        return 1;
    };
    using Slot = void (WINAPI*)();
    using QueueCall = HRESULT (WINAPI*)(ID3D12Device*, const void*, REFIID, void**);
    using PresentCall = HRESULT (WINAPI*)(ID3D12CommandQueue*, UINT, const void*, const void*);
    const Slot* vtable = nullptr;
    std::memcpy(&vtable, static_cast<void*>(xbox), sizeof(vtable));
    const auto queue_call = reinterpret_cast<QueueCall>(vtable[8]);
    const UINT queue_desc[] = {D3D12_COMMAND_LIST_TYPE_DIRECT, 0, 0, 1};
    ID3D12CommandQueue* raw_queue = nullptr;
    HRESULT hr = queue_call(xbox, queue_desc, IID_PPV_ARGS(&raw_queue));
    if (FAILED(hr) || !raw_queue) return fail("create direct queue", hr);
    Com<ID3D12CommandQueue> queue(raw_queue);
    std::memcpy(&vtable, static_cast<void*>(queue.get()), sizeof(vtable));
    const auto present = reinterpret_cast<PresentCall>(vtable[30]);

    D3D12_HEAP_PROPERTIES heap{D3D12_HEAP_TYPE_DEFAULT, D3D12_CPU_PAGE_PROPERTY_UNKNOWN,
                               D3D12_MEMORY_POOL_UNKNOWN, 1, 1};
    D3D12_RESOURCE_DESC desc{};
    desc.Dimension = D3D12_RESOURCE_DIMENSION_TEXTURE2D;
    desc.Width = 400;
    desc.Height = 224;
    desc.DepthOrArraySize = desc.MipLevels = desc.SampleDesc.Count = 1;
    desc.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
    desc.Flags = D3D12_RESOURCE_FLAG_ALLOW_RENDER_TARGET;
    ID3D12Resource* raw_texture = nullptr;
    hr = native->CreateCommittedResource(&heap, D3D12_HEAP_FLAG_NONE, &desc,
        D3D12_RESOURCE_STATE_PRESENT, nullptr, IID_PPV_ARGS(&raw_texture));
    if (FAILED(hr) || !raw_texture) return fail("create source texture", hr);
    Com<ID3D12Resource> texture(raw_texture);

    struct {
        UINT64 token;
        UINT count;
        UINT padding;
        ID3D12Resource* const* resources;
    } plane{7, 1, 0, &raw_texture};
    if (present(queue.get(), 1, nullptr, nullptr) != E_INVALIDARG ||
        present(queue.get(), 2, &plane, nullptr) != E_NOTIMPL ||
        present(queue.get(), 1, &plane, &plane) != E_NOTIMPL || find_window()) {
        return fail("reject invalid and unsupported calls before creating a window");
    }
    hr = present(queue.get(), 1, &plane, nullptr);
    if (hr != S_OK) return fail("create flip chain and present source", hr);
    HWND window = find_window();
    RECT client{};
    if (!window || !GetClientRect(window, &client) ||
        client.right != static_cast<LONG>(desc.Width) || client.bottom != static_cast<LONG>(desc.Height)) {
        return fail("verify the presentation window and client dimensions");
    }
    ++plane.token;
    hr = present(queue.get(), 1, &plane, nullptr);
    if (hr != S_OK || find_window() != window) return fail("present again using the same window", hr);
    hr = present(queue.get(), 0, nullptr, nullptr);
    if (FAILED(hr) || IsWindow(window)) return fail("unbind and destroy the window", hr);
    std::puts("[t41] slot 30: one-plane texture copied to a DXGI flip chain twice");
    return 0;
}

}  // namespace present_test
