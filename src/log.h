#pragma once
#include <string>

void LogInit(const std::wstring& path);
void Log(const char* fmt, ...);

// 游戏聊天框里的系统消息（只有自己看得见）。不在场景里时先存着，进场景再发。
void Chat(const std::string& utf8);
void FlushChat();
// 最近一条提示（面板底部显示），超过 maxAgeMs 就当没有
std::string LastChat(unsigned long long maxAgeMs);

std::string ToUtf8(const std::wstring& w);
std::wstring ToWide(const std::string& s);
