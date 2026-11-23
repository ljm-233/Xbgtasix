// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include <d3d9.h>
#include <d3d12.h>

#include <cstdio>
#include <cstring>

inline int check_frame_events(ID3D12Device* device, ID3D12Device* other) {
    using Slot = void (WINAPI*)();
    using SetInterval = HRESULT (WINAPI*)(ID3D12Device*, IUnknown*, UINT, UINT, UINT);
    using ScheduleEvent = HRESULT (WINAPI*)(ID3D12Device*, UINT, UINT, const void*, UINT);
    using WaitEvent = HRESULT (WINAPI*)(ID3D12Device*, UINT, UINT, const void*, UINT, UINT64*);

    const Slot* vtable;
    std::memcpy(&vtable, static_cast<void*>(device), sizeof(vtable));
    const auto set_interval = reinterpret_cast<SetInterval>(vtable[78]);
    const auto schedule = reinterpret_cast<ScheduleEvent>(vtable[79]);
    const auto wait = reinterpret_cast<WaitEvent>(vtable[80]);
    const auto fail = [](const char* check) {
        std::fprintf(stderr, "[t41] FAIL: %s\n", check);
        return 1;
    };

    constexpr UINT64 untouched = 0xfeedbabe;
    UINT64 token = untouched;
    if (FAILED(set_interval(device, nullptr, 20000, 3, 0)) ||
        FAILED(schedule(device, 0, 0, nullptr, 0)) ||
        FAILED(schedule(device, 2, 5000000, nullptr, 0))) {
        return fail("configure independent types 0 and 2");
    }
    if (wait(other, 0, 0, nullptr, 0, &token) != E_INVALIDARG || token != untouched)
        return fail("schedule must be local to the originating device");
    if (schedule(device, 0, 0, nullptr, 1) != E_NOTIMPL ||
        wait(device, 0, 0, nullptr, 1, &token) != E_NOTIMPL || token != untouched) {
        return fail("unsupported flags must leave the schedule and token intact");
    }
    if (wait(device, 2, 0, nullptr, 0, &token) != D3DERR_DEVICEREMOVED || token != untouched ||
        wait(device, 2, 5, nullptr, 0, &token) != D3DERR_DEVICEREMOVED || token != untouched) {
        return fail("poll and finite timeout must leave the token intact");
    }
    if (FAILED(wait(device, 0, 1000, nullptr, 0, &token)) || token == untouched || token == 0)
        return fail("type 2 must not move the deadline for type 0");

    const UINT64 first = token;
    if (FAILED(schedule(device, 2, 0, nullptr, 0)) ||
        FAILED(wait(device, 2, 1000, nullptr, 0, &token)) || token != first + 1) {
        return fail("a successful wait must issue exactly one new token");
    }
    if (FAILED(set_interval(device, nullptr, 0, 3, 0)) ||
        wait(device, 2, 0, nullptr, 0, &token) != E_INVALIDARG || token != first + 1) {
        return fail("disabling the interval must clear registered events");
    }
    std::puts("[t41] slots 78-80: independent event schedules, timeout and frame tokens");
    return 0;
}
