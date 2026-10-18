#include "hdr.h"

#include <windows.h>
#include <dxgi1_6.h>

#include <atomic>
#include <cmath>

#include "imgui.h"
#include "log.h"

namespace hdr {
namespace {

std::atomic<int> g_mode{kAuto};
std::atomic<int> g_active{kOff};
std::atomic<int> g_nits{203};
std::atomic<unsigned> g_logged{~0u};

// BT.709 线性 → BT.2020 线性（行主序）。UI 颜色差不多都是灰和金，这一步只影响饱和度。
const float k709to2020[9] = {
    0.6274f, 0.3293f, 0.0433f,
    0.0691f, 0.9195f, 0.0114f,
    0.0164f, 0.0880f, 0.8956f,
};

inline float Clamp01(float v) { return v < 0.0f ? 0.0f : (v > 1.0f ? 1.0f : v); }

} // namespace

float SrgbToLinear(float c)
{
    c = Clamp01(c);
    if (c <= 0.04045f) return c / 12.92f;
    return std::pow((c + 0.055f) / 1.055f, 2.4f);
}

float LinearToPq(float c)
{
    c = Clamp01(c);
    if (c <= 0.0f) return 0.0f;
    const float m1 = 0.1593017578125f;    // 2610 / 16384
    const float m2 = 78.84375f;           // 2523 / 4096 * 128
    const float c1 = 0.8359375f;          // 3424 / 4096
    const float c2 = 18.8515625f;         // 2413 / 4096 * 32
    const float c3 = 18.6875f;            // 2392 / 4096 * 32
    const float p = std::pow(c, m1);
    return std::pow((c1 + c2 * p) / (1.0f + c3 * p), m2);
}

int Parse(const std::string& s)
{
    std::string k;
    for (char ch : s) {
        if (ch == ' ' || ch == '\t') continue;
        k += (char)((ch >= 'A' && ch <= 'Z') ? ch - 'A' + 'a' : ch);
    }
    if (k.empty() || k == "auto" || k == "0") return kAuto;
    if (k == "off" || k == "sdr" || k == "none") return kOff;
    if (k == "scrgb" || k == "g10" || k == "linear") return kScRgb;
    if (k == "hdr10" || k == "pq" || k == "g2084") return kHdr10;
    return kAuto;
}

const char* Name(int mode)
{
    switch (mode) {
    case kOff: return "sRGB";
    case kScRgb: return "scRGB";
    case kHdr10: return "HDR10/PQ";
    default: return "auto";
    }
}

void Configure(const std::string& s, int sdrNits)
{
    g_mode.store(Parse(s));
    if (sdrNits >= 80 && sdrNits <= 10000) g_nits.store(sdrNits);
}

void Detect(void* p)
{
    const int mode = g_mode.load();
    int active = kOff;
    DXGI_COLOR_SPACE_TYPE cs = DXGI_COLOR_SPACE_RGB_FULL_G22_NONE_P709;
    int fmt = DXGI_FORMAT_UNKNOWN;
    if (mode != kAuto) {
        active = mode;
    } else if (p) {
        // 决定编码的是后台缓冲格式，不是显示器（显示器在 HDR 模式时，游戏也可能开着 SDR 交换链）：
        //   fp16                        → scRGB（线性，1.0 = 80 nits）
        //   R10G10B10A2 + 显示器 HDR     → HDR10（PQ / BT.2020）
        //   其它（R8G8B8A8 / B8G8R8A8 / 10 位 SDR）→ 合成器自己做 sRGB，别动
        IDXGISwapChain* sc = reinterpret_cast<IDXGISwapChain*>(p);
        DXGI_SWAP_CHAIN_DESC d{};
        if (SUCCEEDED(sc->GetDesc(&d))) {
            fmt = (int)d.BufferDesc.Format;
            IDXGIOutput* out = nullptr;
            if (SUCCEEDED(sc->GetContainingOutput(&out)) && out) {
                IDXGIOutput6* o6 = nullptr;
                if (SUCCEEDED(out->QueryInterface(__uuidof(IDXGIOutput6), reinterpret_cast<void**>(&o6))) && o6) {
                    DXGI_OUTPUT_DESC1 od{};
                    if (SUCCEEDED(o6->GetDesc1(&od))) cs = od.ColorSpace;
                    o6->Release();
                }
                out->Release();
            }
            const bool hdrOut = (cs == DXGI_COLOR_SPACE_RGB_FULL_G2084_NONE_P2020 ||
                                 cs == DXGI_COLOR_SPACE_RGB_STUDIO_G2084_NONE_P2020);
            if (d.BufferDesc.Format == DXGI_FORMAT_R16G16B16A16_FLOAT)
                active = kScRgb;
            else if (d.BufferDesc.Format == DXGI_FORMAT_R10G10B10A2_UNORM && hdrOut)
                active = kHdr10;
            const unsigned tag = (unsigned)active;
            if (g_logged.exchange(tag) != tag)
                Log("hdr: 缓冲格式 %d，显示器色域 %d → %s（SDR 白点 %d nits）", fmt, (int)cs, Name(active),
                    g_nits.load());
        }
    }
    g_active.store(active);
}

int Active() { return g_active.load(); }

void ApplyVertices(void* verts, int count)
{
    const int mode = g_active.load(std::memory_order_relaxed);
    if (mode == kOff || !verts || count <= 0) return;
    const float nits = (float)g_nits.load();
    const float scale = (mode == kScRgb) ? nits / 80.0f : nits / 10000.0f;
    ImDrawVert* v = reinterpret_cast<ImDrawVert*>(verts);
    for (int k = 0; k < count; ++k) {
        const ImU32 col = v[k].col;
        const ImU32 a = (col >> IM_COL32_A_SHIFT) & 0xFF;
        float r = SrgbToLinear(((col >> IM_COL32_R_SHIFT) & 0xFF) / 255.0f);
        float g = SrgbToLinear(((col >> IM_COL32_G_SHIFT) & 0xFF) / 255.0f);
        float b = SrgbToLinear(((col >> IM_COL32_B_SHIFT) & 0xFF) / 255.0f);
        if (mode == kHdr10) {
            const float r2 = k709to2020[0] * r + k709to2020[1] * g + k709to2020[2] * b;
            const float g2 = k709to2020[3] * r + k709to2020[4] * g + k709to2020[5] * b;
            const float b2 = k709to2020[6] * r + k709to2020[7] * g + k709to2020[8] * b;
            r = LinearToPq(r2 * scale);
            g = LinearToPq(g2 * scale);
            b = LinearToPq(b2 * scale);
        } else {
            r = Clamp01(r * scale);
            g = Clamp01(g * scale);
            b = Clamp01(b * scale);
        }
        v[k].col = IM_COL32((int)(r * 255.0f + 0.5f), (int)(g * 255.0f + 0.5f), (int)(b * 255.0f + 0.5f), (int)a);
    }
}

void Apply(void* p)
{
    const int mode = g_active.load(std::memory_order_relaxed);
    if (mode == kOff || !p) return;
    ImDrawData* dd = reinterpret_cast<ImDrawData*>(p);
    if (!dd->Valid) return;
    for (int i = 0; i < dd->CmdListsCount; ++i) {
        ImDrawList* cl = dd->CmdLists[i];
        ApplyVertices(cl->VtxBuffer.Data, cl->VtxBuffer.Size);
    }
}

} // namespace hdr
