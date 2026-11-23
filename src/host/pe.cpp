// SPDX-License-Identifier: GPL-3.0-or-later
// pe.cpp —— PE 导入/导出表解析（只支持 PE32 / PE32+，够用即可）
//
// 健壮性约定（针对畸形/截断输入的加固，逻辑仍是"极简解析"，不是重写）：
//   * 一切结构体/数组解引用前先过 at() / at_span()，越界一律返回 nullptr；
//   * 结构体里声明的数量（NumberOfSections/Names/Functions）与目录 Size、文件剩余字节取小；
//   * 循环上限检查放在进入循环体之前，另有全局 at() 预算，坏文件不会长时间停顿；
//   * 结构性校验（节表越界、SizeOfOptionalHeader 不对等）直接 ok=false，
//     避免"截断文件 → 0 个导入 → check PASS"这种假通过。
#include "pe.h"

#include <windows.h>

#include <algorithm>
#include <cctype>
#include <cstdio>
#include <cstring>

namespace pe {
namespace {

// PE 规范里这些结构体的大小是固定的；这里钉死，下面用来校验文件里的声明值。
static_assert(sizeof(IMAGE_OPTIONAL_HEADER64) == 240, "PE32+ 可选头应为 240 字节");
static_assert(sizeof(IMAGE_OPTIONAL_HEADER32) == 224, "PE32 可选头应为 224 字节");
static_assert(sizeof(IMAGE_SECTION_HEADER) == 40, "节表项应为 40 字节");

// 全局 at() 调用预算：正常 exe/dll 解析只需几百到几千次，
// 一个真文件不可能用掉 40 万次；用满说明 RVA 在乱指，按可疑处理。
constexpr size_t kAtBudget = 400000;
// PE/COFF 规范与加载器的节数上限是 96，超过就不是有效镜像。
constexpr size_t kMaxSections = 96;
// 导入描述符、单模块 thunk、导出名的硬上限（防止数量字段是 0xFFFFFFFF）。
constexpr size_t kMaxImportDescs = 4096;
constexpr size_t kMaxThunksPerModule = 65536;
constexpr size_t kMinThunkEntries = 256;
constexpr size_t kMaxExportNames = 100000;
// 导入/导出符号名长度上限：没有这个上限时，全文件都是非 0 字节的畸形样本
// 会让每次 str() 的 memchr 一路扫到文件尾（400000 次预算 × 单文件大小 = 分钟级以上停顿）。
constexpr size_t kMaxNameLen = 4096;

std::string lower(std::string s) {
    for (auto& c : s) c = static_cast<char>(::tolower(static_cast<unsigned char>(c)));
    return s;
}

bool read_whole(const std::wstring& path, std::vector<uint8_t>* out, std::string* err) {
    HANDLE h = CreateFileW(path.c_str(), GENERIC_READ, FILE_SHARE_READ, nullptr, OPEN_EXISTING, 0,
                           nullptr);
    if (h == INVALID_HANDLE_VALUE) {
        *err = "打不开文件 (err=" + std::to_string(GetLastError()) + ")";
        return false;
    }
    LARGE_INTEGER size{};
    if (!GetFileSizeEx(h, &size) || size.QuadPart <= 0) {
        CloseHandle(h);
        *err = "文件为空或读不到大小";
        return false;
    }
    if (size.QuadPart > 512LL * 1024 * 1024) {
        CloseHandle(h);
        *err = "文件过大（>512MB），拒绝解析";
        return false;
    }
    out->resize(static_cast<size_t>(size.QuadPart));
    DWORD got = 0;
    const bool ok = ReadFile(h, out->data(), static_cast<DWORD>(out->size()), &got, nullptr) != 0;
    CloseHandle(h);
    if (!ok || got != out->size()) {
        *err = "读取不完整";
        return false;
    }
    return true;
}

struct Layout {
    const uint8_t* base = nullptr;
    size_t size = 0;
    const IMAGE_NT_HEADERS32* nt32 = nullptr;
    const IMAGE_NT_HEADERS64* nt64 = nullptr;
    const IMAGE_SECTION_HEADER* sections = nullptr;
    size_t section_count = 0;
    size_t size_of_headers = 0;      // 头部区域字节数；at() 的"头部回落"只允许落在这里
    std::vector<uint8_t> raw_ok;     // 每个节的原始数据是否整段在文件内（1 = 可映射）
    bool is64 = false;

    mutable size_t at_calls = 0;         // 已发生的 at()/at_span() 次数
    mutable bool budget_exhausted = false;

    // 统一用 64 位算偏移，避免 32 位目标上 size_t 溢出
    bool in_bounds(uint64_t off, uint64_t len) const {
        return off <= static_cast<uint64_t>(size) && len <= static_cast<uint64_t>(size) - off;
    }

    // 真正的 RVA -> 文件指针（不计预算，由 at_span 统一记账）
    const uint8_t* locate(uint32_t rva) const {
        for (size_t i = 0; i < section_count; ++i) {
            if (!raw_ok[i]) continue;   // 该节原始数据越界 → 整节不可映射
            const auto& s = sections[i];
            // 32 位会回绕（va=0xFFFFF000、vsize=0x2000 时 va+vsize 变成 0x1000），一律用 64 位算
            const uint64_t va = s.VirtualAddress;
            const uint64_t vsize = s.Misc.VirtualSize ? s.Misc.VirtualSize : s.SizeOfRawData;
            if (static_cast<uint64_t>(rva) < va) continue;
            if (static_cast<uint64_t>(rva) >= va + vsize) continue;
            const uint64_t delta = static_cast<uint64_t>(rva) - va;
            if (delta >= s.SizeOfRawData) return nullptr;   // 落在未初始化区
            const uint64_t off = static_cast<uint64_t>(s.PointerToRawData) + delta;
            if (off >= size) return nullptr;                // raw_ok 已保证，双保险
            return base + static_cast<size_t>(off);
        }
        // 头部回落：旧代码写的是 rva < size，等于把任意 RVA 当文件偏移，会掩盖畸形。
        // 现在只认 SizeOfHeaders 以内的头部区域（该值在 load_layout 里已校验不超文件）。
        if (static_cast<uint64_t>(rva) < size_of_headers) return base + rva;
        return nullptr;
    }

    // 取 rva 处 len 字节；越界或预算耗尽返回 nullptr。
    // 所有结构体/数组解引用都必须走这里，不能只用 at() 拿到首字节就按 sizeof 解引用。
    const uint8_t* at_span(uint32_t rva, size_t len) const {
        if (len == 0) return nullptr;
        if (at_calls >= kAtBudget) {
            budget_exhausted = true;
            return nullptr;
        }
        ++at_calls;
        const uint8_t* p = locate(rva);
        if (!p) return nullptr;
        const size_t off = static_cast<size_t>(p - base);
        if (!in_bounds(off, len)) return nullptr;
        return p;
    }

    const uint8_t* at(uint32_t rva) const { return at_span(rva, 1); }

    const char* str(uint32_t rva) const {
        const uint8_t* p = at(rva);
        if (!p) return nullptr;
        const size_t off = static_cast<size_t>(p - base);
        // 保证有 NUL 结尾；扫描长度封顶 kMaxNameLen，正常符号名远小于它
        const size_t limit = std::min(size - off, kMaxNameLen);
        const void* nul = std::memchr(p, 0, limit);
        if (!nul) return nullptr;
        return reinterpret_cast<const char*>(p);
    }
};

bool load_layout(const std::vector<uint8_t>& buf, Layout* l, std::string* err) {
    l->base = buf.data();
    l->size = buf.size();
    if (buf.size() < sizeof(IMAGE_DOS_HEADER)) {
        *err = "太小，不像 PE";
        return false;
    }
    const auto* dos = reinterpret_cast<const IMAGE_DOS_HEADER*>(buf.data());
    if (dos->e_magic != IMAGE_DOS_SIGNATURE) {
        *err = "没有 MZ 头";
        return false;
    }
    const size_t nt_off = static_cast<size_t>(dos->e_lfanew);
    // 先只要求"签名 + 文件头 + 可选头魔数"可读，按类型算 NT 头大小后再精确校验，
    // 免得把 PE32（248 字节 NT 头）按 PE32+ 的 264 字节误判
    constexpr size_t kProbeLen = sizeof(DWORD) + sizeof(IMAGE_FILE_HEADER) + sizeof(WORD);
    if (nt_off < sizeof(IMAGE_DOS_HEADER) || !l->in_bounds(nt_off, kProbeLen)) {
        *err = "NT 头越界（文件被截断或 e_lfanew 畸形）";
        return false;
    }
    if (*reinterpret_cast<const DWORD*>(buf.data() + nt_off) != IMAGE_NT_SIGNATURE) {
        *err = "没有 PE 签名";
        return false;
    }

    const auto* fh = reinterpret_cast<const IMAGE_FILE_HEADER*>(buf.data() + nt_off + sizeof(DWORD));
    const WORD opt_size = fh->SizeOfOptionalHeader;
    const WORD magic =
        *reinterpret_cast<const WORD*>(buf.data() + nt_off + sizeof(DWORD) + sizeof(IMAGE_FILE_HEADER));

    size_t nt_size = 0;
    if (magic == IMAGE_NT_OPTIONAL_HDR64_MAGIC) {
        // 可选头大小必须正好 240，否则 IMAGE_FIRST_SECTION 会算错位置，后面全乱
        if (opt_size != sizeof(IMAGE_OPTIONAL_HEADER64)) {
            *err = "SizeOfOptionalHeader=" + std::to_string(opt_size) +
                   "，PE32+ 应为 240，文件可疑";
            return false;
        }
        l->is64 = true;
        l->nt64 = reinterpret_cast<const IMAGE_NT_HEADERS64*>(buf.data() + nt_off);
        nt_size = sizeof(IMAGE_NT_HEADERS64);
    } else if (magic == IMAGE_NT_OPTIONAL_HDR32_MAGIC) {
        if (opt_size != sizeof(IMAGE_OPTIONAL_HEADER32)) {
            *err = "SizeOfOptionalHeader=" + std::to_string(opt_size) +
                   "，PE32 应为 224，文件可疑";
            return false;
        }
        l->is64 = false;
        l->nt32 = reinterpret_cast<const IMAGE_NT_HEADERS32*>(buf.data() + nt_off);
        nt_size = sizeof(IMAGE_NT_HEADERS32);
    } else {
        *err = "未知的 PE 魔数";
        return false;
    }
    if (!l->in_bounds(nt_off, nt_size)) {
        *err = "NT 头越界（文件被截断？）";
        return false;
    }

    // ---- 节表：紧跟 NT 头，NumberOfSections 最大 65535，必须整段在文件内 ----
    const size_t sec_off = nt_off + nt_size;
    const size_t avail = (buf.size() > sec_off) ? (buf.size() - sec_off) : 0;
    const size_t declared = fh->NumberOfSections;
    if (declared == 0) {
        *err = "节数为 0，不是有效镜像";
        return false;
    }
    if (declared > kMaxSections) {
        *err = "节数 " + std::to_string(declared) + " 超过规范上限 96";
        return false;
    }
    const size_t fit = avail / sizeof(IMAGE_SECTION_HEADER);
    if (declared > fit) {
        *err = "节表越界：声明 " + std::to_string(declared) + " 个节，文件里只放得下 " +
               std::to_string(fit) + " 个（文件被截断？）";
        return false;
    }
    l->sections = reinterpret_cast<const IMAGE_SECTION_HEADER*>(buf.data() + sec_off);
    // 上面的判定已经保证放得下，min() 保留为双保险（将来放宽判定也不会越界读）
    l->section_count = std::min(declared, fit);

    // ---- SizeOfHeaders：既要是头部回落的上界，也不能超出文件 ----
    const uint32_t raw_hdr =
        l->is64 ? l->nt64->OptionalHeader.SizeOfHeaders : l->nt32->OptionalHeader.SizeOfHeaders;
    if (static_cast<size_t>(raw_hdr) > buf.size()) {
        *err = "SizeOfHeaders 超出文件大小（文件被截断？）";
        return false;
    }
    l->size_of_headers = std::max(static_cast<size_t>(raw_hdr), sec_off);   // 至少覆盖到节表末尾

    // ---- 逐节校验原始数据范围：PointerToRawData + SizeOfRawData 不得越过文件末尾 ----
    // 选择：只把越界的那一节标成"不可映射"，不整文件判无效。
    // 理由：部分加壳/裁切工具会留下 SizeOfRawData 大于实际剩余字节的节，整文件判死会误杀
    // 本来还能读出导入导出的样本；而"不可映射"已经保证 at() 不会越界读。
    // 补偿措施：若所有节都越界（基本等于文件被截断），仍然整体判为无效（见下）。
    l->raw_ok.assign(l->section_count, 0);
    size_t mappable = 0;
    for (size_t i = 0; i < l->section_count; ++i) {
        const auto& s = l->sections[i];
        if (l->in_bounds(static_cast<uint64_t>(s.PointerToRawData),
                         static_cast<uint64_t>(s.SizeOfRawData))) {
            l->raw_ok[i] = 1;
            ++mappable;
        }
    }
    if (mappable == 0) {
        *err = "全部 " + std::to_string(l->section_count) + " 个节的原始数据都超出文件末尾";
        return false;
    }
    return true;
}

// 返回 true = "目录结构与文件不符"（越界），why 里写原因；调用方据此判 ok=false。
// 只在"表项明明应该可读却映射不到"时报可疑，不因为数量字段大就乱判（数量已取 min）。
bool parse_imports(const Layout& l, Info* info, std::string* why) {
    const uint32_t num_dirs = l.is64 ? l.nt64->OptionalHeader.NumberOfRvaAndSizes
                                     : l.nt32->OptionalHeader.NumberOfRvaAndSizes;
    // NumberOfRvaAndSizes 之外的目录项内容是垃圾，不能当真
    if (static_cast<uint32_t>(IMAGE_DIRECTORY_ENTRY_IMPORT) >= num_dirs) return false;
    const IMAGE_DATA_DIRECTORY dir = l.is64 ? l.nt64->OptionalHeader.DataDirectory[IMAGE_DIRECTORY_ENTRY_IMPORT]
                                            : l.nt32->OptionalHeader.DataDirectory[IMAGE_DIRECTORY_ENTRY_IMPORT];
    if (dir.VirtualAddress == 0 || dir.Size == 0) return false;

    // 描述符表上界：dir.Size 里最多放得下几个 20 字节描述符，再套硬上限
    size_t max_desc = dir.Size / sizeof(IMAGE_IMPORT_DESCRIPTOR);
    if (max_desc == 0) return false;   // 连一个描述符都装不下，目录本身畸形，按"没有导入"处理
    max_desc = std::min(max_desc, kMaxImportDescs);

    for (size_t i = 0; i < max_desc; ++i) {   // 上限在进入循环体之前判定
        const uint64_t desc_rva =
            static_cast<uint64_t>(dir.VirtualAddress) + i * sizeof(IMAGE_IMPORT_DESCRIPTOR);
        if (desc_rva > 0xFFFFFFFFull) {       // RVA 是 32 位，越界就是畸形
            *why = "导入描述符表 RVA 超出 32 位范围";
            return true;
        }
        // 旧代码只保证首字节在界内就按 20 字节解引用，这里要求整段在界内
        const auto* desc = reinterpret_cast<const IMAGE_IMPORT_DESCRIPTOR*>(
            l.at_span(static_cast<uint32_t>(desc_rva), sizeof(IMAGE_IMPORT_DESCRIPTOR)));
        if (!desc) {
            // 合法的表一定以全 0 描述符收尾；映射不到说明 RVA 已越界 = 结构性损坏
            *why = "导入描述符表在第 " + std::to_string(i) + " 项处越界（文件被截断？）";
            return true;
        }
        if (desc->Name == 0 && desc->FirstThunk == 0) return false;   // 结束标记

        Import imp;
        const char* mod = l.str(desc->Name);
        imp.module = mod ? lower(mod) : std::string();

        const uint32_t thunk_rva = desc->OriginalFirstThunk ? desc->OriginalFirstThunk : desc->FirstThunk;
        const size_t thunk_size = l.is64 ? 8 : 4;
        // thunk 表上界：旧代码靠"读到 0 或 at() 失败"终止，没有终止符的表会一直读。
        // 以 dir.Size 换算出表项数作上界（实测 mingw 产物把整个导入段写进 Size：
        // 2284/4996/5432 字节 ⇒ 285/624/679 项，远大于实际 14/60/33 项）；
        // 但也见过 Size 只覆盖描述符的极小值，照抄会误截断合法表，故给 256 项下限；
        // 65536 上限保证任何情况下都会停。预算之外还有 at() 全局预算兜底。
        uint64_t cap = dir.Size / thunk_size;
        if (cap < kMinThunkEntries) cap = kMinThunkEntries;
        if (cap > kMaxThunksPerModule) cap = kMaxThunksPerModule;
        for (uint64_t k = 0; k < cap; ++k) {   // 上限在进入循环体之前判定
            const uint64_t t_rva = static_cast<uint64_t>(thunk_rva) + k * thunk_size;
            if (t_rva > 0xFFFFFFFFull) break;
            const uint8_t* tp = l.at_span(static_cast<uint32_t>(t_rva), thunk_size);
            if (!tp) break;
            uint64_t val = 0;
            if (l.is64) {
                uint64_t v = 0;
                std::memcpy(&v, tp, sizeof(v));
                val = v;
            } else {
                uint32_t v = 0;
                std::memcpy(&v, tp, sizeof(v));
                val = v;
            }
            if (val == 0) break;
            const uint64_t ordinal_flag = l.is64 ? 0x8000000000000000ULL : 0x80000000ULL;
            if (val & ordinal_flag) {
                imp.names.push_back("#" + std::to_string(val & 0xFFFF));
                imp.ordinals.push_back(static_cast<uint16_t>(val & 0xFFFF));
            } else {
                // IMAGE_IMPORT_BY_NAME: WORD Hint; char Name[]
                if (val > 0xFFFFFFFFull - 2) break;   // +2 会溢出 32 位 RVA
                const char* name = l.str(static_cast<uint32_t>(val) + 2);
                if (name) {
                    imp.names.push_back(name);
                    imp.ordinals.push_back(0);
                }
            }
        }
        if (!imp.module.empty()) info->imports.push_back(std::move(imp));
    }
    // 走到 dir.Size 允许的上限：说明表里没有全 0 收尾项，但没越界，先不当成损坏
    return false;
}

bool parse_exports(const Layout& l, Info* info, std::string* why) {
    const uint32_t num_dirs = l.is64 ? l.nt64->OptionalHeader.NumberOfRvaAndSizes
                                     : l.nt32->OptionalHeader.NumberOfRvaAndSizes;
    // NumberOfRvaAndSizes 之外的目录项内容是垃圾，不能当真
    if (static_cast<uint32_t>(IMAGE_DIRECTORY_ENTRY_EXPORT) >= num_dirs) return false;
    const IMAGE_DATA_DIRECTORY dir = l.is64 ? l.nt64->OptionalHeader.DataDirectory[IMAGE_DIRECTORY_ENTRY_EXPORT]
                                            : l.nt32->OptionalHeader.DataDirectory[IMAGE_DIRECTORY_ENTRY_EXPORT];
    if (dir.VirtualAddress == 0 || dir.Size == 0) return false;
    // 导出目录结构体本身必须完整在文件内（旧代码只保证首字节在界内就按 40 字节解引用）
    const auto* exp = reinterpret_cast<const IMAGE_EXPORT_DIRECTORY*>(
        l.at_span(dir.VirtualAddress, sizeof(IMAGE_EXPORT_DIRECTORY)));
    if (!exp) {
        *why = "导出目录结构体越界（文件被截断？）";
        return true;
    }

    // 声明值 vs 目录 Size：名字/序号/函数表都是 4 或 2 字节一项，dir.Size 除了就是天然上界；
    // NumberOfNames 可到 0xFFFFFFFF，必须和 dir.Size、硬上限三者取小。
    const uint64_t by_dir = dir.Size / sizeof(uint32_t);
    uint64_t n_names = std::min<uint64_t>(exp->NumberOfNames, by_dir);
    n_names = std::min<uint64_t>(n_names, kMaxExportNames);
    const uint64_t n_funcs = std::min<uint64_t>(exp->NumberOfFunctions, by_dir);
    if (n_names == 0) return false;

    // AddressOfNames + 4*N 必须整段在文件内（旧代码只校验了首字节）
    const uint8_t* names_p =
        l.at_span(exp->AddressOfNames, static_cast<size_t>(n_names * sizeof(uint32_t)));
    if (!names_p) return false;   // 数量已按 min 收窄，映射不到就当没有导出，不整文件判死

    // AddressOfNameOrdinals 旧代码取了完全没用（死代码）。这里既校验 2*N 的越界，
    // 也真的用起来：序号 >= NumberOfFunctions 的条目自相矛盾，跳过。
    const uint8_t* ords_p =
        n_funcs ? l.at_span(exp->AddressOfNameOrdinals, static_cast<size_t>(n_names * 2)) : nullptr;

    for (uint64_t i = 0; i < n_names; ++i) {   // 上限在进入循环体之前判定
        if (ords_p) {
            uint16_t ord = 0;
            std::memcpy(&ord, ords_p + i * 2, sizeof(ord));
            if (static_cast<uint64_t>(ord) >= n_funcs) continue;
        }
        uint32_t name_rva = 0;
        std::memcpy(&name_rva, names_p + i * sizeof(uint32_t), sizeof(name_rva));
        const char* name = l.str(name_rva);
        if (name) info->exports.push_back(name);
    }
    return false;
}

// read_exports() 的实现：只解析导出表，不碰导入表。
// 旧实现直接调 parse_file()，名不符实，还会把损坏 DLL 的导入表一起拖进来解析。
std::vector<std::string> exports_only(const std::wstring& path) {
    std::vector<std::string> out;
    std::vector<uint8_t> buf;
    std::string err;
    if (!read_whole(path, &buf, &err)) return out;
    Layout l;
    if (!load_layout(buf, &l, &err)) return out;
    Info info;
    std::string why;   // 导出表可疑时这里只有原因，调用方只要能用的导出名
    parse_exports(l, &info, &why);
    return std::move(info.exports);
}

}  // namespace

Info parse_file(const std::wstring& path) {
    Info info;
    std::vector<uint8_t> buf;
    std::string err;
    if (!read_whole(path, &buf, &err)) {
        info.error = err;
        return info;
    }
    Layout l;
    if (!load_layout(buf, &l, &err)) {
        info.error = err;
        return info;
    }
    info.ok = true;
    info.is_64 = l.is64;
    info.machine = l.is64 ? l.nt64->FileHeader.Machine : l.nt32->FileHeader.Machine;
    info.subsystem = static_cast<uint16_t>(
        l.is64 ? l.nt64->OptionalHeader.Subsystem : l.nt32->OptionalHeader.Subsystem);
    std::string why_imp;
    std::string why_exp;
    const bool suspect_imp = parse_imports(l, &info, &why_imp);
    const bool suspect_exp = parse_exports(l, &info, &why_exp);
    // at() 预算被打穿：RVA 几乎肯定在乱指，不能让 check 拿着半份结果出 PASS
    if (l.budget_exhausted) {
        info.ok = false;
        info.error = "RVA 解引用超过 " + std::to_string(kAtBudget) + " 次预算，文件疑似畸形，已停止解析";
        info.imports.clear();
        info.exports.clear();
    } else if (suspect_imp || suspect_exp) {
        // 目录结构指向文件外：给出确切原因，别让 main 把"0 个导入"当成 PASS
        info.ok = false;
        info.error = suspect_imp ? why_imp : why_exp;
        info.imports.clear();
        info.exports.clear();
    }
    return info;
}

std::vector<std::string> read_exports(const std::wstring& path) {
    return exports_only(path);
}

}  // namespace pe
