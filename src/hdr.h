// HDR / 广色域交换链上的面板颜色修正。
//
// 面板是直接画进游戏后台缓冲的，而这个缓冲不一定按 sRGB 编码显示：
//   * HDR10（R10G10B10A2 + ST.2084/PQ + BT.2020）：缓冲里存的是 PQ 编码的绝对亮度，
//     我们按 sRGB 写进去的 0.5 会被当成「比 SDR 白还亮」→ 面板一片惨白、颜色发灰。
//   * scRGB（R16G16B16A16_FLOAT + 线性 + BT.709）：缓冲里存的是线性光，1.0 = 80 nits，
//     我们写 sRGB 编码值进去，等于少做了一次反 gamma → 同样偏亮、偏白。
// 所以画之前把 ImGui 顶点颜色换成目标缓冲的编码。工作在顶点颜色上（不改 imgui 后端的
// 着色器），混色是在目标空间里做的，HDR10 的渐变可能有轻微量化，实测可接受。
#pragma once

#include <string>

namespace hdr {

enum Mode { kAuto = 0, kOff = 1, kScRgb = 2, kHdr10 = 3 };

int Parse(const std::string& s);              // ini 里的写法 → Mode
const char* Name(int mode);                   // 日志/界面用
void Configure(const std::string& s, int sdrNits);   // 读配置：模式 + SDR 白点亮度
void Detect(void* swapChain);                 // 看交换链的色域/格式；强制模式时不用它
int Active();                                 // 当前实际生效的转换（kOff / kScRgb / kHdr10）
void Apply(void* drawData);                   // 换算 ImDrawData 的顶点颜色（kOff 时不动）
void ApplyVertices(void* verts, int count);   // Apply 的内核，单测直接用（ImDrawVert 数组）

// 纯函数，单测直接用
float SrgbToLinear(float c);
float LinearToPq(float c);

} // namespace hdr
