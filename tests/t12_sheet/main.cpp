// t12_sheet — 把一堆 PNG 拼成网格缩略图（WIC 解码 + 缩放 + 编码）
//
// 用途：批量渲染的载具模型、导出的贴图库，都需要"一眼看全部"。
//
// 用法：
//   t12_sheet.exe <输入目录> <输出.png> [每格像素=128] [列数=16]
//
// 输出时按文件名排序，格子在左到右、上到下排列；同时写一个 <输出>.idx.txt
// 记录"第几格 = 哪个文件"，方便回查。
#include <windows.h>
#include <wincodec.h>

#include <algorithm>
#include <cstdio>
#include <cstring>
#include <string>
#include <vector>

#include "xbgtasix/trace.h"

namespace {

std::vector<std::string> list_pngs(const char* dir) {
    std::vector<std::string> out;
    WIN32_FIND_DATAA fd{};
    const std::string pattern = std::string(dir) + "\\*.png";
    HANDLE h = FindFirstFileA(pattern.c_str(), &fd);
    if (h == INVALID_HANDLE_VALUE) return out;
    do {
        if (!(fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY)) out.push_back(fd.cFileName);
    } while (FindNextFileA(h, &fd));
    FindClose(h);
    std::sort(out.begin(), out.end());
    return out;
}

std::wstring widen(const std::string& s) {
    wchar_t buf[1024];
    MultiByteToWideChar(CP_UTF8, 0, s.c_str(), -1, buf, 1024);
    return buf;
}

}  // namespace

int main(int argc, char** argv) {
    const char* indir = argc > 1 ? argv[1] : ".";
    const char* outfile = argc > 2 ? argv[2] : "sheet.png";
    const UINT tile = argc > 3 ? static_cast<UINT>(atoi(argv[3])) : 128;
    const UINT cols = argc > 4 ? static_cast<UINT>(atoi(argv[4])) : 16;

    CoInitializeEx(nullptr, COINIT_MULTITHREADED);
    IWICImagingFactory* factory = nullptr;
    if (FAILED(CoCreateInstance(CLSID_WICImagingFactory, nullptr, CLSCTX_INPROC_SERVER,
                                IID_IWICImagingFactory, reinterpret_cast<void**>(&factory)))) {
        xbgtasix::logf("[t12] WIC 初始化失败");
        return 1;
    }

    const auto files = list_pngs(indir);
    if (files.empty()) {
        xbgtasix::logf("[t12] %s 里没有 PNG", indir);
        return 1;
    }
    const UINT rows = static_cast<UINT>((files.size() + cols - 1) / cols);
    const UINT w = cols * tile, h = rows * tile;
    xbgtasix::logf("[t12] %zu 张 -> %ux%u 网格（每格 %u，列 %u）", files.size(), w, h, tile, cols);

    // 目标位图（BGRA，先手动拼像素，再用 WIC 编码）
    std::vector<unsigned char> canvas(size_t(w) * h * 4);
    for (size_t i = 0; i < canvas.size(); i += 4) {
        canvas[i + 0] = 24; canvas[i + 1] = 24; canvas[i + 2] = 28; canvas[i + 3] = 255;
    }

    FILE* idx = nullptr;
    const std::string idx_path = std::string(outfile) + ".idx.txt";
    fopen_s(&idx, idx_path.c_str(), "w");

    for (size_t k = 0; k < files.size(); ++k) {
        const std::string path = std::string(indir) + "\\" + files[k];
        IWICBitmapDecoder* dec = nullptr;
        if (FAILED(factory->CreateDecoderFromFilename(widen(path).c_str(), nullptr, GENERIC_READ,
                                                      WICDecodeMetadataCacheOnLoad, &dec)))
            continue;
        IWICBitmapFrameDecode* frame = nullptr;
        if (FAILED(dec->GetFrame(0, &frame))) {
            dec->Release();
            continue;
        }
        IWICBitmapScaler* scaler = nullptr;
        factory->CreateBitmapScaler(&scaler);
        if (scaler && FAILED(scaler->Initialize(frame, tile, tile, WICBitmapInterpolationModeFant))) {
            scaler->Release();
            scaler = nullptr;
        }
        IWICBitmapSource* src = scaler ? static_cast<IWICBitmapSource*>(scaler)
                                       : static_cast<IWICBitmapSource*>(frame);
        std::vector<unsigned char> px(size_t(tile) * tile * 4);
        if (FAILED(src->CopyPixels(nullptr, tile * 4, static_cast<UINT>(px.size()), px.data()))) {
            xbgtasix::logf("[t12] 跳过（CopyPixels 失败）: %s", files[k].c_str());
            if (scaler) scaler->Release();
            frame->Release();
            dec->Release();
            continue;
        }
        if (scaler) scaler->Release();
        const UINT cx = static_cast<UINT>(k % cols) * tile;
        const UINT cy = static_cast<UINT>(k / cols) * tile;
        for (UINT y = 0; y < tile; ++y) {
            std::memcpy(&canvas[(size_t(cy + y) * w + cx) * 4], &px[size_t(y) * tile * 4],
                        size_t(tile) * 4);
        }
        if (idx) {
            std::fprintf(idx, "%zu\t%s\n", k, files[k].c_str());
        }
        frame->Release();
        dec->Release();
    }
    if (idx) fclose(idx);

    // 编码
    IWICStream* stream = nullptr;
    IWICBitmapEncoder* enc = nullptr;
    IWICBitmapFrameEncode* ef = nullptr;
    bool ok = false;
    if (SUCCEEDED(factory->CreateStream(&stream)) &&
        SUCCEEDED(stream->InitializeFromFilename(widen(outfile).c_str(), GENERIC_WRITE)) &&
        SUCCEEDED(factory->CreateEncoder(GUID_ContainerFormatPng, nullptr, &enc)) &&
        SUCCEEDED(enc->Initialize(stream, WICBitmapEncoderNoCache)) &&
        SUCCEEDED(enc->CreateNewFrame(&ef, nullptr)) && SUCCEEDED(ef->Initialize(nullptr))) {
        WICPixelFormatGUID fmt = GUID_WICPixelFormat32bppBGRA;
        ef->SetSize(w, h);
        ef->SetPixelFormat(&fmt);
        ok = SUCCEEDED(ef->WritePixels(h, w * 4, static_cast<UINT>(canvas.size()), canvas.data())) &&
             SUCCEEDED(ef->Commit()) && SUCCEEDED(enc->Commit());
    }
    if (ef) ef->Release();
    if (enc) enc->Release();
    if (stream) stream->Release();
    factory->Release();

    xbgtasix::logf("[t12] %s -> %s（%zu 格）", ok ? "OK" : "FAIL", outfile, files.size());
    return ok ? 0 : 1;
}
