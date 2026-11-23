// probes.cpp —— t39 用的两个探针程序
//
//   probe_ok.exe      正常退出（0）        -> 分类应为 PASS
//   probe_unimpl.exe  调一个未实现的 API    -> stop 模式下分类应为 UNIMPL
//
// 刻意做成极简：它们只用来验证"启动器能不能正确分类"，不测别的。
#include <windows.h>

extern "C" __declspec(dllimport) HRESULT WINAPI XLaunchRestartOnCrash(void* async);

#ifdef PROBE_UNIMPL
int main() {
    // 这个导出目前仍是生成桩；XBGTASIX_UNIMPL=stop 时它会打印并退出 70
    XLaunchRestartOnCrash(nullptr);
    return 0;
}
#else
int main() { return 0; }
#endif
