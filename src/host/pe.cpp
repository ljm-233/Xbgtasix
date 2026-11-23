// pe.cpp —— PE 导入/导出表解析（只支持 PE32 / PE32+，够用即可）
#include "pe.h"

#include <windows.h>

#include <cstdio>
#include <cstring>

namespace pe {
namespace {

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
    bool is64 = false;

    bool in_bounds(size_t off, size_t len) const { return off <= size && len <= size - off; }

    // RVA -> 文件偏移
    const uint8_t* at(uint32_t rva) const {
        // 先查节表
        for (size_t i = 0; i < section_count; ++i) {
            const auto& s = sections[i];
            const uint32_t va = s.VirtualAddress;
            const uint32_t vsize = s.Misc.VirtualSize ? s.Misc.VirtualSize : s.SizeOfRawData;
            if (rva >= va && rva < va + vsize) {
                const uint32_t delta = rva - va;
                if (delta >= s.SizeOfRawData) return nullptr;   // 落在未初始化区
                const size_t off = static_cast<size_t>(s.PointerToRawData) + delta;
                if (!in_bounds(off, 1)) return nullptr;
                return base + off;
            }
        }
        // 头部区域（导入表很少在这，但保险）
        if (rva < size) return base + rva;
        return nullptr;
    }

    const char* str(uint32_t rva) const {
        const uint8_t* p = at(rva);
        if (!p) return nullptr;
        const size_t off = static_cast<size_t>(p - base);
        // 保证有 NUL 结尾
        const void* nul = std::memchr(p, 0, size - off);
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
    if (nt_off + sizeof(IMAGE_NT_HEADERS64) > buf.size()) {
        *err = "NT 头越界";
        return false;
    }
    if (*reinterpret_cast<const DWORD*>(buf.data() + nt_off) != IMAGE_NT_SIGNATURE) {
        *err = "没有 PE 签名";
        return false;
    }
    const WORD magic = *reinterpret_cast<const WORD*>(buf.data() + nt_off + 24);
    if (magic == IMAGE_NT_OPTIONAL_HDR64_MAGIC) {
        l->is64 = true;
        l->nt64 = reinterpret_cast<const IMAGE_NT_HEADERS64*>(buf.data() + nt_off);
        l->sections = IMAGE_FIRST_SECTION(l->nt64);
        l->section_count = l->nt64->FileHeader.NumberOfSections;
    } else if (magic == IMAGE_NT_OPTIONAL_HDR32_MAGIC) {
        l->is64 = false;
        l->nt32 = reinterpret_cast<const IMAGE_NT_HEADERS32*>(buf.data() + nt_off);
        l->sections = IMAGE_FIRST_SECTION(l->nt32);
        l->section_count = l->nt32->FileHeader.NumberOfSections;
    } else {
        *err = "未知的 PE 魔数";
        return false;
    }
    return true;
}

void parse_imports(const Layout& l, Info* info) {
    const IMAGE_DATA_DIRECTORY dir = l.is64 ? l.nt64->OptionalHeader.DataDirectory[IMAGE_DIRECTORY_ENTRY_IMPORT]
                                            : l.nt32->OptionalHeader.DataDirectory[IMAGE_DIRECTORY_ENTRY_IMPORT];
    if (dir.VirtualAddress == 0 || dir.Size == 0) return;

    for (uint32_t i = 0;; ++i) {
        const uint8_t* desc_p = l.at(dir.VirtualAddress + i * sizeof(IMAGE_IMPORT_DESCRIPTOR));
        if (!desc_p) break;
        const auto* desc = reinterpret_cast<const IMAGE_IMPORT_DESCRIPTOR*>(desc_p);
        if (desc->Name == 0 && desc->FirstThunk == 0) break;   // 结束标记

        Import imp;
        const char* mod = l.str(desc->Name);
        imp.module = mod ? lower(mod) : std::string();

        const uint32_t thunk_rva = desc->OriginalFirstThunk ? desc->OriginalFirstThunk : desc->FirstThunk;
        const size_t thunk_size = l.is64 ? 8 : 4;
        for (uint32_t k = 0;; ++k) {
            const uint8_t* tp = l.at(thunk_rva + k * static_cast<uint32_t>(thunk_size));
            if (!tp) break;
            uint64_t val = 0;
            if (l.is64) {
                val = *reinterpret_cast<const uint64_t*>(tp);
            } else {
                val = *reinterpret_cast<const uint32_t*>(tp);
            }
            if (val == 0) break;
            const uint64_t ordinal_flag = l.is64 ? 0x8000000000000000ULL : 0x80000000ULL;
            if (val & ordinal_flag) {
                imp.names.push_back("#" + std::to_string(val & 0xFFFF));
                imp.ordinals.push_back(static_cast<uint16_t>(val & 0xFFFF));
            } else {
                // IMAGE_IMPORT_BY_NAME: WORD Hint; char Name[]
                const char* name = l.str(static_cast<uint32_t>(val) + 2);
                if (name) {
                    imp.names.push_back(name);
                    imp.ordinals.push_back(0);
                }
            }
            if (k > 200000) break;   // 防御：坏文件
        }
        if (!imp.module.empty()) info->imports.push_back(std::move(imp));
        if (i > 4096) break;   // 防御
    }
}

void parse_exports(const Layout& l, Info* info) {
    const IMAGE_DATA_DIRECTORY dir = l.is64 ? l.nt64->OptionalHeader.DataDirectory[IMAGE_DIRECTORY_ENTRY_EXPORT]
                                            : l.nt32->OptionalHeader.DataDirectory[IMAGE_DIRECTORY_ENTRY_EXPORT];
    if (dir.VirtualAddress == 0 || dir.Size == 0) return;
    const uint8_t* exp_p = l.at(dir.VirtualAddress);
    if (!exp_p) return;
    const auto* exp = reinterpret_cast<const IMAGE_EXPORT_DIRECTORY*>(exp_p);
    const uint8_t* names_p = l.at(exp->AddressOfNames);
    const uint8_t* ords_p = l.at(exp->AddressOfNameOrdinals);
    if (!names_p || !ords_p) return;
    for (DWORD i = 0; i < exp->NumberOfNames; ++i) {
        const uint32_t name_rva = reinterpret_cast<const uint32_t*>(names_p)[i];
        const char* name = l.str(name_rva);
        if (name) info->exports.push_back(name);
        if (i > 100000) break;
    }
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
    parse_imports(l, &info);
    parse_exports(l, &info);
    return info;
}

std::vector<std::string> read_exports(const std::wstring& path) {
    return parse_file(path).exports;
}

}  // namespace pe
