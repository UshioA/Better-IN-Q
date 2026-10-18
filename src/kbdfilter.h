#pragma once
// ============================================================================
//  kbdfilter.h —— 面板开着时，让游戏读不到键盘
//
//  面板的 WndProc 已经吃掉 WM_KEYDOWN / WM_CHAR 了，但游戏并不看消息：它自己
//  轮询 GetAsyncKeyState 之类的函数（和鼠标走 GetCursorPos 是同一套做法，所以
//  cursorfilter 才要拦）。这里把那几个函数的指针按值换掉，面板开着时一律返回
//  「什么都没按」。手柄由 padfilter 管，鼠标位置由 cursorfilter 管。
// ============================================================================
#include <string>

namespace kbdfilter {

bool Install(std::string& info);   // 换不到就返回 false（游戏可能晚点才解开导入表）
bool Installed();

// 面板开着 → 游戏读不到按键。关面板时不用恢复什么：按键状态本来就是现读的。
void SetBlock(bool block);

// 诊断用：这一次开面板总共拦下了多少次读取（面板关掉时写进日志）
void ResetStats();
std::string Stats();   // "GetAsyncKeyState=12 GetKeyState=0 ..."

} // namespace kbdfilter
