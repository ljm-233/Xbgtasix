// system_ui.cpp —— 系统侧 UI 的实现：Win32 窗口 + D3D12 flip 交换链 + 原生子控件
//
// 为什么这么组合：
//   - 窗口与 flip 交换链这条路，t31_swapchain / t32_window 已经验证过（含 ResizeBuffers）
//   - 文字渲染要从零做字体（纹理图集、字形、排版），先不做；
//     改用 STATIC / EDIT / BUTTON 原生子控件，它们自己会画字、也会处理输入
//   - 于是"系统侧 UI"这一步既有真实观感，又不引入未验证的东西
//
// 无桌面/无 GPU 时（CI、服务会话）必须**优雅降级**：system_ui_available() 返回 false，
// 调用方按"不接管"处理，而不是卡死。
#include "system_ui.h"

#include <windows.h>
#include <d3d12.h>
#include <dxgi1_6.h>

#include <atomic>
#include <functional>
#include <cstdio>
#include <cstring>
#include <mutex>

#include "xbgtasix/trace.h"

namespace xbgtasix {
namespace {

constexpr const wchar_t* kClass = L"xbgtasix_system_ui";

struct UiState {
    HWND hwnd = nullptr;
    HWND buttons[3] = {nullptr, nullptr, nullptr};
    HWND edit = nullptr;
    int clicked = -1;          // First/Second/Third，或 -1
    bool done = false;
    std::string text;
    uint32_t deadline_ms = 0;
    bool text_mode = false;
    int default_button = 1;
    int cancel_button = 1;
};

std::mutex g_mu;
UiState* g_state = nullptr;    // 仅窗口线程使用；用指针是因为 WndProc 是 C 回调

// ---- 子类化窗口过程：处理按钮点击 ----
LRESULT CALLBACK ui_proc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp) {
    UiState* st = reinterpret_cast<UiState*>(GetWindowLongPtrA(hwnd, GWLP_USERDATA));
    switch (msg) {
        case WM_COMMAND: {
            const int id = LOWORD(wp);
            const int code = HIWORD(wp);
            // 只认真正的点击。之前不判断通知码，EDIT 创建时发的 EN_CHANGE
            // 被误当成"用户点了确定"，导致还没等人操作就把控件初始文本当结果返回。
            if (st && id >= 100 && id <= 102 && code == BN_CLICKED) {
                st->clicked = id - 100;
                st->done = true;
                return 0;
            }
            if (st && id == 201 && code == BN_CLICKED && st->edit) {
                // 用宽字符读，再转 UTF-8：GetWindowTextA 在非中文系统上会把中文变成 ??
                wchar_t wbuf[512] = {0};
                GetWindowTextW(st->edit, wbuf, 511);
                const int bytes = WideCharToMultiByte(CP_UTF8, 0, wbuf, -1, nullptr, 0, nullptr, nullptr);
                if (bytes > 1) {
                    std::string out(bytes - 1, 0);
                    WideCharToMultiByte(CP_UTF8, 0, wbuf, -1, out.data(), bytes, nullptr, nullptr);
                    st->text = out;
                }
                st->done = true;
                return 0;
            }
            break;
        }
        case WM_CLOSE:
            if (st) {
                st->clicked = st->cancel_button >= 0 ? st->cancel_button : -1;
                st->done = true;
            }
            return 0;
        case WM_DESTROY:
            PostQuitMessage(0);
            return 0;
        default:
            break;
    }
    return DefWindowProcW(hwnd, msg, wp, lp);
}

// 读环境变量里的"脚本化回答"，供自动化测试用（没人点鼠标也能验端到端）
// 读环境变量并转成 UTF-8。
// 注意：不能用 GetEnvironmentVariableA —— 非 ASCII 内容在 SetEnvironmentVariableA
// 那一步就会失败（实测返回 0），中文根本传不进来。统一走宽字符 + CP_UTF8。
std::string env_utf8(const wchar_t* name) {
    wchar_t wbuf[512] = {0};
    const DWORD n = GetEnvironmentVariableW(name, wbuf, 511);
    if (n == 0) return std::string();
    const int bytes = WideCharToMultiByte(CP_UTF8, 0, wbuf, -1, nullptr, 0, nullptr, nullptr);
    if (bytes <= 1) return std::string();
    // 同上：先按容量（含结尾 0）分配，写完再把长度收回来
    std::string out(static_cast<size_t>(bytes), 0);
    WideCharToMultiByte(CP_UTF8, 0, wbuf, -1, out.data(), bytes, nullptr, nullptr);
    out.resize(static_cast<size_t>(bytes - 1));
    return out;
}

bool auto_response(int* button, std::string* text) {
    const std::string value = env_utf8(L"XBGTASIX_UI_AUTORESPONSE");
    const char* buf = value.c_str();
    const DWORD n = static_cast<DWORD>(value.size());
    xbgtasix::logf("[sysui] 自动回答环境变量: n=%lu \"%s\"", n, buf);
    if (n == 0) return false;
    if (_stricmp(buf, "first") == 0) { *button = 0; return true; }
    if (_stricmp(buf, "second") == 0) { *button = 1; return true; }
    if (_stricmp(buf, "third") == 0) { *button = 2; return true; }
    if (text) *text = buf;    // 其它内容当作输入框的回答
    *button = -2;
    return true;
}

// ---- D3D12：设备 + 队列 + flip 交换链，画一帧背景 ----
struct Gfx {
    ID3D12Device* dev = nullptr;
    ID3D12CommandQueue* queue = nullptr;
    IDXGISwapChain1* swap = nullptr;
    ID3D12CommandAllocator* alloc = nullptr;
    ID3D12GraphicsCommandList* list = nullptr;
    ID3D12Resource* buffers[2] = {nullptr, nullptr};
    ID3D12DescriptorHeap* rtv_heap = nullptr;
    ID3D12Fence* fence = nullptr;
    HANDLE fence_event = nullptr;
    UINT64 fence_value = 0;

    bool init(HWND hwnd, UINT w, UINT h) {
        if (FAILED(D3D12CreateDevice(nullptr, D3D_FEATURE_LEVEL_11_0, IID_ID3D12Device,
                                     reinterpret_cast<void**>(&dev))) || !dev) {
            return false;
        }
        D3D12_COMMAND_QUEUE_DESC qd{};
        qd.Type = D3D12_COMMAND_LIST_TYPE_DIRECT;
        if (FAILED(dev->CreateCommandQueue(&qd, IID_ID3D12CommandQueue,
                                          reinterpret_cast<void**>(&queue)))) return false;

        IDXGIFactory2* factory = nullptr;
        if (FAILED(CreateDXGIFactory2(0, IID_IDXGIFactory2, reinterpret_cast<void**>(&factory))) ||
            !factory) return false;
        DXGI_SWAP_CHAIN_DESC1 sd{};
        sd.Width = w;
        sd.Height = h;
        sd.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
        sd.SampleDesc.Count = 1;
        sd.BufferUsage = DXGI_USAGE_RENDER_TARGET_OUTPUT;
        sd.BufferCount = 2;
        sd.SwapEffect = DXGI_SWAP_EFFECT_FLIP_DISCARD;
        sd.AlphaMode = DXGI_ALPHA_MODE_IGNORE;
        const HRESULT hr = factory->CreateSwapChainForHwnd(queue, hwnd, &sd, nullptr, nullptr, &swap);
        factory->Release();
        if (FAILED(hr) || !swap) return false;

        D3D12_DESCRIPTOR_HEAP_DESC hd{};
        hd.Type = D3D12_DESCRIPTOR_HEAP_TYPE_RTV;
        hd.NumDescriptors = 2;
        if (FAILED(dev->CreateDescriptorHeap(&hd, IID_ID3D12DescriptorHeap,
                                             reinterpret_cast<void**>(&rtv_heap)))) return false;
        for (UINT i = 0; i < 2; ++i) {
            if (FAILED(swap->GetBuffer(i, IID_ID3D12Resource,
                                       reinterpret_cast<void**>(&buffers[i])))) return false;
        }
        if (FAILED(dev->CreateCommandAllocator(D3D12_COMMAND_LIST_TYPE_DIRECT,
                                               IID_ID3D12CommandAllocator,
                                               reinterpret_cast<void**>(&alloc)))) return false;
        if (FAILED(dev->CreateCommandList(0, D3D12_COMMAND_LIST_TYPE_DIRECT, alloc, nullptr,
                                          IID_ID3D12GraphicsCommandList,
                                          reinterpret_cast<void**>(&list)))) return false;
        list->Close();
        dev->CreateFence(0, D3D12_FENCE_FLAG_NONE, IID_ID3D12Fence,
                         reinterpret_cast<void**>(&fence));
        fence_event = CreateEventA(nullptr, FALSE, FALSE, nullptr);
        return true;
    }

    // 画一帧纯色背景（真正的面板内容由子控件负责）
    void present_frame(float r, float g, float b) {
        if (!list || !swap || !rtv_heap) return;
        IDXGISwapChain3* s3 = nullptr;
        swap->QueryInterface(IID_IDXGISwapChain3, reinterpret_cast<void**>(&s3));
        const UINT idx = s3 ? s3->GetCurrentBackBufferIndex() : 0;
        ID3D12Resource* target = buffers[idx & 1];
        if (!target) { if (s3) s3->Release(); return; }

        const UINT inc = dev->GetDescriptorHandleIncrementSize(D3D12_DESCRIPTOR_HEAP_TYPE_RTV);
        D3D12_CPU_DESCRIPTOR_HANDLE rtv = rtv_heap->GetCPUDescriptorHandleForHeapStart();
        rtv.ptr += (idx & 1) * inc;
        dev->CreateRenderTargetView(target, nullptr, rtv);

        alloc->Reset();
        list->Reset(alloc, nullptr);
        D3D12_RESOURCE_BARRIER bar{};
        bar.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
        bar.Transition.pResource = target;
        bar.Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
        bar.Transition.StateBefore = D3D12_RESOURCE_STATE_PRESENT;
        bar.Transition.StateAfter = D3D12_RESOURCE_STATE_RENDER_TARGET;
        list->ResourceBarrier(1, &bar);
        const float color[4] = {r, g, b, 1.0f};
        list->ClearRenderTargetView(rtv, color, 0, nullptr);
        bar.Transition.StateBefore = D3D12_RESOURCE_STATE_RENDER_TARGET;
        bar.Transition.StateAfter = D3D12_RESOURCE_STATE_PRESENT;
        list->ResourceBarrier(1, &bar);
        list->Close();
        ID3D12CommandList* cls[] = {list};
        queue->ExecuteCommandLists(1, cls);
        swap->Present(1, 0);

        // 等这一帧做完再进下一帧（否则重置 allocator 会 DEVICE_REMOVED，见 graphics-pitfalls 9）
        if (fence) {
            ++fence_value;
            queue->Signal(fence, fence_value);
            if (fence->GetCompletedValue() < fence_value) {
                fence->SetEventOnCompletion(fence_value, fence_event);
                WaitForSingleObject(fence_event, 2000);
            }
        }
        if (s3) s3->Release();
    }

    void shutdown() {
        if (fence_event) CloseHandle(fence_event);
        if (fence) fence->Release();
        for (auto* b : buffers) if (b) b->Release();
        if (rtv_heap) rtv_heap->Release();
        if (list) list->Release();
        if (alloc) alloc->Release();
        if (swap) swap->Release();
        if (queue) queue->Release();
        if (dev) dev->Release();
    }
};

Gfx g_gfx;                 // 单实例：系统侧 UI 同时只会有一个面板

bool ensure_class() {
    static bool tried = false;
    static bool ok = false;
    if (tried) return ok;
    tried = true;
    WNDCLASSEXW wc{};
    wc.cbSize = sizeof(wc);
    wc.lpfnWndProc = ui_proc;
    wc.hInstance = GetModuleHandleW(nullptr);
    wc.lpszClassName = kClass;
    // IDC_ARROW 用数字常量，避免未定义 UNICODE 时宏展开成 ANSI 版本
    wc.hCursor = LoadCursorW(nullptr, MAKEINTRESOURCEW(32512));
    wc.hbrBackground = reinterpret_cast<HBRUSH>(COLOR_WINDOW + 1);
    const ATOM a = RegisterClassExW(&wc);
    ok = (a != 0) || GetLastError() == ERROR_CLASS_ALREADY_EXISTS;
    return ok;
}

// 统一的"开窗 -> 摆控件 -> 跑消息循环 -> 收尾"流程
bool run_panel(UiState& st, const wchar_t* title, std::function<void(HWND)> build_controls,
               uint32_t timeout_ms) {
    (void)build_controls;
    if (!ensure_class()) {
        xbgtasix::logf("[sysui] 无法注册窗口类 -> 降级");
        return false;
    }
    st.deadline_ms = GetTickCount() + (timeout_ms ? timeout_ms : 60000);

    const UINT w = 720, h = 420;
    HWND hwnd = CreateWindowExW(WS_EX_TOPMOST, kClass, title, WS_OVERLAPPED | WS_CAPTION |
                                    WS_SYSMENU | WS_VISIBLE,
                                CW_USEDEFAULT, CW_USEDEFAULT, w, h, nullptr, nullptr,
                                GetModuleHandleW(nullptr), nullptr);
    if (!hwnd) {
        xbgtasix::logf("[sysui] CreateWindowEx 失败 err=%lu -> 降级", GetLastError());
        return false;
    }
    st.hwnd = hwnd;
    SetWindowLongPtrA(hwnd, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(&st));

    // 背景用 D3D12 flip 交换链画（验证过的路径）；失败也不致命，窗口本身还能显示控件
    const bool gfx_ok = g_gfx.init(hwnd, w, h);
    if (gfx_ok) g_gfx.present_frame(0.10f, 0.11f, 0.14f);
    else xbgtasix::logf("[sysui] 交换链不可用，只有子控件（不致命）");

    build_controls(hwnd);
    SetForegroundWindow(hwnd);

    // 脚本化回答（自动化测试用）：直接当作"已点击"
    int auto_btn = -3;
    std::string auto_text;
    const bool have_auto = auto_response(&auto_btn, &auto_text);
    if (have_auto && auto_btn >= 0) st.clicked = auto_btn;
    if (have_auto && auto_btn == -2) { st.text = auto_text; }

    MSG msg{};
    while (!st.done) {
        while (PeekMessageW(&msg, nullptr, 0, 0, PM_REMOVE)) {
            TranslateMessage(&msg);
            DispatchMessageW(&msg);
        }
        if (have_auto) { st.done = true; break; }
        if (GetTickCount() > st.deadline_ms) {
            xbgtasix::logf("[sysui] 面板超时，按取消处理");
            st.clicked = st.cancel_button >= 0 ? st.cancel_button : -1;
            break;
        }
        Sleep(10);
    }
    if (!st.text_mode && st.clicked < 0) st.clicked = -1;

    DestroyWindow(hwnd);
    if (gfx_ok) g_gfx.shutdown();
    st.hwnd = nullptr;
    return true;
}

void make_button(HWND parent, UiState& st, int i, const wchar_t* label, int x) {
    if (!label) return;
    st.buttons[i] = CreateWindowExW(0, L"BUTTON", label, WS_CHILD | WS_VISIBLE | BS_PUSHBUTTON, x,
                                   320, 180, 40, parent,
                                   reinterpret_cast<HMENU>(static_cast<INT_PTR>(100 + i)),
                                   GetModuleHandleW(nullptr), nullptr);
}

std::wstring widen(const char* s) {
    if (!s) return L"";
    const int n = MultiByteToWideChar(CP_UTF8, 0, s, -1, nullptr, 0);
    std::wstring out(static_cast<size_t>(n > 0 ? n : 1), L'\0');
    if (n > 1) MultiByteToWideChar(CP_UTF8, 0, s, -1, out.data(), n);
    out.resize(n > 0 ? static_cast<size_t>(n - 1) : 0);
    return out;
}

}  // namespace

bool system_ui_available() {
    // 无交互桌面（服务会话）时别开窗，直接降级
    const HWINSTA ws = GetProcessWindowStation();
    if (ws) {
        USEROBJECTFLAGS uof{};
        DWORD needed = 0;
        if (GetUserObjectInformationA(ws, UOI_FLAGS, &uof, sizeof(uof), &needed) &&
            (uof.dwFlags & WSF_VISIBLE) == 0) {
            return false;
        }
    }
    return true;
}

SysUiMessageButton system_ui_show_message(const char* title, const char* content, const char* b1,
                                          const char* b2, const char* b3, int default_button,
                                          int cancel_button, uint32_t timeout_ms) {
    UiState st{};
    st.default_button = default_button;
    st.cancel_button = cancel_button;

    const std::wstring wt = widen(title && *title ? title : "Xbgtasix");
    auto build = [&](HWND hwnd) {
        if (content) {
            CreateWindowExW(0, L"STATIC", widen(content).c_str(), WS_CHILD | WS_VISIBLE, 20, 20,
                            660, 260, hwnd, nullptr, GetModuleHandleW(nullptr), nullptr);
        }
        int x = 20;
        make_button(hwnd, st, 0, b1 ? widen(b1).c_str() : nullptr, x);
        if (b1) x += 200;
        make_button(hwnd, st, 1, b2 ? widen(b2).c_str() : nullptr, x);
        if (b2) x += 200;
        make_button(hwnd, st, 2, b3 ? widen(b3).c_str() : nullptr, x);
    };

    if (!run_panel(st, wt.c_str(), build, timeout_ms)) return SysUiMessageButton::None;
    if (st.clicked < 0) return SysUiMessageButton::None;
    xbgtasix::logf("[sysui] 用户点了第 %d 个按钮（标题: %s）", st.clicked + 1, title ? title : "");
    return static_cast<SysUiMessageButton>(st.clicked);
}

bool system_ui_show_text_entry(const char* title, const char* description, const char* default_text,
                               uint32_t max_len, std::string* out, uint32_t timeout_ms) {
    UiState st{};
    st.text_mode = true;

    const std::wstring wt = widen(title && *title ? title : "Xbgtasix");
    auto build = [&](HWND hwnd) {
        if (description) {
            CreateWindowExW(0, L"STATIC", widen(description).c_str(), WS_CHILD | WS_VISIBLE, 20, 20,
                            660, 24, hwnd, nullptr, GetModuleHandleW(nullptr), nullptr);
        }
        st.edit = CreateWindowExW(WS_EX_CLIENTEDGE, L"EDIT", widen(default_text).c_str(),
                                  WS_CHILD | WS_VISIBLE | WS_TABSTOP | ES_AUTOHSCROLL, 20, 60, 660,
                                  28, hwnd, reinterpret_cast<HMENU>(static_cast<INT_PTR>(200)),
                                  GetModuleHandleW(nullptr), nullptr);
        if (st.edit && max_len) {
            SendMessageW(st.edit, EM_SETLIMITTEXT, static_cast<WPARAM>(max_len), 0);
            SetFocus(st.edit);
        }
        // 必须有显式的确定按钮：不能靠 EDIT 的通知当确认（EN_CHANGE 会在创建时就发）
        CreateWindowExW(0, L"BUTTON", L"确定", WS_CHILD | WS_VISIBLE | BS_DEFPUSHBUTTON, 600, 100,
                        80, 30, hwnd, reinterpret_cast<HMENU>(static_cast<INT_PTR>(201)),
                        GetModuleHandleW(nullptr), nullptr);
    };

    xbgtasix::logf("[sysui] 输入框: 标题=\"%s\" 默认=\"%s\"", title ? title : "",
                   default_text ? default_text : "");
    if (!run_panel(st, wt.c_str(), build, timeout_ms)) {
        xbgtasix::logf("[sysui] 输入框: 面板没跑起来 -> 降级");
        return false;
    }
    xbgtasix::logf("[sysui] 输入框: 面板结束，结果长度=%zu", st.text.size());
    if (st.text.empty()) return false;
    if (out) *out = st.text;
    xbgtasix::logf("[sysui] 用户输入: %s", st.text.c_str());
    return true;
}

}  // namespace xbgtasix
