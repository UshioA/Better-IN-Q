#pragma once
// ============================================================================
//  padfilter.h —— 游戏读手柄时经过这里
//
//  1. 面板开着：游戏读到的是「什么都没按」。关面板后要等所有键松开才恢复，
//     免得在面板里按的 A 漏给游戏。
//  2. 面板组合键（默认 视图键+菜单键）：这两个键单按在游戏里有用（地图、菜单）。
//     按下其中一个时先扣住最多 100ms：期间凑齐整个组合 → 游戏完全收不到；
//     没凑齐 → 照常放行（晚 0.1 秒）；没等到就松开了 → 补发一次点按。
//
//  做法：在游戏模块里找到「指向 XInputGetState 的指针」（导入表或它自己存的函数指针），
//  换成我们的函数。exe 有 Steam 壳，真正的导入表要等游戏跑起来才解开，所以按值找。
//  我们自己的快捷键和 ImGui 都是自己 GetProcAddress 拿的原函数，不受影响。
// ============================================================================
#include <string>

namespace padfilter {

// 找到并替换游戏的 XInputGetState 指针。xinput 还没加载时返回 false，可以过一会儿再试。
bool Install(std::string& info);
bool Installed();

void SetBlock(bool block);            // 面板开着 → 挡住全部输入

// 诊断用：游戏自己调了几次 XInputGetState（日志 perf 行）
std::string Profile();   // 面板组合键里的手柄按钮（XINPUT_GAMEPAD_* 位），0 = 不扣

// 游戏自己每帧读手柄时把原始状态记下来：快捷键那边直接用它，就不用再单独轮询 XInput。
// 轮询会和游戏的调用抢 XInput 内部的锁，蓝牙手柄上一次调用可能几毫秒，卡顿会摊到所有输入上。
// 返回 false = 这个位置最近一秒没被游戏读过。
bool Latest(unsigned long pad, unsigned short* buttons, unsigned char* lt, unsigned char* rt);
// 测试用：直接对一份手柄状态跑一遍过滤（不经过 XInput）
void TestFilter(unsigned long pad, void* xinputState);

} // namespace padfilter
