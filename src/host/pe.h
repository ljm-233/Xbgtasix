// pe.h —— 极简 PE 解析：只做三件事（判断是不是 PE、读导入、读导出）
//
// 为什么要自己写：`check` 命令要在**没有 Python 的机器**上对表，
// 所以核心逻辑必须是 C++。逻辑与 tools/analyze_exe.py 一致，可以互相校验。
#pragma once

#include <cstdint>
#include <string>
#include <vector>

namespace pe {

struct Import {
    std::string module;              // 小写模块名，如 "xgameruntime.dll"
    std::vector<std::string> names;  // 按名字导入的符号；按序号导入记为 "#123"
    std::vector<uint16_t> ordinals;  // 与 names 一一对应，按名字导入时为 0
};

struct Info {
    bool ok = false;
    bool is_64 = false;
    uint16_t machine = 0;
    uint16_t subsystem = 0;
    std::string error;               // ok=false 时的原因
    std::vector<Import> imports;
    std::vector<std::string> exports;  // 按名字导出的符号
};

// 读磁盘文件并解析。失败时 Info::ok = false，error 里有原因。
Info parse_file(const std::wstring& path);

// 只要导出名（check 命令对表用，避免解析整个导入表）
std::vector<std::string> read_exports(const std::wstring& path);

}  // namespace pe
