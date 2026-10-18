#pragma once
// ============================================================================
//  dinfilter.h —— 面板开着时，让游戏从 DirectInput 读不到键盘和鼠标
//
//  光拦 user32 那套（kbdfilter）不够：MHW 的键鼠走的是 DirectInput8 ——
//  游戏加载 dinput8.dll（Stracker's Loader 就是拿这个文件做代理的），键盘是
//  DirectInput 的键盘设备、鼠标是它的相对位移设备，两条路都不经过
//  GetAsyncKeyState / GetCursorPos。
//
//  做法（两路一起上）：
//   1) 换掉游戏模块里 `DirectInput8Create` 的指针；游戏调它拿到 IDirectInput8 后，
//      再换那张虚表的 CreateDevice，于是游戏之后每建的设备都会被我们截住。
//   2) 自己建一台键盘、一台鼠标，直接换它们的虚表。设备虚表是接口级、进程内共用的，
//      所以游戏**先前**建好的设备会一起被换掉——只做 1) 的时候，游戏在插件装好之前
//      就建了设备，就会整段漏掉（0.7.6 到 0.7.10 的漏洞）。
//
//  两条路换的都是设备虚表的 GetDeviceState（第 9 项）和 GetDeviceData（第 10 项）。
//  自建的那两台设备建完就 Release，只借它们的虚表指针用。
// ============================================================================
#include <string>

namespace dinfilter {

// 装不上（dinput8 还没加载 / 游戏还没导入 DirectInput8Create）返回 false，调用方隔两秒再试
bool Install(std::string& info);
bool Installed();

// 面板开着 = true：GetDeviceState / GetDeviceData 一律返回「什么都没按、没动」
void SetBlock(bool block);

// 诊断用：这一次开面板截到了几台设备（面板关掉时写进日志）
void ResetStats();
std::string Stats();

// 数字版：换掉了几张设备虚表 / 截到几台游戏自己建的设备（测试和日志用）
int Vtables();
int GameDevices();

} // namespace dinfilter
