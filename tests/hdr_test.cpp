// HDR 颜色换算的单元测试：纯函数 + ImDrawData 顶点颜色改写（不需要 D3D 设备）
#include "../src/hdr.h"

#include <cmath>
#include <cstdio>
#include <string>
#include <vector>

#include "imgui.h"

static int g_fail = 0;

#define CHECK(cond, ...)                                  \
    do {                                                  \
        if (!(cond)) {                                    \
            std::printf("  FAIL ");                       \
            std::printf(__VA_ARGS__);                     \
            std::printf("\n");                            \
            ++g_fail;                                     \
        } else {                                          \
            std::printf("  ok   ");                       \
            std::printf(__VA_ARGS__);                     \
            std::printf("\n");                            \
        }                                                 \
    } while (0)

static bool Near(float a, float b, float eps) { return std::fabs(a - b) <= eps; }

// 造一个只有一个顶点、一个命令的 ImDrawData，跑 Apply 后看颜色
static ImU32 RoundTrip(int mode, const char* spec, int nits, ImU32 in)
{
    (void)mode;
    hdr::Configure(spec, nits);
    hdr::Detect(nullptr);   // 强制模式时不需要交换链
    ImDrawVert v{ImVec2(0, 0), ImVec2(0, 0), in};
    hdr::ApplyVertices(&v, 1);
    return v.col;
}

int main()
{
    std::printf("hdr_test\n");

    // sRGB → 线性
    CHECK(Near(hdr::SrgbToLinear(0.0f), 0.0f, 1e-6f), "SrgbToLinear(0) = 0");
    CHECK(Near(hdr::SrgbToLinear(1.0f), 1.0f, 1e-6f), "SrgbToLinear(1) = 1");
    CHECK(Near(hdr::SrgbToLinear(0.5f), 0.2140f, 2e-3f), "SrgbToLinear(0.5) ≈ 0.2140");

    // PQ：0 → 0，1（10000 nits）→ 1，单调
    CHECK(Near(hdr::LinearToPq(0.0f), 0.0f, 1e-6f), "LinearToPq(0) = 0");
    CHECK(Near(hdr::LinearToPq(1.0f), 1.0f, 1e-4f), "LinearToPq(1) = 1");
    CHECK(hdr::LinearToPq(0.0203f) > 0.55f && hdr::LinearToPq(0.0203f) < 0.59f,
          "203 nits → PQ %.3f（应在 0.57 附近）", hdr::LinearToPq(0.0203f));
    CHECK(hdr::LinearToPq(0.1f) > hdr::LinearToPq(0.02f), "LinearToPq 单调");

    // 模式解析
    CHECK(hdr::Parse("") == hdr::kAuto && hdr::Parse("auto") == hdr::kAuto, "Parse(auto)");
    CHECK(hdr::Parse("off") == hdr::kOff && hdr::Parse("SDR") == hdr::kOff, "Parse(off)");
    CHECK(hdr::Parse("scrgb") == hdr::kScRgb && hdr::Parse("G10") == hdr::kScRgb, "Parse(scrgb)");
    CHECK(hdr::Parse("hdr10") == hdr::kHdr10 && hdr::Parse("PQ") == hdr::kHdr10, "Parse(hdr10)");
    CHECK(hdr::Parse("???") == hdr::kAuto, "Parse(未知) 退回 auto");

    // 关掉时不动
    {
        const ImU32 in = IM_COL32(128, 96, 64, 200);
        const ImU32 out = RoundTrip(hdr::kOff, "off", 203, in);
        CHECK(out == in, "off：颜色原样不动");
    }

    // scRGB：128 → 线性 0.2158 × (203/80) = 0.5476 → 140
    {
        const ImU32 in = IM_COL32(128, 128, 128, 255);
        const ImU32 out = RoundTrip(hdr::kScRgb, "scrgb", 203, in);
        const int r = (out >> IM_COL32_R_SHIFT) & 0xFF;
        CHECK(r >= 138 && r <= 142, "scRGB：128 → %d（应在 140 附近）", r);
        CHECK(((out >> IM_COL32_A_SHIFT) & 0xFF) == 255, "scRGB：alpha 不变");
    }

    // scRGB：白色仍然饱和到 255
    {
        const ImU32 out = RoundTrip(hdr::kScRgb, "scrgb", 203, IM_COL32(255, 255, 255, 255));
        CHECK(((out >> IM_COL32_R_SHIFT) & 0xFF) == 255, "scRGB：白色仍是 255");
    }

    // HDR10：203 nits 的白 → PQ 0.57 → 8 位 145
    {
        const ImU32 out = RoundTrip(hdr::kHdr10, "hdr10", 203, IM_COL32(255, 255, 255, 255));
        const int r = (out >> IM_COL32_R_SHIFT) & 0xFF;
        CHECK(r >= 145 && r <= 150, "HDR10：白 → %d（应在 147 附近）", r);
    }

    // HDR10：黑色仍然是 0
    {
        const ImU32 out = RoundTrip(hdr::kHdr10, "hdr10", 203, IM_COL32(0, 0, 0, 255));
        CHECK(((out >> IM_COL32_R_SHIFT) & 0xFF) == 0, "HDR10：黑仍是 0");
    }

    // SDR 白点调高，HDR10 的 PQ 也跟着升
    {
        const ImU32 a = RoundTrip(hdr::kHdr10, "hdr10", 100, IM_COL32(255, 255, 255, 255));
        const ImU32 b = RoundTrip(hdr::kHdr10, "hdr10", 400, IM_COL32(255, 255, 255, 255));
        const int ra = (a >> IM_COL32_R_SHIFT) & 0xFF, rb = (b >> IM_COL32_R_SHIFT) & 0xFF;
        CHECK(rb > ra, "HDR10：白点 100 → %d，400 → %d（应更高）", ra, rb);
    }

    // 配置里的 SdrNits 越界会被夹住
    {
        hdr::Configure("hdr10", 5);
        hdr::Detect(nullptr);
        const ImU32 out = RoundTrip(hdr::kHdr10, "hdr10", 5, IM_COL32(255, 255, 255, 255));
        const int r = (out >> IM_COL32_R_SHIFT) & 0xFF;
        CHECK(r > 0, "SdrNits 越界（5）被夹到 80，颜色仍是有效值（%d）", r);
    }

    if (g_fail) {
        std::printf("\n%d 项失败\n", g_fail);
        return 1;
    }
    std::printf("\n全部通过（0 项失败）\n");
    return 0;
}
