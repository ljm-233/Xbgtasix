// xbgtasix: 日志 + 未实现 API 策略
#include "xbgtasix/trace.h"

#include <windows.h>
#include <winternl.h>   // RtlCaptureStackBackTrace

#include <mutex>
#include <set>
#include <cstring>
#include <string>

namespace xbgtasix {
namespace {

std::mutex g_mu;
std::set<std::string> g_seen;
char g_root[MAX_PATH] = {0};

// 用 Win32 句柄 + FILE_APPEND_DATA 打开：每次 WriteFile 一整行，
// 在这一个句柄上原子追加（fprintf 走 stdio 缓冲，多模块并发时会切碎行）。
// exe 目录不可写时（Program Files / Game Pass 安装目录）回落到 %LOCALAPPDATA%。
HANDLE g_handle = INVALID_HANDLE_VALUE;
bool g_open_failed = false;

bool try_open(const char* path) {
    g_handle = CreateFileA(path, FILE_APPEND_DATA, FILE_SHARE_READ | FILE_SHARE_WRITE, nullptr,
                           OPEN_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
    return g_handle != INVALID_HANDLE_VALUE;
}

void ensure_open() {
    if (g_handle != INVALID_HANDLE_VALUE || g_open_failed) return;
    char path[MAX_PATH] = {0};
    DWORD n = GetEnvironmentVariableA("XBGTASIX_LOG", path, MAX_PATH);
    if (n > 0) {
        if (try_open(path)) return;
        // 显式路径打不开时不能直接放弃：调用方（启动器）可能给了个相对/不可写路径。
        // 继续走下面的回落链，至少还能落一份日志。
        fprintf(stderr, "[xbgtasix] 警告：XBGTASIX_LOG=%s 打不开，改用回落路径\n", path);
    }
    char exe[MAX_PATH] = {0};
    GetModuleFileNameA(nullptr, exe, MAX_PATH);
    if (char* slash = strrchr(exe, '\\')) *(slash + 1) = 0;
    _snprintf(path, MAX_PATH, "%sxbgtasix.log", exe);
    if (try_open(path)) return;

    char base[MAX_PATH] = {0};
    const DWORD bn = GetEnvironmentVariableA("XBGTASIX_PLS_ROOT", base, MAX_PATH);
    if (bn > 0) {
        _snprintf(path, MAX_PATH, "%s\\xbgtasix.log", base);
    } else if (GetEnvironmentVariableA("LOCALAPPDATA", base, MAX_PATH) > 0) {
        _snprintf(path, MAX_PATH, "%s\\Xbgtasix\\xbgtasix.log", base);
        // 目录可能不存在：逐级建一次，失败就放弃落盘（stderr 仍有输出）
        std::string d = std::string(base) + "\\Xbgtasix";
        CreateDirectoryA(d.c_str(), nullptr);
    }
    if (!try_open(path)) g_open_failed = true;
}

// ---------- 栈帧符号化 ----------
// ASLR 下裸地址每重启一次就变、没有 PDB 时根本没法定位"谁调了未实现 API"。
// 把帧换算成 模块文件名+0xRVA 之后，只要事后对同一份 exe/dll 跑 objdump -d，
// 就能把 RVA 对回具体代码位置。
constexpr DWORD kNameW = 512;      // 模块全路径（宽字符）缓冲
constexpr size_t kNameU8 = 1600;   // UTF-8 文件名缓冲（最坏 3 字节/宽字符 + '\0'）

// 取模块的"仅文件名"（UTF-8）。日志里不需要长路径，而且各机器安装路径不同，
// 只有文件名在排障时才有意义。
bool module_short_name_utf8(HMODULE mod, char* out, size_t out_size) {
    if (!mod || !out || out_size == 0) return false;
    wchar_t wpath[kNameW] = {0};
    const DWORD nw = GetModuleFileNameW(mod, wpath, kNameW);
    if (nw == 0) return false;          // 句柄无效/路径取不到：交给调用方兜底
    if (nw >= kNameW) return false;     // 路径过长被截断：丢了文件名还不如直接兜底
    const DWORD len = nw;               // 未截断时 wpath[nw] 已经是 '\0'
    // 只保留文件名：同时认 '\\' 和 '/'，别的地方灌进来的路径可能是混合分隔符
    const wchar_t* base = wpath;
    for (DWORD i = 0; i < len; ++i) {
        if (wpath[i] == L'\\' || wpath[i] == L'/') base = wpath + i + 1;
    }
    if (*base == L'\0') return false;
    // 宽字符 -> UTF-8（项目日志统一 UTF-8）。cchWideChar 传 -1 让 API 自己算长度，
    // 不要手工按 n-1 传，否则会掉最后一个字符；返 byte 数含结尾 '\0'，直接可用。
    const int nb = WideCharToMultiByte(CP_UTF8, 0, base, -1, out, static_cast<int>(out_size),
                                       nullptr, nullptr);
    if (nb <= 0) return false;   // 缓冲不足（名字过长）时同样走兜底
    return true;
}

// 打一帧：#<序号> <模块文件名>+0x<RVA>；解析不到模块时打 <unknown>+0x<原始地址>。
// 注意：本函数不持有 g_mu —— logf 内部自己加锁，外面再套一把会死锁。
void log_frame(unsigned index, void* addr) {
    const uintptr_t u = reinterpret_cast<uintptr_t>(addr);
    if (addr) {
        HMODULE mod = nullptr;
        // GetModuleHandleExW 第一参数声明是 LPCWSTR，但 FROM_ADDRESS 模式下传的其实是
        // "地址"而不是字符串；按 Windows API 惯例直接 reinterpret_cast 强转（API 内部
        // 不会解引用它），这也是微软文档示例的标准写法。
        const BOOL ok = GetModuleHandleExW(
            GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
            reinterpret_cast<LPCWSTR>(addr), &mod);
        if (ok && mod) {
            char name[kNameU8] = {0};
            if (module_short_name_utf8(mod, name, sizeof(name))) {
                logf("[xbgtasix]   #%-2u %s+0x%llX", index, name,
                     static_cast<unsigned long long>(u - reinterpret_cast<uintptr_t>(mod)));
                return;
            }
        }
    }
    // 解析不到模块（JIT 生成代码、手工映射的内存、句柄获取失败）：保留原始地址，
    // 至少还能在调试器里对着地址下断点；这里不 return，每一帧都要打出去。
    logf("[xbgtasix]   #%-2u <unknown>+0x%llX", index, static_cast<unsigned long long>(u));
}

// 主模块基址：事后用 objdump 把上面的 RVA 映射回代码时要用。
// 只打主模块，枚举全部模块会让日志长得没法看。
void log_main_module_base() {
    HMODULE exe = GetModuleHandleW(nullptr);
    if (!exe) return;
    char name[kNameU8] = {0};
    if (!module_short_name_utf8(exe, name, sizeof(name))) strcpy(name, "<unknown>");
    logf("[xbgtasix]   模块基址: %s=0x%016llX", name,
         static_cast<unsigned long long>(reinterpret_cast<uintptr_t>(exe)));
}

}  // namespace

UnimplMode unimpl_mode() {
    char buf[16] = {0};
    DWORD n = GetEnvironmentVariableA("XBGTASIX_UNIMPL", buf, sizeof(buf));
    if (n > 0 && _stricmp(buf, "log") == 0) return UnimplMode::Log;
    return UnimplMode::Stop;
}

void maybe_install_veh();   // 定义在下方

void logf(const char* fmt, ...) {
    // 第一次写日志时装异常钩子：它必须在"游戏还没调用任何 shim 初始化"时就已经就位，
    // 否则游戏在更早的位置崩溃就抓不到（实测挂在 set_game_root 上装，游戏没走到那）。
    maybe_install_veh();
    // 先在锁外把整行格式化好，再在锁内一次 WriteFile：
    // 原来持锁 fprintf+fflush，stderr 若接的是无人读的管道会整段卡死
    char line[2048];
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(line, sizeof(line), fmt, ap);
    va_end(ap);
    const size_t len = strlen(line);

    fwrite(line, 1, len, stderr);
    fputc('\n', stderr);
    fflush(stderr);   // stderr 本来就不该在锁内 flush

    std::lock_guard<std::mutex> lock(g_mu);
    ensure_open();
    if (g_handle != INVALID_HANDLE_VALUE) {
        DWORD written = 0;
        char buf[2064];
        const int n = _snprintf(buf, sizeof(buf), "%s\n", line);
        WriteFile(g_handle, buf, static_cast<DWORD>(n > 0 ? n : 0), &written, nullptr);
    }
}

// 环境变量只读一次：桩是热路径，而且运行期改 env 会让行为不确定
UnimplMode unimpl_mode_cached() {
    static const UnimplMode mode = unimpl_mode();
    return mode;
}

// ---------------------------------------------------------------------------
// 异常钩子：把首次致命异常（断言断点/非法访问）的地址符号化成 模块+RVA。
// 没有它，游戏崩了只会给一个退出码；有了它可以直接 objdump 出是哪个函数。
// 用 XBGTASIX_VEH=1 打开（默认关闭：它改变异常处理路径）。
// ---------------------------------------------------------------------------
LONG WINAPI on_fatal(PEXCEPTION_POINTERS info) {
    // 只记一次会漏：第一个异常可能是可恢复的（例如 xmem 的按需提交 AV），
    // 它把额度用掉，真正致命的那次就不打印了。改成前 N 次都记。
    static int count = 0;
    if (count++ >= 8) return EXCEPTION_CONTINUE_SEARCH;
    const DWORD code = info->ExceptionRecord->ExceptionCode;
    // 只报"致命类"：断言断点与访问冲突
    if (code != EXCEPTION_BREAKPOINT && code != EXCEPTION_ACCESS_VIOLATION &&
        code != EXCEPTION_ILLEGAL_INSTRUCTION && code != EXCEPTION_STACK_OVERFLOW) {
        return EXCEPTION_CONTINUE_SEARCH;
    }
    void* addr = info->ExceptionRecord->ExceptionAddress;
    HMODULE mod = nullptr;
    char name[256] = "<unknown>";
    unsigned long long rva = reinterpret_cast<unsigned long long>(addr);
    if (GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS |
                               GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                           reinterpret_cast<LPCWSTR>(addr), &mod) &&
        mod) {
        char full[MAX_PATH] = {0};
        GetModuleFileNameA(mod, full, MAX_PATH - 1);
        const char* base = full;
        for (const char* q = full; *q; ++q) {
            if (*q == '\\' || *q == '/') base = q + 1;
        }
        strncpy(name, base, sizeof(name) - 1);
        rva = reinterpret_cast<unsigned long long>(addr) - reinterpret_cast<unsigned long long>(mod);
    }
    logf("[xbgtasix] FATAL 异常 0x%08lX at %s+0x%llX", code, name, rva);

    // 定点采集：game+0x247AF2D 是"跨度 <= 2 MiB-1"不变量断言。
    // 断言式：([rsp+0x78] - [rsp+0x70]) + qword[game+0x75E6FF0] <= 0x1FFFFF
    // 这里把三个操作数、减法结果与最终和全部打出来，并显式判断无符号回绕。
    if (code == EXCEPTION_BREAKPOINT && rva == 0x247AF2D) {
        const CONTEXT* c = info->ContextRecord;
        auto read_q = [](unsigned long long addr, unsigned long long* out) -> bool {
            MEMORY_BASIC_INFORMATION mbi{};
            if (!addr) return false;
            if (VirtualQuery(reinterpret_cast<void*>(addr), &mbi, sizeof(mbi)) == 0) return false;
            if (mbi.State != MEM_COMMIT || (mbi.Protect & (PAGE_NOACCESS | PAGE_GUARD))) return false;
            std::memcpy(out, reinterpret_cast<const void*>(addr), sizeof(*out));
            return true;
        };
        unsigned long long begin = 0, end = 0, g = 0;
        const bool ok_b = read_q(c->Rsp + 0x70, &begin);
        const bool ok_e = read_q(c->Rsp + 0x78, &end);
        const bool ok_g = read_q(c->Rip - 0x247AF2D + 0x75E6FF0, &g);   // 同模块基址 + 全局 RVA
        const unsigned long long diff = end - begin;              // 无符号减法
        const bool wrapped = end < begin;
        const unsigned long long sum = diff + g;
        logf("[xbgtasix]   断言操作数: begin=[rsp+0x70]=0x%llX(%s) end=[rsp+0x78]=0x%llX(%s) "
             "g=0x%llX(%s)",
             begin, ok_b ? "可读" : "不可读", end, ok_e ? "可读" : "不可读", g,
             ok_g ? "可读" : "不可读");
        logf("[xbgtasix]   减法 end-begin = 0x%llX %s；加 g 后 = 0x%llX；阈值 0x1FFFFF -> %s",
             diff, wrapped ? "（发生无符号回绕！）" : "（无回绕）", sum,
             sum > 0x1FFFFFULL ? "超出（触发断言）" : "未超出");
    }
    // 现场寄存器 + 关键指针指向的内存：断言检查的是"某对象某字段非 0"，
    // 光有栈看不出那个对象是什么，把 rsi/rdi/rax 与 rsi 前 48 字节打出来。
    {
        const CONTEXT* c = info->ContextRecord;
        logf("[xbgtasix] 现场寄存器 rax=%p rbx=%p rcx=%p rdx=%p rsi=%p rdi=%p r8=%p r9=%p",
             reinterpret_cast<void*>(c->Rax), reinterpret_cast<void*>(c->Rbx),
             reinterpret_cast<void*>(c->Rcx), reinterpret_cast<void*>(c->Rdx),
             reinterpret_cast<void*>(c->Rsi), reinterpret_cast<void*>(c->Rdi),
             reinterpret_cast<void*>(c->R8), reinterpret_cast<void*>(c->R9));
        auto dump = [](const char* tag, unsigned long long addr) {
            if (!addr) return;
            MEMORY_BASIC_INFORMATION mbi{};
            if (VirtualQuery(reinterpret_cast<void*>(addr), &mbi, sizeof(mbi)) == 0) return;
            if (mbi.State != MEM_COMMIT || (mbi.Protect & (PAGE_NOACCESS | PAGE_GUARD))) return;
            const auto* b = reinterpret_cast<const unsigned char*>(addr);
            char hex[3 * 96 + 1] = {0};
            char asc[97] = {0};
            for (int i = 0; i < 96; ++i) {
                snprintf(hex + i * 3, 4, "%02X ", b[i]);
                asc[i] = (b[i] >= 32 && b[i] < 127) ? static_cast<char>(b[i]) : '.';
            }
            logf("[xbgtasix]   %s @%p: %s |%s|", tag, reinterpret_cast<void*>(addr), hex, asc);
        };
        // 断言对象所在区域：基址/偏移/状态能区分"新分配的块"与"静态结构"
        if (c->Rsi) {
            MEMORY_BASIC_INFORMATION mbi{};
            if (VirtualQuery(reinterpret_cast<void*>(c->Rsi), &mbi, sizeof(mbi)) != 0) {
                logf("[xbgtasix] 断言对象所在区域 allocBase=%p base=%p size=0x%llX state=0x%lX "
                     "protect=0x%lX | 对象偏移=0x%llX",
                     mbi.AllocationBase, mbi.BaseAddress,
                     static_cast<unsigned long long>(mbi.RegionSize),
                     static_cast<unsigned long>(mbi.State), static_cast<unsigned long>(mbi.Protect),
                     static_cast<unsigned long long>(c->Rsi -
                                                     reinterpret_cast<unsigned long long>(mbi.AllocationBase)));
            }
        }
        dump("rsi", c->Rsi);
        dump("rdi", c->Rdi);

        // 故障指令**自己的**栈帧：有些断言比较的是 [rsp+K] 里的两个值（例如
        // "这个跨度必须落在 2 MB 以内"），寄存器里根本看不到，只有它的栈窗口能回答。
        // 用 ContextRecord->Rsp（故障那一刻的 rsp），不是 VEH 现在的 rsp。
        // 逐 qword 做 VirtualQuery，读到未提交页就停，绝不赌"栈总是可读"。
        if (c->Rsp) {
            char line[768] = {0};
            int used = 0;
            for (int i = 0; i < 20; ++i) {
                const auto addr = c->Rsp + static_cast<unsigned long long>(i) * 8;
                MEMORY_BASIC_INFORMATION m{};
                if (VirtualQuery(reinterpret_cast<const void*>(addr), &m, sizeof(m)) == 0) break;
                if (m.State != MEM_COMMIT || (m.Protect & (PAGE_NOACCESS | PAGE_GUARD))) break;
                const int n = snprintf(line + used, sizeof(line) - static_cast<size_t>(used),
                                       " +0x%02X=%llx", i * 8,
                                       *reinterpret_cast<const unsigned long long*>(addr));
                if (n <= 0 || used + n >= static_cast<int>(sizeof(line))) break;
                used += n;
            }
            if (used > 0) logf("[xbgtasix] 故障栈帧%s", line);
        }
    }

    // 同时 dump 调用栈（模块+RVA）：断言点本身信息很少，栈才能说明"谁把它逼到这里"。
    // 尤其是崩在游戏内部时，栈上能看出最后一个返回去的是我们的哪个 DLL。
    {
        void* frames[24] = {0};
        const USHORT n = RtlCaptureStackBackTrace(1, 24, frames, nullptr);
        for (USHORT i = 0; i < n; ++i) {
            void* fa = frames[i];
            HMODULE fm = nullptr;
            char fname[256] = "<unknown>";
            unsigned long long frva = reinterpret_cast<unsigned long long>(fa);
            if (GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS |
                                       GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                                   reinterpret_cast<LPCWSTR>(fa), &fm) &&
                fm) {
                char full[MAX_PATH] = {0};
                GetModuleFileNameA(fm, full, MAX_PATH - 1);
                const char* b = full;
                for (const char* q = full; *q; ++q) {
                    if (*q == '\\' || *q == '/') b = q + 1;
                }
                strncpy(fname, b, sizeof(fname) - 1);
                frva = reinterpret_cast<unsigned long long>(fa) -
                       reinterpret_cast<unsigned long long>(fm);
            }
            logf("[xbgtasix]   #%-2u %s+0x%llX", i, fname, frva);
        }
    }
    if (code == EXCEPTION_BREAKPOINT) {
        logf("[xbgtasix] 这是断点指令：通常是游戏自己的断言失败");
    } else if (code == EXCEPTION_ACCESS_VIOLATION && info->ExceptionRecord->NumberParameters >= 2) {
        logf("[xbgtasix] 访问类型=%llu 地址=%p", info->ExceptionRecord->ExceptionInformation[0],
             reinterpret_cast<void*>(info->ExceptionRecord->ExceptionInformation[1]));
    }
    // ---------------------------------------------------------------------
    // 可恢复陷阱：game+0x247AF2D 的"跨度 <= 2 MiB-1"不变量在本构建里结构性恒真
    // （实测 begin=0、end=0x4000000、g=0，无回绕，两项都是游戏自己的立即数）。
    // 这里允许显式跳过它以便观察**下一处**阻塞，但必须：
    //   * 只在环境变量里点名才生效（默认绝不改变游戏行为）
    //   * 大声记录"掩盖了游戏自身的不变量"
    //   * 恢复 rsi（0x247AF35 要用它，现场的 rsi 已被冲成小整数）
    // ---------------------------------------------------------------------
    if (code == EXCEPTION_BREAKPOINT && rva == 0x247AF2D) {
        char env[256] = {0};
        const DWORD n = GetEnvironmentVariableA("XBGTASIX_SKIP_GAME_ASSERT", env, sizeof(env));
        if (n > 0 && std::strstr(env, "247AF2D")) {
            logf("[xbgtasix] ⚠ 跳过游戏断言 game+0x247AF2D（XBGTASIX_SKIP_GAME_ASSERT 已点名）");
            logf("[xbgtasix]   这**掩盖了游戏自身的不变量**：跨度 0x4000000 > 0x1FFFFF，"
                 "仅用于探测下一处阻塞");
            logf("[xbgtasix]   陷阱前 rsi=0x%llX rcx=0x%llX -> 用 rcx 恢复 rsi 并跳到 +0x247AF2E",
                 static_cast<unsigned long long>(info->ContextRecord->Rsi),
                 static_cast<unsigned long long>(info->ContextRecord->Rcx));
            // 可选实验：给游戏那块"瞬时块"播种（g_1475E6FF0 = game+0x75E6FF0）。
            // 目的只是测试"释放路径是否只缺一个非空基址"，不是修复：
            // 守卫式里 g 是加项，播种不能让它通过（已用算术证明），只能让跳过守卫后的
            // 释放路径有东西可用。默认关闭，必须显式点名。
            {
                char tb[32] = {0};
                if (GetEnvironmentVariableA("XBGTASIX_TRANSIENT_BLOCK", tb, sizeof(tb)) > 0 &&
                    tb[0] == '1') {
                    const unsigned long long base =
                        reinterpret_cast<unsigned long long>(addr) - rva;
                    auto* g = reinterpret_cast<unsigned long long*>(base + 0x75E6FF0);
                    MEMORY_BASIC_INFORMATION gm{};
                    if (VirtualQuery(g, &gm, sizeof(gm)) && gm.State == MEM_COMMIT &&
                        !(gm.Protect & (PAGE_NOACCESS | PAGE_GUARD))) {
                        if (*g == 0) {
                            void* blk = VirtualAlloc(nullptr, 2u << 20, MEM_COMMIT | MEM_RESERVE,
                                                     PAGE_READWRITE);
                            *g = reinterpret_cast<unsigned long long>(blk);
                            logf("[xbgtasix] 实验：已为瞬时块播种 g=%p（2 MiB，仅测试释放路径）",
                                 blk);
                        } else {
                            logf("[xbgtasix] 实验：g 已非空（0x%llX），不覆盖",
                                 static_cast<unsigned long long>(*g));
                        }
                    } else {
                        logf("[xbgtasix] 实验：g 所在页不可读，放弃播种");
                    }
                }
            }
            info->ContextRecord->Rsi = info->ContextRecord->Rcx;
            info->ContextRecord->Rip = reinterpret_cast<unsigned long long>(addr) - rva + 0x247AF2E;
            return EXCEPTION_CONTINUE_EXECUTION;
        }
        logf("[xbgtasix]   （想跨过去看下一处阻塞：设 XBGTASIX_SKIP_GAME_ASSERT=247AF2D）");
    }

    return EXCEPTION_CONTINUE_SEARCH;   // 继续原处理，不改变游戏行为
}

void maybe_install_veh() {
    static bool done = false;
    if (done) return;
    done = true;
    char buf[8] = {0};
    if (GetEnvironmentVariableA("XBGTASIX_VEH", buf, sizeof(buf)) == 0 || buf[0] != '1') return;
    AddVectoredExceptionHandler(1, on_fatal);
    logf("[xbgtasix] 已安装异常钩子（XBGTASIX_VEH=1）");
}

void unimpl(const char* api) {
    {
        std::lock_guard<std::mutex> lock(g_mu);
        if (!g_seen.insert(api).second) {
            // 只报第一次，避免日志被刷爆
            if (unimpl_mode() == UnimplMode::Log) return;
        }
    }
    logf("[xbgtasix] UNIMPLEMENTED: %s", api);
    if (unimpl_mode_cached() == UnimplMode::Stop) {
        // 退出前先留下"谁调的"：只有 API 名对定位没帮助，栈比名字有用得多。
        void* frames[24] = {0};
        const USHORT n = RtlCaptureStackBackTrace(0, 24, frames, nullptr);
        log_main_module_base();   // 先给基址，再把每帧换算成 模块+0xRVA
        for (USHORT i = 0; i < n; ++i) {
            log_frame(i, frames[i]);   // 逐帧 logf；解析失败走 <unknown> 兜底，不中断
        }
        logf("[xbgtasix] stop (set XBGTASIX_UNIMPL=log to continue)");
        // ExitProcess 会等所有 DLL 的 DllMain(detach)：若我们此刻正持有 loader lock，
        // 就会互等死锁。TerminateProcess 跳过这些回调，适合"诊断性退出"。
        if (GetModuleHandleA("ntdll.dll")) {
            using PFN = VOID(WINAPI*)(HANDLE, UINT);
            auto fn = reinterpret_cast<PFN>(
                GetProcAddress(GetModuleHandleA("kernel32.dll"), "TerminateProcess"));
            if (fn) fn(GetCurrentProcess(), 70);
        }
        ExitProcess(70);
    }
}

void set_game_root(const char* path) {
    maybe_install_veh();
    std::lock_guard<std::mutex> lock(g_mu);
    strncpy(g_root, path ? path : "", MAX_PATH - 1);
}

const char* game_root() { return g_root; }

}  // namespace xbgtasix
