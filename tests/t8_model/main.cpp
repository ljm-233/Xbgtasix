// SPDX-License-Identifier: GPL-3.0-or-later
// t8_model — 用我们自己的 D3D12 管线渲染从游戏包里导出的模型
//
// 输入：OBJ（v / vt / f）+ PNG（可选，WIC 解码）
// 输出：renders/model.png（WIC 编码），同时做中心像素断言
//
// 目的：把"能读资产"变成"能看见资产"。不依赖任何游戏代码，只吃导出文件。
#include <windows.h>
#include <wincodec.h>
#include <d3d12.h>

#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <unordered_map>
#include <vector>

#include "xbgtasix/trace.h"

extern "C" {
HRESULT WINAPI XGameRuntimeInitialize(void);
HRESULT WINAPI D3D12CreateDevice(IUnknown* adapter, D3D_FEATURE_LEVEL min_level, REFIID riid,
                                 void** device);
HRESULT WINAPI D3D12SerializeVersionedRootSignature(const D3D12_VERSIONED_ROOT_SIGNATURE_DESC* desc,
                                                     ID3DBlob** blob, ID3DBlob** error);
HRESULT WINAPI D3D12GetDebugInterface(REFIID riid, void** out);
}

namespace {

constexpr UINT kDim = 512;
constexpr UINT kRowPitch = kDim * 4;

int g_fail = 0;

void check(HRESULT hr, const char* what) {
    if (FAILED(hr)) {
        xbgtasix::logf("[t8] FAIL %s hr=0x%08lx", what, static_cast<unsigned long>(hr));
        ++g_fail;
    }
}

// ---------------- 4x4 矩阵（行向量约定，HLSL 里用 mul(pos, M)）----------------

struct Mat4 {
    float m[4][4];
    static Mat4 identity() {
        Mat4 r{};
        for (int i = 0; i < 4; ++i) r.m[i][i] = 1.0f;
        return r;
    }
};

Mat4 mul(const Mat4& a, const Mat4& b) {
    Mat4 r{};
    for (int i = 0; i < 4; ++i)
        for (int j = 0; j < 4; ++j) {
            float s = 0;
            for (int k = 0; k < 4; ++k) s += a.m[i][k] * b.m[k][j];
            r.m[i][j] = s;
        }
    return r;
}

Mat4 rotation_y(float rad) {
    Mat4 r = Mat4::identity();
    r.m[0][0] = std::cos(rad);
    r.m[0][2] = -std::sin(rad);
    r.m[2][0] = std::sin(rad);
    r.m[2][2] = std::cos(rad);
    return r;
}

Mat4 rotation_x(float rad) {
    Mat4 r = Mat4::identity();
    r.m[1][1] = std::cos(rad);
    r.m[1][2] = -std::sin(rad);
    r.m[2][1] = std::sin(rad);
    r.m[2][2] = std::cos(rad);
    return r;
}

Mat4 look_at(const float eye[3], const float at[3], const float up[3]) {
    float f[3] = {at[0] - eye[0], at[1] - eye[1], at[2] - eye[2]};
    float fl = std::sqrt(f[0] * f[0] + f[1] * f[1] + f[2] * f[2]);
    for (float& v : f) v /= (fl > 1e-6f ? fl : 1.0f);
    float s[3] = {f[1] * up[2] - f[2] * up[1], f[2] * up[0] - f[0] * up[2], f[0] * up[1] - f[1] * up[0]};
    float sl = std::sqrt(s[0] * s[0] + s[1] * s[1] + s[2] * s[2]);
    for (float& v : s) v /= (sl > 1e-6f ? sl : 1.0f);
    float u[3] = {s[1] * f[2] - s[2] * f[1], s[2] * f[0] - s[0] * f[2], s[0] * f[1] - s[1] * f[0]};

    // 右手系视图矩阵（行向量约定）：第三行是 -f，相机看向 -Z。
    // 写成 +f 会让可见点落在相机背后（w<0 被裁掉），这是最容易写错的一行。
    Mat4 r = Mat4::identity();
    r.m[0][0] = s[0];  r.m[0][1] = u[0];  r.m[0][2] = -f[0];
    r.m[1][0] = s[1];  r.m[1][1] = u[1];  r.m[1][2] = -f[1];
    r.m[2][0] = s[2];  r.m[2][1] = u[2];  r.m[2][2] = -f[2];
    r.m[3][0] = -(s[0] * eye[0] + s[1] * eye[1] + s[2] * eye[2]);
    r.m[3][1] = -(u[0] * eye[0] + u[1] * eye[1] + u[2] * eye[2]);
    r.m[3][2] =  (f[0] * eye[0] + f[1] * eye[1] + f[2] * eye[2]);
    return r;
}

Mat4 perspective(float fov_y, float aspect, float zn, float zf) {
    const float h = 1.0f / std::tan(fov_y * 0.5f);
    const float w = h / aspect;
    Mat4 r{};
    r.m[0][0] = w;
    r.m[1][1] = h;
    r.m[2][2] = zf / (zn - zf);      // 右手系：相机看向 -Z
    r.m[2][3] = -1.0f;
    r.m[3][2] = zf * zn / (zn - zf);
    return r;
}

// ---------------- OBJ 载入 ----------------

struct Mesh {
    std::vector<float> verts;   // pos(3) + uv(2) + nrm(3)
    std::vector<unsigned> idx;
    float center[3] = {0, 0, 0};
    float radius = 1.0f;
    UINT triangles() const { return static_cast<UINT>(idx.size() / 3); }
};

bool load_obj(const char* path, Mesh& out) {
    FILE* f = nullptr;
    if (fopen_s(&f, path, "rb") != 0 || !f) {
        xbgtasix::logf("[t8] 打不开 OBJ: %s", path);
        return false;
    }
    std::vector<float> pos;      // 3 per vertex
    std::vector<float> uv;       // 2 per vertex
    std::unordered_map<unsigned long long, unsigned> map;
    char line[512];
    while (fgets(line, sizeof(line), f)) {
        if (line[0] == 'v' && line[1] == ' ') {
            float x, y, z;
            if (sscanf_s(line + 2, "%f %f %f", &x, &y, &z) >= 3) {
                pos.push_back(x); pos.push_back(y); pos.push_back(z);
            }
        } else if (line[0] == 'v' && line[1] == 't') {
            float u, v;
            if (sscanf_s(line + 3, "%f %f", &u, &v) >= 2) {
                uv.push_back(u); uv.push_back(v);
            }
        } else if (line[0] == 'f' && line[1] == ' ') {
            unsigned v[3] = {0, 0, 0}, t[3] = {0, 0, 0};
            int n = 0;
            const char* p = line + 2;
            while (*p && n < 3) {
                while (*p == ' ') ++p;
                if (!*p || *p == '\n' || *p == '\r') break;
                int vi = 0, ti = 0;
                const char* q = p;
                while (*q && *q != ' ' && *q != '/') { vi = vi * 10 + (*q - '0'); ++q; }
                if (*q == '/') {
                    ++q;
                    if (*q != '/') { while (*q && *q != ' ' && *q != '/') { ti = ti * 10 + (*q - '0'); ++q; } }
                }
                while (*q && *q != ' ') ++q;
                v[n] = vi > 0 ? static_cast<unsigned>(vi - 1) : 0u;
                t[n] = ti > 0 ? static_cast<unsigned>(ti - 1) : 0u;
                ++n;
                p = q;
            }
            for (int i = 1; i + 1 < n; ++i) {
                const int order[3] = {0, i, i + 1};
                for (int k = 0; k < 3; ++k) {
                    const int s = order[k];
                    unsigned long long key = (static_cast<unsigned long long>(v[s]) << 32) | t[s];
                    auto it = map.find(key);
                    unsigned index;
                    if (it == map.end()) {
                        index = static_cast<unsigned>(out.verts.size() / 8);
                        float px = v[s] * 3 + 0 < pos.size() ? pos[v[s] * 3 + 0] : 0.0f;
                        float py = v[s] * 3 + 1 < pos.size() ? pos[v[s] * 3 + 1] : 0.0f;
                        float pz = v[s] * 3 + 2 < pos.size() ? pos[v[s] * 3 + 2] : 0.0f;
                        float u = t[s] * 2 + 1 < uv.size() ? uv[t[s] * 2 + 0] : 0.0f;
                        float w = t[s] * 2 + 1 < uv.size() ? uv[t[s] * 2 + 1] : 0.0f;
                        out.verts.push_back(px); out.verts.push_back(py); out.verts.push_back(pz);
                        out.verts.push_back(u);  out.verts.push_back(w);
                        out.verts.push_back(0.0f); out.verts.push_back(0.0f); out.verts.push_back(0.0f);
                        map[key] = index;
                    } else {
                        index = it->second;
                    }
                    out.idx.push_back(index);
                }
            }
        }
    }
    fclose(f);

    char shade_mode[16] = {0};
    GetEnvironmentVariableA("XBGTASIX_T8_SHADING", shade_mode, sizeof(shade_mode));
    const bool flat = std::strcmp(shade_mode, "smooth") != 0;   // 默认平直

    if (flat) {
        // 平直着色：每个三角形用独立顶点 + 面法线，保留布料褶皱的硬边
        std::vector<float> nv;
        std::vector<unsigned> ni;
        nv.reserve(out.idx.size() * 8);
        for (size_t i = 0; i + 2 < out.idx.size(); i += 3) {
            const size_t a = out.idx[i] * 8, b = out.idx[i + 1] * 8, c = out.idx[i + 2] * 8;
            const float ux = out.verts[b] - out.verts[a], uy = out.verts[b + 1] - out.verts[a + 1],
                        uz = out.verts[b + 2] - out.verts[a + 2];
            const float vx = out.verts[c] - out.verts[a], vy = out.verts[c + 1] - out.verts[a + 1],
                        vz = out.verts[c + 2] - out.verts[a + 2];
            float nx = uy * vz - uz * vy, ny = uz * vx - ux * vz, nz = ux * vy - uy * vx;
            // RAGE 网格是顺时针绕序：叉积方向朝内，取反才是"朝外"的法线
            nx = -nx; ny = -ny; nz = -nz;
            const float len = std::sqrt(nx * nx + ny * ny + nz * nz);
            if (len > 1e-9f) { nx /= len; ny /= len; nz /= len; }
            for (size_t k : {a, b, c}) {
                nv.push_back(out.verts[k]);
                nv.push_back(out.verts[k + 1]);
                nv.push_back(out.verts[k + 2]);
                nv.push_back(out.verts[k + 3]);
                nv.push_back(out.verts[k + 4]);
                nv.push_back(nx);
                nv.push_back(ny);
                nv.push_back(nz);
                ni.push_back(static_cast<unsigned>(ni.size()));
            }
        }
        out.verts.swap(nv);
        out.idx.swap(ni);
    }

    // 逐面法线累加到顶点，再归一化
    if (!flat) {
        for (size_t i = 0; i + 2 < out.idx.size(); i += 3) {
            const size_t a = out.idx[i] * 8, b = out.idx[i + 1] * 8, c = out.idx[i + 2] * 8;
            const float ux = out.verts[b] - out.verts[a], uy = out.verts[b + 1] - out.verts[a + 1],
                        uz = out.verts[b + 2] - out.verts[a + 2];
            const float vx = out.verts[c] - out.verts[a], vy = out.verts[c + 1] - out.verts[a + 1],
                        vz = out.verts[c + 2] - out.verts[a + 2];
            const float nx = uy * vz - uz * vy, ny = uz * vx - ux * vz, nz = ux * vy - uy * vx;
            for (size_t k : {a, b, c}) {
                out.verts[k + 5] += nx;
                out.verts[k + 6] += ny;
                out.verts[k + 7] += nz;
            }
        }
    }
    for (size_t i = 0; i < out.verts.size(); i += 8) {
        const float len = std::sqrt(out.verts[i + 5] * out.verts[i + 5] +
                                    out.verts[i + 6] * out.verts[i + 6] +
                                    out.verts[i + 7] * out.verts[i + 7]);
        if (len > 1e-6f) {
            out.verts[i + 5] /= len;
            out.verts[i + 6] /= len;
            out.verts[i + 7] /= len;
        } else {
            out.verts[i + 5] = 0; out.verts[i + 6] = 1; out.verts[i + 7] = 0;
        }
    }

    if (out.verts.empty() || out.idx.empty()) {
        xbgtasix::logf("[t8] OBJ 里没有几何数据");
        return false;
    }

    // 归一化：算 bbox 中心与半径，让模型落在单位球内
    float mn[3] = {1e30f, 1e30f, 1e30f}, mx[3] = {-1e30f, -1e30f, -1e30f};
    for (size_t i = 0; i < out.verts.size(); i += 8) {
        for (int k = 0; k < 3; ++k) {
            mn[k] = out.verts[i + k] < mn[k] ? out.verts[i + k] : mn[k];
            mx[k] = out.verts[i + k] > mx[k] ? out.verts[i + k] : mx[k];
        }
    }
    for (int k = 0; k < 3; ++k) out.center[k] = (mn[k] + mx[k]) * 0.5f;
    float r = 0.0f;
    for (size_t i = 0; i < out.verts.size(); i += 8) {
        float d = 0.0f;
        for (int k = 0; k < 3; ++k) {
            const float t = out.verts[i + k] - out.center[k];
            d += t * t;
        }
        r = std::sqrt(d) > r ? std::sqrt(d) : r;
    }
    out.radius = r > 1e-6f ? r : 1.0f;
    xbgtasix::logf("[t8] OBJ: %u 顶点, %u 三角形, 半径 %.3f", static_cast<unsigned>(out.verts.size() / 8),
                   out.triangles(), out.radius);
    return true;
}

// ---------------- WIC ----------------

bool load_png_wic(const wchar_t* path, std::vector<unsigned char>& rgba, UINT& w, UINT& h) {
    HRESULT hr = CoInitializeEx(nullptr, COINIT_MULTITHREADED);
    if (FAILED(hr) && hr != RPC_E_CHANGED_MODE) return false;

    IWICImagingFactory* factory = nullptr;
    hr = CoCreateInstance(CLSID_WICImagingFactory, nullptr, CLSCTX_INPROC_SERVER,
                          IID_IWICImagingFactory, reinterpret_cast<void**>(&factory));
    if (FAILED(hr)) return false;

    IWICBitmapDecoder* dec = nullptr;
    hr = factory->CreateDecoderFromFilename(path, nullptr, GENERIC_READ, WICDecodeMetadataCacheOnLoad, &dec);
    if (SUCCEEDED(hr)) {
        IWICBitmapFrameDecode* frame = nullptr;
        hr = dec->GetFrame(0, &frame);
        if (SUCCEEDED(hr)) {
            IWICFormatConverter* conv = nullptr;
            hr = factory->CreateFormatConverter(&conv);
            if (SUCCEEDED(hr)) {
                hr = conv->Initialize(frame, GUID_WICPixelFormat32bppRGBA, WICBitmapDitherTypeNone, nullptr,
                                      0.0, WICBitmapPaletteTypeCustom);
                if (SUCCEEDED(hr)) {
                    frame->GetSize(&w, &h);
                    rgba.resize(size_t(w) * h * 4);
                    hr = conv->CopyPixels(nullptr, w * 4, static_cast<UINT>(rgba.size()), rgba.data());
                }
                conv->Release();
            }
            frame->Release();
        }
        dec->Release();
    }
    factory->Release();
    return SUCCEEDED(hr) && !rgba.empty();
}

bool save_png_wic(const wchar_t* path, const unsigned char* rgba, UINT w, UINT h) {
    IWICImagingFactory* factory = nullptr;
    if (FAILED(CoCreateInstance(CLSID_WICImagingFactory, nullptr, CLSCTX_INPROC_SERVER,
                                IID_IWICImagingFactory, reinterpret_cast<void**>(&factory))))
        return false;
    IWICStream* stream = nullptr;
    IWICBitmapEncoder* enc = nullptr;
    IWICBitmapFrameEncode* frame = nullptr;
    bool ok = false;
    if (SUCCEEDED(factory->CreateStream(&stream)) &&
        SUCCEEDED(stream->InitializeFromFilename(path, GENERIC_WRITE)) &&
        SUCCEEDED(factory->CreateEncoder(GUID_ContainerFormatPng, nullptr, &enc)) &&
        SUCCEEDED(enc->Initialize(stream, WICBitmapEncoderNoCache)) &&
        SUCCEEDED(enc->CreateNewFrame(&frame, nullptr)) &&
        SUCCEEDED(frame->Initialize(nullptr))) {
        WICPixelFormatGUID fmt = GUID_WICPixelFormat32bppRGBA;
        frame->SetSize(w, h);
        frame->SetPixelFormat(&fmt);
        ok = SUCCEEDED(frame->WritePixels(h, w * 4, static_cast<UINT>(w * h * 4),
                                          const_cast<BYTE*>(rgba)));
        if (ok) ok = SUCCEEDED(frame->Commit()) && SUCCEEDED(enc->Commit());
    }
    if (frame) frame->Release();
    if (enc) enc->Release();
    if (stream) stream->Release();
    factory->Release();
    return ok;
}

// ---------------- 资源 ----------------

ID3D12Resource* make_buffer(ID3D12Device* dev, UINT64 size, D3D12_HEAP_TYPE heap,
                            D3D12_RESOURCE_STATES st) {
    D3D12_HEAP_PROPERTIES hp{};
    hp.Type = heap;
    D3D12_RESOURCE_DESC bd{};
    bd.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER;
    bd.Width = size;
    bd.Height = 1;
    bd.DepthOrArraySize = 1;
    bd.MipLevels = 1;
    bd.Format = DXGI_FORMAT_UNKNOWN;
    bd.SampleDesc.Count = 1;
    bd.Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;
    ID3D12Resource* r = nullptr;
    check(dev->CreateCommittedResource(&hp, D3D12_HEAP_FLAG_NONE, &bd, st, nullptr,
                                       IID_ID3D12Resource, reinterpret_cast<void**>(&r)),
          "CreateCommittedResource(buffer)");
    return r;
}

UINT align256(UINT v) { return (v + 255u) & ~255u; }

}  // namespace

int main(int argc, char** argv) {
    const char* obj_path = argc > 1 ? argv[1] : "model.obj";
    const char* png_path = argc > 2 ? argv[2] : "";
    const char* out_png = argc > 3 ? argv[3] : "model.png";
    CoInitializeEx(nullptr, COINIT_MULTITHREADED);
    xbgtasix::logf("[t8] OBJ=%s PNG=%s OUT=%s", obj_path, png_path[0] ? png_path : "(none)", out_png);

    Mesh mesh;
    if (!load_obj(obj_path, mesh)) return 1;

    std::vector<unsigned char> tex;
    UINT tw = 0, th = 0;
    bool have_tex = false;
    if (png_path[0]) {
        wchar_t wpath[512];
        MultiByteToWideChar(CP_UTF8, 0, png_path, -1, wpath, 512);
        have_tex = load_png_wic(wpath, tex, tw, th);
        xbgtasix::logf("[t8] 贴图: %s (%ux%u)", have_tex ? "ok" : "失败", tw, th);
    }
    if (!have_tex) {   // 兜底：棋盘
        tw = th = 64;
        tex.resize(size_t(tw) * th * 4);
        for (UINT y = 0; y < th; ++y)
            for (UINT x = 0; x < tw; ++x) {
                const bool odd = ((x / 16) + (y / 16)) & 1;
                unsigned char* p = &tex[(y * tw + x) * 4];
                p[0] = odd ? 120 : 96; p[1] = 110; p[2] = odd ? 104 : 128; p[3] = 255;
            }
    }

    // 先开调试层：PSO 校验失败时能拿到具体原因
    {
        ID3D12Debug* dbg = nullptr;
        if (SUCCEEDED(D3D12GetDebugInterface(IID_ID3D12Debug, reinterpret_cast<void**>(&dbg))) && dbg) {
            dbg->EnableDebugLayer();
            dbg->Release();
            xbgtasix::logf("[t8] D3D12 调试层已启用");
        } else {
            xbgtasix::logf("[t8] 调试层不可用（继续跑）");
        }
    }

    check(XGameRuntimeInitialize(), "XGameRuntimeInitialize");
    ID3D12Device* dev = nullptr;
    check(D3D12CreateDevice(nullptr, D3D_FEATURE_LEVEL_11_0, IID_ID3D12Device,
                            reinterpret_cast<void**>(&dev)),
          "D3D12CreateDevice");
    if (!dev) return 1;

    // root signature: b0(CBV) 走 32-bit 常量太挤，用 CBV；t0(SRV) + 静态采样器
    // [0] b0 用 root CBV（直接给地址，少一层描述符表）
    // [1] t0 用描述符表
    D3D12_DESCRIPTOR_RANGE range{};
    range.RangeType = D3D12_DESCRIPTOR_RANGE_TYPE_SRV;
    range.NumDescriptors = 1;
    range.BaseShaderRegister = 0;
    range.OffsetInDescriptorsFromTableStart = 0;

    D3D12_ROOT_PARAMETER params[2]{};
    params[0].ParameterType = D3D12_ROOT_PARAMETER_TYPE_CBV;
    params[0].Descriptor.ShaderRegister = 0;
    params[0].Descriptor.RegisterSpace = 0;
    params[0].ShaderVisibility = D3D12_SHADER_VISIBILITY_ALL;
    params[1].ParameterType = D3D12_ROOT_PARAMETER_TYPE_DESCRIPTOR_TABLE;
    params[1].DescriptorTable.NumDescriptorRanges = 1;
    params[1].DescriptorTable.pDescriptorRanges = &range;
    params[1].ShaderVisibility = D3D12_SHADER_VISIBILITY_PIXEL;

    D3D12_STATIC_SAMPLER_DESC samp{};
    samp.Filter = D3D12_FILTER_MIN_MAG_MIP_LINEAR;
    samp.AddressU = samp.AddressV = samp.AddressW = D3D12_TEXTURE_ADDRESS_MODE_WRAP;
    samp.MaxAnisotropy = 1;
    samp.ComparisonFunc = D3D12_COMPARISON_FUNC_ALWAYS;
    samp.MaxLOD = 0.0f;
    samp.ShaderVisibility = D3D12_SHADER_VISIBILITY_PIXEL;

    D3D12_VERSIONED_ROOT_SIGNATURE_DESC rs{};
    rs.Version = D3D_ROOT_SIGNATURE_VERSION_1;
    rs.Desc_1_0.NumParameters = 2;
    rs.Desc_1_0.pParameters = params;
    rs.Desc_1_0.NumStaticSamplers = 1;
    rs.Desc_1_0.pStaticSamplers = &samp;
    // 没有这个标志，root signature 就声明"不使用输入装配器"，
    // 带 InputLayout 的图形 PSO 会被判 E_INVALIDARG
    rs.Desc_1_0.Flags = D3D12_ROOT_SIGNATURE_FLAG_ALLOW_INPUT_ASSEMBLER_INPUT_LAYOUT;

    ID3DBlob* sig = nullptr;
    ID3DBlob* err = nullptr;
    check(D3D12SerializeVersionedRootSignature(&rs, &sig, &err), "SerializeVersionedRootSignature");
    if (err) { xbgtasix::logf("[t8] rs err: %s", static_cast<const char*>(err->GetBufferPointer())); err->Release(); }
    ID3D12RootSignature* root = nullptr;
    if (!sig) return 1;
    check(dev->CreateRootSignature(0, sig->GetBufferPointer(), sig->GetBufferSize(),
                                   IID_ID3D12RootSignature, reinterpret_cast<void**>(&root)),
          "CreateRootSignature");
    sig->Release();
    if (!root) return 1;

    // 着色器从文件读（build.sh 用 dxc 编）
    auto load_file = [](const char* p, std::vector<unsigned char>& out) {
        FILE* f = nullptr;
        if (fopen_s(&f, p, "rb") != 0 || !f) return false;
        fseek(f, 0, SEEK_END);
        long n = ftell(f);
        fseek(f, 0, SEEK_SET);
        out.resize(static_cast<size_t>(n));
        const bool ok = fread(out.data(), 1, out.size(), f) == out.size();
        fclose(f);
        return ok;
    };
    std::vector<unsigned char> vs, ps;
    if (!load_file("model.vs.cso", vs) || !load_file("model.ps.cso", ps)) {
        xbgtasix::logf("[t8] 缺 model.vs.cso / model.ps.cso");
        return 1;
    }
    char t8mode[8] = {0};
    GetEnvironmentVariableA("XBGTASIX_T8_MODE", t8mode, sizeof(t8mode));
    const int mode = t8mode[0] ? t8mode[0] - '0' : 0;
    if (mode == 7) {   // 诊断：硬编码矩阵，绕开 CBV
        std::vector<unsigned char> hvs, hps;
        if (load_file("model_hm.vs.cso", hvs) && load_file("model_hm.ps.cso", hps)) {
            vs = hvs;
            ps = hps;
            xbgtasix::logf("[t8] 诊断: 硬编码矩阵 shader");
        }
    }
    if (mode == 6) {   // 诊断：绕开 MVP，直接看顶点数据是否到达 GPU
        std::vector<unsigned char> dvs, dps;
        if (load_file("model_dbg.vs.cso", dvs) && load_file("model_dbg.ps.cso", dps)) {
            vs = dvs;
            ps = dps;
            xbgtasix::logf("[t8] 诊断: 顶点直出 shader");
        }
    }
    if (mode == 4) {   // 诊断：用 t1 的 SV_VertexID 三角形，绕开 VB/IA
        std::vector<unsigned char> tvs, tps;
        if (!load_file("triangle.vs.cso", tvs) || !load_file("triangle.ps.cso", tps)) {
            xbgtasix::logf("[t8] 缺 triangle.*.cso");
            return 1;
        }
        vs = tvs;
        ps = tps;
        xbgtasix::logf("[t8] 诊断: 用 t1 三角形 shader");
    }

    xbgtasix::logf("[t8] VS=%zu B (%.4s)  PS=%zu B (%.4s)", vs.size(),
                   reinterpret_cast<const char*>(vs.data()), ps.size(),
                   reinterpret_cast<const char*>(ps.data()));
    D3D12_INPUT_ELEMENT_DESC layout[3]{};
    layout[0] = {"POSITION", 0, DXGI_FORMAT_R32G32B32_FLOAT, 0, 0, D3D12_INPUT_CLASSIFICATION_PER_VERTEX_DATA, 0};
    layout[1] = {"TEXCOORD", 0, DXGI_FORMAT_R32G32_FLOAT, 0, 12, D3D12_INPUT_CLASSIFICATION_PER_VERTEX_DATA, 0};
    layout[2] = {"NORMAL", 0, DXGI_FORMAT_R32G32B32_FLOAT, 0, 20, D3D12_INPUT_CLASSIFICATION_PER_VERTEX_DATA, 0};

    D3D12_GRAPHICS_PIPELINE_STATE_DESC pd{};
    pd.pRootSignature = root;
    pd.VS = {vs.data(), vs.size()};
    pd.PS = {ps.data(), ps.size()};
    pd.InputLayout = {layout, mode == 4 ? 0 : 3};
    pd.RasterizerState.FillMode = D3D12_FILL_MODE_SOLID;
    {
        char cull[8] = {0};
        const bool cull_on = GetEnvironmentVariableA("XBGTASIX_T8_CULL", cull, sizeof(cull)) == 0 ||
                             cull[0] != '0';
        pd.RasterizerState.CullMode = cull_on ? D3D12_CULL_MODE_BACK : D3D12_CULL_MODE_NONE;
        // RAGE 的网格是顺时针绕序：开剔除时要把"正面"判据也翻过来，
        // 否则剔掉的是外表面、看到的是内壁（表现为整体变暗）
        pd.RasterizerState.FrontCounterClockwise = cull_on ? TRUE : FALSE;
    }
    pd.RasterizerState.FrontCounterClockwise = FALSE;
    pd.RasterizerState.DepthClipEnable = TRUE;
    pd.RasterizerState.MultisampleEnable = FALSE;
    pd.RasterizerState.AntialiasedLineEnable = FALSE;
    pd.DepthStencilState.DepthEnable = TRUE;
    pd.DepthStencilState.DepthWriteMask = D3D12_DEPTH_WRITE_MASK_ALL;
    pd.DepthStencilState.DepthFunc = D3D12_COMPARISON_FUNC_LESS;
    pd.DepthStencilState.StencilEnable = FALSE;
    pd.DepthStencilState.StencilReadMask = 0xFF;
    pd.DepthStencilState.StencilWriteMask = 0xFF;
    pd.BlendState.RenderTarget[0].RenderTargetWriteMask = D3D12_COLOR_WRITE_ENABLE_ALL;
    pd.SampleMask = UINT_MAX;
    pd.SampleDesc.Count = 1;
    pd.PrimitiveTopologyType = D3D12_PRIMITIVE_TOPOLOGY_TYPE_TRIANGLE;
    pd.NumRenderTargets = 1;
    pd.RTVFormats[0] = DXGI_FORMAT_R8G8B8A8_UNORM;
    pd.DSVFormat = DXGI_FORMAT_D32_FLOAT;

    // 诊断开关：1=去掉深度 2=去掉输入布局 3=两者都去
    {
        if (mode == 4) {
            pd.InputLayout = {nullptr, 0};
        }
        {
            const int m = mode;
            if (m == 1 || m == 3) {
                pd.DSVFormat = DXGI_FORMAT_UNKNOWN;
                pd.DepthStencilState.DepthEnable = FALSE;
                xbgtasix::logf("[t8] 诊断: 关闭深度");
            }
            if (m == 2 || m == 3) {
                pd.InputLayout = {nullptr, 0};
                xbgtasix::logf("[t8] 诊断: 关闭输入布局");
            }
        }
    }
    ID3D12PipelineState* pso = nullptr;
    const HRESULT hpso = dev->CreateGraphicsPipelineState(&pd, IID_ID3D12PipelineState,
                                                          reinterpret_cast<void**>(&pso));
    check(hpso, "CreateGraphicsPipelineState");
    if (FAILED(hpso)) {
        ID3D12InfoQueue* q = nullptr;
        if (SUCCEEDED(dev->QueryInterface(IID_ID3D12InfoQueue, reinterpret_cast<void**>(&q))) && q) {
            const UINT64 n = q->GetNumStoredMessages();
            for (UINT64 i = 0; i < n; ++i) {
                SIZE_T len = 0;
                q->GetMessage(i, nullptr, &len);
                if (!len) continue;
                std::vector<char> buf(len);
                auto* msg = reinterpret_cast<D3D12_MESSAGE*>(buf.data());
                if (SUCCEEDED(q->GetMessage(i, msg, &len))) {
                    xbgtasix::logf("[t8] D3D12: %s", msg->pDescription);
                }
            }
            q->Release();
        }
    }
    if (!pso) return 1;

    // 顶点 / 索引
    const UINT vb_bytes = static_cast<UINT>(mesh.verts.size() * sizeof(float));
    const UINT ib_bytes = static_cast<UINT>(mesh.idx.size() * sizeof(unsigned));
    ID3D12Resource* vb = make_buffer(dev, vb_bytes, D3D12_HEAP_TYPE_UPLOAD, D3D12_RESOURCE_STATE_GENERIC_READ);
    ID3D12Resource* ib = make_buffer(dev, ib_bytes, D3D12_HEAP_TYPE_UPLOAD, D3D12_RESOURCE_STATE_GENERIC_READ);
    ID3D12Resource* cb = make_buffer(dev, 256, D3D12_HEAP_TYPE_UPLOAD, D3D12_RESOURCE_STATE_GENERIC_READ);
    if (!vb || !ib || !cb) return 1;
    D3D12_RANGE none{0, 0};
    void* p = nullptr;
    vb->Map(0, &none, &p); std::memcpy(p, mesh.verts.data(), vb_bytes);
    ib->Map(0, &none, &p); std::memcpy(p, mesh.idx.data(), ib_bytes);
    float* mvp = nullptr;
    cb->Map(0, &none, reinterpret_cast<void**>(&mvp));

    // 纹理
    const UINT tex_row = align256(tw * 4);
    D3D12_HEAP_PROPERTIES hp{};
    hp.Type = D3D12_HEAP_TYPE_DEFAULT;
    D3D12_RESOURCE_DESC tdesc{};
    tdesc.Dimension = D3D12_RESOURCE_DIMENSION_TEXTURE2D;
    tdesc.Width = tw;
    tdesc.Height = th;
    tdesc.DepthOrArraySize = 1;
    tdesc.MipLevels = 1;
    tdesc.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
    tdesc.SampleDesc.Count = 1;
    tdesc.Layout = D3D12_TEXTURE_LAYOUT_UNKNOWN;
    ID3D12Resource* gputex = nullptr;
    check(dev->CreateCommittedResource(&hp, D3D12_HEAP_FLAG_NONE, &tdesc,
                                       D3D12_RESOURCE_STATE_COPY_DEST, nullptr, IID_ID3D12Resource,
                                       reinterpret_cast<void**>(&gputex)),
          "CreateCommittedResource(tex)");
    if (!gputex) return 1;

    ID3D12Resource* uptex = make_buffer(dev, UINT64(tex_row) * th, D3D12_HEAP_TYPE_UPLOAD,
                                        D3D12_RESOURCE_STATE_GENERIC_READ);
    if (!uptex) return 1;
    void* tp = nullptr;
    uptex->Map(0, &none, &tp);
    for (UINT y = 0; y < th; ++y)
        std::memcpy(static_cast<unsigned char*>(tp) + UINT64(y) * tex_row, &tex[size_t(y) * tw * 4], tw * 4);

    // 颜色 / 深度目标
    D3D12_RESOURCE_DESC cd{};
    cd.Dimension = D3D12_RESOURCE_DIMENSION_TEXTURE2D;
    cd.Width = kDim;
    cd.Height = kDim;
    cd.DepthOrArraySize = 1;
    cd.MipLevels = 1;
    cd.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
    cd.SampleDesc.Count = 1;
    cd.Layout = D3D12_TEXTURE_LAYOUT_UNKNOWN;
    cd.Flags = D3D12_RESOURCE_FLAG_ALLOW_RENDER_TARGET;
    D3D12_CLEAR_VALUE cv{};
    cv.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
    const float clear[4] = {0.09f, 0.10f, 0.13f, 1.0f};
    std::memcpy(cv.Color, clear, sizeof(clear));
    ID3D12Resource* color = nullptr;
    check(dev->CreateCommittedResource(&hp, D3D12_HEAP_FLAG_NONE, &cd,
                                       D3D12_RESOURCE_STATE_RENDER_TARGET, &cv, IID_ID3D12Resource,
                                       reinterpret_cast<void**>(&color)),
          "CreateCommittedResource(color)");
    if (!color) return 1;

    D3D12_RESOURCE_DESC dd = cd;
    dd.Format = DXGI_FORMAT_D32_FLOAT;
    dd.Flags = D3D12_RESOURCE_FLAG_ALLOW_DEPTH_STENCIL;
    D3D12_CLEAR_VALUE dcv{};
    dcv.Format = DXGI_FORMAT_D32_FLOAT;
    dcv.DepthStencil.Depth = 1.0f;
    ID3D12Resource* depth = nullptr;
    check(dev->CreateCommittedResource(&hp, D3D12_HEAP_FLAG_NONE, &dd,
                                       D3D12_RESOURCE_STATE_DEPTH_WRITE, &dcv, IID_ID3D12Resource,
                                       reinterpret_cast<void**>(&depth)),
          "CreateCommittedResource(depth)");
    if (!depth) return 1;

    // 描述符
    D3D12_DESCRIPTOR_HEAP_DESC shd{};
    shd.Type = D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV;
    shd.NumDescriptors = 1;
    shd.Flags = D3D12_DESCRIPTOR_HEAP_FLAG_SHADER_VISIBLE;
    ID3D12DescriptorHeap* heap = nullptr;
    check(dev->CreateDescriptorHeap(&shd, IID_ID3D12DescriptorHeap, reinterpret_cast<void**>(&heap)),
          "CreateDescriptorHeap(cbv/srv)");
    D3D12_DESCRIPTOR_HEAP_DESC rhd{};
    rhd.Type = D3D12_DESCRIPTOR_HEAP_TYPE_RTV;
    rhd.NumDescriptors = 1;
    ID3D12DescriptorHeap* rtv_heap = nullptr;
    check(dev->CreateDescriptorHeap(&rhd, IID_ID3D12DescriptorHeap,
                                    reinterpret_cast<void**>(&rtv_heap)), "CreateDescriptorHeap(rtv)");
    D3D12_DESCRIPTOR_HEAP_DESC dhd{};
    dhd.Type = D3D12_DESCRIPTOR_HEAP_TYPE_DSV;
    dhd.NumDescriptors = 1;
    ID3D12DescriptorHeap* dsv_heap = nullptr;
    check(dev->CreateDescriptorHeap(&dhd, IID_ID3D12DescriptorHeap,
                                    reinterpret_cast<void**>(&dsv_heap)), "CreateDescriptorHeap(dsv)");
    if (!heap || !rtv_heap || !dsv_heap) return 1;

    D3D12_CPU_DESCRIPTOR_HANDLE rtv = rtv_heap->GetCPUDescriptorHandleForHeapStart();
    D3D12_CPU_DESCRIPTOR_HANDLE dsv = dsv_heap->GetCPUDescriptorHandleForHeapStart();
    dev->CreateRenderTargetView(color, nullptr, rtv);
    dev->CreateDepthStencilView(depth, nullptr, dsv);

    D3D12_CPU_DESCRIPTOR_HANDLE srv_cpu = heap->GetCPUDescriptorHandleForHeapStart();
    D3D12_SHADER_RESOURCE_VIEW_DESC srvd{};
    srvd.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
    srvd.ViewDimension = D3D12_SRV_DIMENSION_TEXTURE2D;
    srvd.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
    srvd.Texture2D.MipLevels = 1;
    dev->CreateShaderResourceView(gputex, &srvd, srv_cpu);

    // 回读
    const UINT rb_bytes = kRowPitch * kDim;
    D3D12_HEAP_PROPERTIES rhp{};
    rhp.Type = D3D12_HEAP_TYPE_READBACK;
    D3D12_RESOURCE_DESC rbd{};
    rbd.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER;
    rbd.Width = rb_bytes;
    rbd.Height = 1;
    rbd.DepthOrArraySize = 1;
    rbd.MipLevels = 1;
    rbd.Format = DXGI_FORMAT_UNKNOWN;
    rbd.SampleDesc.Count = 1;
    rbd.Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;
    ID3D12Resource* rb = nullptr;
    check(dev->CreateCommittedResource(&rhp, D3D12_HEAP_FLAG_NONE, &rbd,
                                       D3D12_RESOURCE_STATE_COPY_DEST, nullptr, IID_ID3D12Resource,
                                       reinterpret_cast<void**>(&rb)),
          "CreateCommittedResource(readback)");
    if (!rb) return 1;

    // 命令
    D3D12_COMMAND_QUEUE_DESC qd{};
    qd.Type = D3D12_COMMAND_LIST_TYPE_DIRECT;
    ID3D12CommandQueue* queue = nullptr;
    check(dev->CreateCommandQueue(&qd, IID_ID3D12CommandQueue, reinterpret_cast<void**>(&queue)), "CreateCommandQueue");
    ID3D12CommandAllocator* alloc = nullptr;
    check(dev->CreateCommandAllocator(D3D12_COMMAND_LIST_TYPE_DIRECT, IID_ID3D12CommandAllocator,
                                      reinterpret_cast<void**>(&alloc)), "CreateCommandAllocator");
    ID3D12GraphicsCommandList* list = nullptr;
    check(dev->CreateCommandList(0, D3D12_COMMAND_LIST_TYPE_DIRECT, alloc, nullptr,
                                 IID_ID3D12GraphicsCommandList, reinterpret_cast<void**>(&list)),
          "CreateCommandList");
    ID3D12Fence* fence = nullptr;
    check(dev->CreateFence(0, D3D12_FENCE_FLAG_NONE, IID_ID3D12Fence, reinterpret_cast<void**>(&fence)),
          "CreateFence");
    HANDLE ev = CreateEventA(nullptr, FALSE, FALSE, nullptr);
    UINT64 fv = 0;
    if (!queue || !alloc || !list || !fence) return 1;

    auto wait = [&] {
        ++fv;
        queue->Signal(fence, fv);
        if (fence->GetCompletedValue() < fv) {
            fence->SetEventOnCompletion(fv, ev);
            WaitForSingleObject(ev, 20000);
        }
    };

    // 上传纹理
    D3D12_TEXTURE_COPY_LOCATION usrc{};
    usrc.pResource = uptex;
    usrc.Type = D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT;
    usrc.PlacedFootprint.Footprint.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
    usrc.PlacedFootprint.Footprint.Width = tw;
    usrc.PlacedFootprint.Footprint.Height = th;
    usrc.PlacedFootprint.Footprint.Depth = 1;
    usrc.PlacedFootprint.Footprint.RowPitch = tex_row;
    D3D12_TEXTURE_COPY_LOCATION udst{};
    udst.pResource = gputex;
    udst.Type = D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;
    list->CopyTextureRegion(&udst, 0, 0, 0, &usrc, nullptr);
    D3D12_RESOURCE_BARRIER tb{};
    tb.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
    tb.Transition.pResource = gputex;
    tb.Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
    tb.Transition.StateBefore = D3D12_RESOURCE_STATE_COPY_DEST;
    tb.Transition.StateAfter = D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE;
    list->ResourceBarrier(1, &tb);
    check(list->Close(), "List::Close(upload)");
    { ID3D12CommandList* ls[] = {list}; queue->ExecuteCommandLists(1, ls); wait(); }

    // 相机：绕 Y 轴 30°，距离按半径自适应
    const float eye[3] = {1.45f * std::cos(0.5f), 0.95f, 1.55f};
    const float at[3] = {0, 0, 0};
    const float up[3] = {0, 1, 0};
    // RAGE 数据是 Z-up，要转成 Y-up。行向量约定下绕 X 旋转 +90°：
    //   y' = y*cos + z*sin = z   (Z 轴朝上 -> Y 轴朝上)
    // 写成 -90° 会得到 y' = -z，模型整个倒过来（很容易看漏，因为"倒着"也能渲染成功）
    // 绕 Y 再转 180°，从车辆/衣物的正面看，而不是背面
    Mat4 model = mul(rotation_x(1.5708f), rotation_y(0.6f + 3.14159f));
    Mat4 mvp_m = mul(mul(model, look_at(eye, at, up)), perspective(0.9f, 1.0f, 0.05f, 100.0f));
    // 模型归一化到单位球：先平移 -center，再缩放 1/radius
    // 行向量约定：p' = (p - center) * s
    //   -> 旋转/缩放部分乘 s，平移部分也必须是 -center * s（少乘 s 会把模型整体推开）
    const float s = 1.0f / mesh.radius;
    Mat4 norm = Mat4::identity();
    for (int i = 0; i < 3; ++i) {
        norm.m[0][i] *= s;
        norm.m[1][i] *= s;
        norm.m[2][i] *= s;
        norm.m[3][i] = -mesh.center[i] * s;
    }
    mvp_m = mul(norm, mvp_m);
    std::memcpy(mvp, mvp_m.m, sizeof(mvp_m.m));
    xbgtasix::logf("[t8] MVP[0..3] = %.3f %.3f %.3f %.3f / [12..15] = %.3f %.3f %.3f %.3f",
                   mvp[0], mvp[1], mvp[2], mvp[3], mvp[12], mvp[13], mvp[14], mvp[15]);
    xbgtasix::logf("[t8] VB VA=0x%llx size=%u stride=20  IB VA=0x%llx size=%u  CB VA=0x%llx",
                   static_cast<unsigned long long>(vb->GetGPUVirtualAddress()), vb_bytes,
                   static_cast<unsigned long long>(ib->GetGPUVirtualAddress()), ib_bytes,
                   static_cast<unsigned long long>(cb->GetGPUVirtualAddress()));

    // 诊断：索引统计 + 退化三角形比例
    {
        unsigned maxi = 0;
        for (unsigned i : mesh.idx) maxi = i > maxi ? i : maxi;
        xbgtasix::logf("[t8] 索引: %zu 个, 最大=%u, 顶点数=%zu, 前6个=(%u,%u,%u,%u,%u,%u)",
                       mesh.idx.size(), maxi, mesh.verts.size() / 5,
                       mesh.idx[0], mesh.idx[1], mesh.idx[2], mesh.idx[3], mesh.idx[4], mesh.idx[5]);
        UINT degenerate = 0;
        for (size_t t = 0; t + 2 < mesh.idx.size(); t += 3) {
            const float* a = &mesh.verts[mesh.idx[t] * 8];
            const float* b = &mesh.verts[mesh.idx[t + 1] * 8];
            const float* c = &mesh.verts[mesh.idx[t + 2] * 8];
            const float ux = b[0] - a[0], uy = b[1] - a[1], uz = b[2] - a[2];
            const float vx = c[0] - a[0], vy = c[1] - a[1], vz = c[2] - a[2];
            const float cx = uy * vz - uz * vy, cy = uz * vx - ux * vz, cz = ux * vy - uy * vx;
            if (cx * cx + cy * cy + cz * cz < 1e-14f) ++degenerate;
        }
        xbgtasix::logf("[t8] 退化三角形 %u / %u", degenerate, mesh.triangles());
    }

    // 诊断：全部顶点的 NDC 包围盒 + 前 3 个顶点
    {
        float mn[3] = {1e9f, 1e9f, 1e9f}, mx[3] = {-1e9f, -1e9f, -1e9f};
        for (size_t i = 0; i < mesh.verts.size(); i += 8) {
            float p[4] = {mesh.verts[i], mesh.verts[i + 1], mesh.verts[i + 2], 1.0f}, o[4] = {0, 0, 0, 0};
            for (int r = 0; r < 4; ++r)
                for (int c = 0; c < 4; ++c) o[c] += p[r] * mvp_m.m[r][c];
            if (o[3] <= 0.0f) continue;
            for (int k = 0; k < 3; ++k) {
                const float v = o[k] / o[3];
                mn[k] = v < mn[k] ? v : mn[k];
                mx[k] = v > mx[k] ? v : mx[k];
            }
        }
        xbgtasix::logf("[t8] NDC 包围盒: x[%.2f,%.2f] y[%.2f,%.2f] z[%.2f,%.2f]",
                       mn[0], mx[0], mn[1], mx[1], mn[2], mx[2]);
    }

    // 诊断：把前 3 个顶点投到 NDC 看看落在哪
    for (int vi = 0; vi < 3; ++vi) {
        const float* v = &mesh.verts[vi * 8];
        float p[4] = {v[0], v[1], v[2], 1.0f}, o[4] = {0, 0, 0, 0};
        for (int r = 0; r < 4; ++r)
            for (int c = 0; c < 4; ++c) o[c] += p[r] * mvp_m.m[r][c];
        xbgtasix::logf("[t8] 顶点%d NDC = (%.2f, %.2f, %.2f) w=%.2f", vi,
                       o[0] / (o[3] ? o[3] : 1), o[1] / (o[3] ? o[3] : 1),
                       o[2] / (o[3] ? o[3] : 1), o[3]);
    }

    // 绘制
    check(alloc->Reset(), "Allocator::Reset");
    check(list->Reset(alloc, pso), "List::Reset");
    list->OMSetRenderTargets(1, &rtv, FALSE, &dsv);
    list->ClearRenderTargetView(rtv, clear, 0, nullptr);
    list->ClearDepthStencilView(dsv, D3D12_CLEAR_FLAG_DEPTH, 1.0f, 0, 0, nullptr);
    D3D12_VIEWPORT vp{0, 0, float(kDim), float(kDim), 0, 1};
    D3D12_RECT sc{0, 0, LONG(kDim), LONG(kDim)};
    list->RSSetViewports(1, &vp);
    list->RSSetScissorRects(1, &sc);
    list->SetPipelineState(pso);
    list->SetGraphicsRootSignature(root);
    list->SetDescriptorHeaps(1, &heap);
    list->SetGraphicsRootConstantBufferView(0, cb->GetGPUVirtualAddress());
    list->SetGraphicsRootDescriptorTable(1, heap->GetGPUDescriptorHandleForHeapStart());
    D3D12_VERTEX_BUFFER_VIEW vbv{vb->GetGPUVirtualAddress(), vb_bytes, 32};
    D3D12_INDEX_BUFFER_VIEW ibv{ib->GetGPUVirtualAddress(), ib_bytes, DXGI_FORMAT_R32_UINT};
    list->IASetVertexBuffers(0, 1, &vbv);
    list->IASetIndexBuffer(&ibv);
    list->IASetPrimitiveTopology(D3D_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
    if (mode == 4) {
        list->DrawInstanced(3, 1, 0, 0);
    } else {
        list->DrawIndexedInstanced(static_cast<UINT>(mesh.idx.size()), 1, 0, 0, 0);
    }

    D3D12_RESOURCE_BARRIER b{};
    b.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
    b.Transition.pResource = color;
    b.Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
    b.Transition.StateBefore = D3D12_RESOURCE_STATE_RENDER_TARGET;
    b.Transition.StateAfter = D3D12_RESOURCE_STATE_COPY_SOURCE;
    list->ResourceBarrier(1, &b);

    D3D12_TEXTURE_COPY_LOCATION dst{};
    dst.pResource = rb;
    dst.Type = D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT;
    dst.PlacedFootprint.Footprint.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
    dst.PlacedFootprint.Footprint.Width = kDim;
    dst.PlacedFootprint.Footprint.Height = kDim;
    dst.PlacedFootprint.Footprint.Depth = 1;
    dst.PlacedFootprint.Footprint.RowPitch = kRowPitch;
    D3D12_TEXTURE_COPY_LOCATION src{};
    src.pResource = color;
    src.Type = D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;
    list->CopyTextureRegion(&dst, 0, 0, 0, &src, nullptr);
    check(list->Close(), "List::Close(draw)");
    { ID3D12CommandList* ls[] = {list}; queue->ExecuteCommandLists(1, ls); wait(); }

    // 回读 + 统计非背景像素
    unsigned char* px = nullptr;
    D3D12_RANGE rr{0, rb_bytes};
    int rc = 1;
    if (SUCCEEDED(rb->Map(0, &rr, reinterpret_cast<void**>(&px))) && px) {
        UINT covered = 0;
        for (UINT y = 0; y < kDim; ++y)
            for (UINT x = 0; x < kDim; ++x) {
                const unsigned char* q = px + y * kRowPitch + x * 4;
                if (std::abs(int(q[0]) - int(clear[0] * 255)) > 6 ||
                    std::abs(int(q[1]) - int(clear[1] * 255)) > 6 ||
                    std::abs(int(q[2]) - int(clear[2] * 255)) > 6)
                    ++covered;
            }
        const double pct = 100.0 * covered / (kDim * kDim);
        xbgtasix::logf("[t8] 非背景像素 %.1f%%（%u / %u）", pct, covered, kDim * kDim);

        wchar_t wout[512];
        MultiByteToWideChar(CP_UTF8, 0, out_png, -1, wout, 512);
        if (save_png_wic(wout, px, kDim, kDim)) {
            xbgtasix::logf("[t8] 已写出 %s", out_png);
            if (pct > 2.0) {
                xbgtasix::logf("[t8] PASS: 模型占屏 %.1f%%", pct);
                rc = 0;
            } else {
                xbgtasix::logf("[t8] FAIL: 画面几乎全是背景（模型没画出来或相机不对）");
            }
        } else {
            xbgtasix::logf("[t8] FAIL: PNG 编码失败");
        }
        D3D12_RANGE w{0, 0};
        rb->Unmap(0, &w);
    }

    if (rb) rb->Release();
    if (dsv_heap) dsv_heap->Release();
    if (rtv_heap) rtv_heap->Release();
    if (heap) heap->Release();
    if (depth) depth->Release();
    if (color) color->Release();
    if (uptex) uptex->Release();
    if (gputex) gputex->Release();
    if (cb) cb->Release();
    if (ib) ib->Release();
    if (vb) vb->Release();
    if (pso) pso->Release();
    if (root) root->Release();
    if (fence) fence->Release();
    if (list) list->Release();
    if (alloc) alloc->Release();
    if (queue) queue->Release();
    if (ev) CloseHandle(ev);
    dev->Release();

    xbgtasix::logf("[t8] done rc=%d failures=%d", rc, g_fail);
    // 返回码必须同时反映 D3D 调用失败：只看像素会让 CreateRenderTargetView/
    // CreateFence 之类的失败静默通过（审查发现 6 个测试都有这个问题）
    return rc ? rc : (g_fail ? 1 : 0);
}
