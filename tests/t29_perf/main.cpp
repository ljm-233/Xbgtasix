// t29_perf — 兼容层性能基准
//
// 只测**现在能测的**：
//   1) shim 调用开销（转发到系统 d3d12 vs 直接调用）
//   2) XMemAlloc/Free 吞吐 vs VirtualAlloc 基线
//   3) 任务队列提交+派发延迟
//   4) 通用异步（XAsyncRun）往返
//   5) 启动成本：读配置 + 461 个导出的 GetProcAddress
//
// 不测的（需要 exe）：真实游戏帧时间、资产流式加载、着色器管线编译。
#include <windows.h>
#include <d3d12.h>

#include <cstdio>
#include <cstring>
#include <vector>

#include "xbgtasix/trace.h"

extern "C" {
HRESULT WINAPI XGameRuntimeInitialize(void);
void* WINAPI XMemAlloc(uint64_t attributes, size_t size);
void WINAPI XMemFree(void* p);
HRESULT WINAPI XTaskQueueCreate(int work_mode, int comp_mode, void** queue);
HRESULT WINAPI XTaskQueueSubmitCallback(void* queue, int port, void* context, void* callback);
bool WINAPI XTaskQueueDispatch(void* queue, int port, unsigned timeout_ms);
void WINAPI XTaskQueueCloseHandle(void* queue);
HRESULT WINAPI XAsyncRun(void* async_block, void* work);
HRESULT WINAPI XAsyncGetStatus(void* async_block, bool wait);
}

namespace {

struct Blk {
    void* queue;
    void* context;
    void (*callback)(Blk*);
    unsigned internal[sizeof(void*) * 4 / sizeof(unsigned)];
};

LARGE_INTEGER g_freq{};
double now_ms() {
    LARGE_INTEGER t{};
    QueryPerformanceCounter(&t);
    return static_cast<double>(t.QuadPart) * 1000.0 / static_cast<double>(g_freq.QuadPart);
}

void CALLBACK noop(void*, bool) {}
HRESULT CALLBACK async_noop(Blk*) { return S_OK; }

struct Result {
    const char* name;
    double value;
    const char* unit;
};

std::vector<Result> g_results;
int g_fail = 0;
int g_checks = 0;

// 这个测试以前一条断言都没有、无条件 return 0 —— 在 run_suite 里永远是绿。
// 现在它也用同一套 ok()：基准数字必须有"可证伪"的期望值兜着。
void ok(bool cond, const char* what, const char* detail = "") {
    ++g_checks;
    if (cond) {
        xbgtasix::logf("[t29]   PASS  %-44s %s", what, detail);
    } else {
        xbgtasix::logf("[t29]   FAIL  %-44s %s", what, detail);
        ++g_fail;
    }
}

void record(const char* name, double value, const char* unit) {
    g_results.push_back(Result{name, value, unit});
    xbgtasix::logf("[t29] %-40s %10.3f %s", name, value, unit);
}

}  // namespace

int main(int argc, char** argv) {
    QueryPerformanceFrequency(&g_freq);
    if (argc > 1) SetEnvironmentVariableA("XBGTASIX_CONFIG", argv[1]);

    // ---- 5a) 启动：读配置 ----
    {
        const double t0 = now_ms();
        XGameRuntimeInitialize();
        record("启动：XGameRuntimeInitialize（含读配置）", now_ms() - t0, "ms");
    }

    HMODULE self = GetModuleHandleA("xgameruntime.dll");
    HMODULE d3d12 = LoadLibraryA("d3d12.dll");

    // ---- 5b) 启动：解析全部导出 ----
    // 以前 self 为空就整段静默跳过，测试照样 PASS。
    ok(self != nullptr, "xgameruntime.dll 已在本进程里（否则导出基准没有意义）");
    if (self) {
        IMAGE_DOS_HEADER* dos = reinterpret_cast<IMAGE_DOS_HEADER*>(self);
        IMAGE_NT_HEADERS* nt = reinterpret_cast<IMAGE_NT_HEADERS*>(
            reinterpret_cast<unsigned char*>(self) + dos->e_lfanew);
        auto* dir = &nt->OptionalHeader.DataDirectory[IMAGE_DIRECTORY_ENTRY_EXPORT];
        auto* exp = reinterpret_cast<IMAGE_EXPORT_DIRECTORY*>(
            reinterpret_cast<unsigned char*>(self) + dir->VirtualAddress);
        auto* names = reinterpret_cast<DWORD*>(reinterpret_cast<unsigned char*>(self) +
                                               exp->AddressOfNames);
        const double t0 = now_ms();
        int found = 0;
        for (DWORD i = 0; i < exp->NumberOfNames; ++i) {
            const char* n = reinterpret_cast<const char*>(reinterpret_cast<unsigned char*>(self) +
                                                          names[i]);
            if (GetProcAddress(self, n)) ++found;
        }
        const double dt = now_ms() - t0;
        record("启动：解析全部导出（GetProcAddress）", dt, "ms");
        record("导出数量", static_cast<double>(found), "个");
        record("人均导出解析成本", dt * 1000.0 / (found ? found : 1), "us");
        // 期望值不是实测值：导出表自己声明了多少个名字，就必须解析出多少个。
        // 少一个都说明有导出"在表里但拿不到"，那是真的坏。
        char detail[80];
        _snprintf(detail, sizeof(detail), "found=%d 表内名字=%lu", found,
                  static_cast<unsigned long>(exp->NumberOfNames));
        ok(found == static_cast<int>(exp->NumberOfNames),
           "解析出的导出数 == 导出表声明数", detail);
        ok(found > 400, "导出数 > 400（本模块规模的事实基线，少到这个数以下要查）", detail);
    }

    // ---- 1) 转发开销：拿一个无副作用的转发导出量调用成本 ----
    // 不用 D3D12CreateDevice：传 nullptr 作为 out 指针时驱动行为不确定（实测会挂住）。
    // D3D12EnableExperimentalFeatures 什么都不做，只测"进 shim -> GetProcAddress -> 真函数"的成本。
    if (d3d12) {
        using PFN = HRESULT(WINAPI*)(unsigned, const void*, void*, unsigned*);
        auto real = reinterpret_cast<PFN>(GetProcAddress(d3d12, "D3D12EnableExperimentalFeatures"));
        ok(real != nullptr, "系统 d3d12.dll 导出 D3D12EnableExperimentalFeatures（转发基准的前提）");
        if (real) {
            const int N = 200000;
            double t0 = now_ms();
            volatile HRESULT sink = 0;
            for (int i = 0; i < N; ++i) sink = real(0, nullptr, nullptr, nullptr);
            const double direct = (now_ms() - t0) * 1000.0 / N;
            t0 = now_ms();
            for (int i = 0; i < N; ++i) sink = D3D12EnableExperimentalFeatures(0, nullptr, nullptr, nullptr);
            const double viaShim = (now_ms() - t0) * 1000.0 / N;
            (void)sink;
            record("转发：系统 d3d12 直调", direct, "us/次");
            record("转发：经 d3d12_xs shim", viaShim, "us/次");
            record("转发：shim 额外开销", viaShim - direct, "us/次");
            // 性能回归线：shim 只是 GetProcAddress + 一次跳转，不该比直调慢一个量级。
            // 50 是字面量门槛（实测约 1.1 倍，留足噪声余量）。
            char detail[64];
            _snprintf(detail, sizeof(detail), "%.2fx（门槛 50x）", direct > 0 ? viaShim / direct : 0.0);
            ok(direct > 0.0 && viaShim < direct * 50.0, "shim 转发开销 < 直调的 50 倍", detail);
        }
    }

    // ---- 2) XMemAlloc/Free vs VirtualAlloc 基线 ----
    {
        const int N = 50000;
        const size_t size = 4096;
        double t0 = now_ms();
        for (int i = 0; i < N; ++i) {
            void* p = XMemAlloc(0, size);
            if (p) XMemFree(p);
        }
        const double shim = (now_ms() - t0);
        t0 = now_ms();
        for (int i = 0; i < N; ++i) {
            void* p = VirtualAlloc(nullptr, size, MEM_COMMIT | MEM_RESERVE, PAGE_READWRITE);
            if (p) VirtualFree(p, 0, MEM_RELEASE);
        }
        const double native = (now_ms() - t0);
        record("XMemAlloc/Free 组合", shim * 1000.0 / N, "us/次");
        record("VirtualAlloc/Free 基线", native * 1000.0 / N, "us/次");
        record("XMem 相对开销倍数", shim / (native > 0 ? native : 1), "x");
        record("XMem 吞吐", N / (shim / 1000.0), "次/秒");
    }

    // ---- 3) 任务队列：提交 + 手动派发 ----
    {
        void* q = nullptr;
        const HRESULT qhr = XTaskQueueCreate(0, 0, &q);
        ok(SUCCEEDED(qhr) && q != nullptr, "XTaskQueueCreate（Manual 模式）");
        if (SUCCEEDED(qhr) && q) {
            const int N = 20000;
            const double t0 = now_ms();
            for (int i = 0; i < N; ++i) {
                XTaskQueueSubmitCallback(q, 0, nullptr, reinterpret_cast<void*>(&noop));
            }
            const double submit = (now_ms() - t0) * 1000.0 / N;
            const double t1 = now_ms();
            int drained = 0;
            while (XTaskQueueDispatch(q, 0, 0)) ++drained;
            const double dispatch = (now_ms() - t1) * 1000.0 / (drained ? drained : 1);
            record("队列：提交一次回调", submit, "us/次");
            record("队列：派发一次回调", dispatch, "us/次");
            record("队列：实际派发数量", static_cast<double>(drained), "个");
            // 期望值是字面量：提交了 20000 个就必须派发出 20000 个（少一个都是丢任务）
            char detail[64];
            _snprintf(detail, sizeof(detail), "drained=%d", drained);
            ok(drained == 20000, "队列派发数量 == 提交的 20000", detail);
            XTaskQueueCloseHandle(q);
        }
    }

    // ---- 4) 异步往返 ----
    {
        const int N = 2000;
        const double t0 = now_ms();
        int done = 0;
        for (int i = 0; i < N; ++i) {
            Blk b{};
            if (SUCCEEDED(XAsyncRun(&b, reinterpret_cast<void*>(&async_noop)))) {
                if (XAsyncGetStatus(&b, true) == S_OK) ++done;
            }
        }
        const double dt = (now_ms() - t0) * 1000.0 / N;
        record("异步：XAsyncRun + wait 往返", dt, "us/次");
        record("异步：完成数量", static_cast<double>(done), "个");
        // 期望值字面量 2000：一次往返失败都不许有
        char detail[64];
        _snprintf(detail, sizeof(detail), "done=%d", done);
        ok(done == 2000, "异步往返完成数量 == 2000（一次都不许失败）", detail);
    }

    xbgtasix::logf("[t29] ---- 汇总 ----");
    for (const auto& r : g_results) {
        xbgtasix::logf("[t29] %s = %.3f %s", r.name, r.value, r.unit);
    }
    xbgtasix::logf("[t29] done checks=%d failures=%d -> %s", g_checks, g_fail,
                   g_fail ? "FAIL" : "PASS");
    return g_fail ? 1 : 0;
}
