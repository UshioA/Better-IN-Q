#pragma once
// ============================================================================
//  cursorfilter.h —— 游戏挪鼠标、锁鼠标时经过这里
//
//  游戏在场景里每帧把系统光标放回窗口中间（SetCursorPos），再读光标位置算出这一帧
//  鼠标挪了多少来转视角。面板开着时这会把光标一直拽回中间，没法点面板。
//
//  面板开着时：
//    SetCursorPos  不执行，只记下游戏想放到哪
//    GetCursorPos  告诉游戏光标就在它想放的位置（算出来鼠标没动，视角不转）
//    ClipCursor    不执行，只记下；打开面板时游戏若锁着光标，改成锁在游戏窗口里
//  关面板时把光标放回游戏最后想放的位置、恢复游戏要的锁定范围，游戏无缝接着用。
//
//  做法和 padfilter 一样：在游戏模块的数据段里按值找到这几个函数的指针，换成我们的。
//  面板自己（ImGui）调的是我们 DLL 导入表里的原函数，不受影响。
// ============================================================================
#include <string>

namespace cursorfilter {

// 找到并替换游戏模块里的 SetCursorPos / GetCursorPos / ClipCursor 指针。
// 一个 SetCursorPos 都没找到时返回 false，可以过一会儿再试。
bool Install(std::string& info);
bool Installed();

void SetBlock(bool block);   // 面板开着 → 游戏挪不动、锁不住光标

} // namespace cursorfilter
