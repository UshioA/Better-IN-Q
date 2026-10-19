// 存档档案：把配置按「Steam 账号 + 存档位」分开存。
//
// 为什么需要：BetterINQ.ini 原来就一个，放在插件目录里。同一台电脑换个 Steam
// 账号，或者同一账号换一个存档（MHW 有 3 个存档位），大家会共用同一份配置——
// 套装、猫饭、激活的那一套全都会串。
//
// 现在的位置：<插件目录>\profiles\<SteamID64>_<存档位>.ini，
// 当前选的是哪一档记在 <插件目录>\profiles\profile.txt 里（机器级，不分账号）。
// 老的单份 BetterINQ.ini 不删，第一次进某个档案时复制一份过去当底子。
#pragma once

#include <string>
#include <vector>

namespace profile {

// 插件目录（带结尾反斜杠），要在 Worker 里、读配置之前调一次
void Init(const std::wstring& pluginDir);

// SteamID64 的十进制字符串；拿不到时是空串
const std::string& SteamId();

// 面板上显示的账号：SteamID64，拿不到就写「未知」
std::string AccountLabel();

// 当前存档位（1..3）
int Slot();
void SetSlot(int slot);

// 当前档案的 ini 路径（目录不存在会建）：只放套装和猫饭
std::wstring ConfigPath();

// 所有存档共用的那份：<插件目录>\BetterINQ.ini（面板设置、快捷键、语言、HDR…）
std::wstring BasePath();

// 老的单份配置：<插件目录>\BetterINQ.ini
std::wstring LegacyPath();

// 当前档案的 ini 还不存在、但老的配置在 → 复制过去当底子。返回是否复制了
bool EnsureMigrated();

// 这个账号已经有配置文件的存档位（给面板显示用）
std::vector<int> SlotsWithData();

}  // namespace profile
