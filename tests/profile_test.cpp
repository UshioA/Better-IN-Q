// 存档档案的单元测试：路径形状、老配置迁移、存档位切换（不碰游戏进程）
#include "../src/profile.h"

#include <windows.h>

#include <cstdio>
#include <string>

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

static bool EndsWith(const std::wstring& s, const wchar_t* tail)
{
    const std::wstring t(tail);
    return s.size() >= t.size() && s.compare(s.size() - t.size(), t.size(), t) == 0;
}

int main()
{
    // 干净的临时目录（测试自己的，不碰真实插件目录）
    const std::wstring dir = L"out\\profile_test_dir\\";
    system("rmdir /s /q out\\profile_test_dir 2>nul");

    profile::Init(dir);
    const std::wstring p1 = profile::ConfigPath();
    CHECK(EndsWith(p1, L"_1.ini"), "默认档案是 1 档（%ls）", p1.c_str());
    CHECK(p1.find(L"profiles\\") != std::wstring::npos, "档案放在 profiles\\ 下（%ls）", p1.c_str());
    CHECK(profile::Slot() == 1, "默认存档位是 1（%d）", profile::Slot());

    // 没有老配置：不迁移
    CHECK(!profile::EnsureMigrated(), "没有老配置时不迁移");

    // 有老配置：第一次进档案时复制一份
    const std::wstring legacy = profile::LegacyPath();
    HANDLE h = CreateFileW(legacy.c_str(), GENERIC_WRITE, 0, nullptr, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
    CHECK(h != INVALID_HANDLE_VALUE, "能写老配置（%ls）", legacy.c_str());
    if (h != INVALID_HANDLE_VALUE) {
        const char body[] = "[General]\r\nQuestID=5\r\n";
        DWORD wrote = 0;
        WriteFile(h, body, (DWORD)sizeof(body) - 1, &wrote, nullptr);
        CloseHandle(h);
    }
    CHECK(profile::EnsureMigrated(), "有老配置时复制成档案");
    CHECK(GetFileAttributesW(p1.c_str()) != INVALID_FILE_ATTRIBUTES, "档案文件已生成");
    CHECK(GetFileAttributesW(legacy.c_str()) != INVALID_FILE_ATTRIBUTES, "老文件保留着");
    CHECK(!profile::EnsureMigrated(), "已经迁移过就不再动它");

    // 切存档位：路径跟着变，机器级记下来
    profile::SetSlot(2);
    CHECK(profile::Slot() == 2, "切到 2 档");
    CHECK(EndsWith(profile::ConfigPath(), L"_2.ini"), "2 档的路径（%ls）", profile::ConfigPath().c_str());
    CHECK(profile::EnsureMigrated(), "2 档还没有配置，也用老配置当底子");

    profile::SetSlot(9);   // 越界夹回 1
    CHECK(profile::Slot() == 1, "越界的存档位夹回 1（%d）", profile::Slot());

    // 有文件的存档位能列出来（1 和 2 都迁移过了）
    profile::SetSlot(1);
    const std::vector<int> slots = profile::SlotsWithData();
    bool has1 = false, has2 = false;
    for (int s : slots) {
        has1 = has1 || s == 1;
        has2 = has2 || s == 2;
    }
    CHECK(has1 && has2, "1、2 两档都算「有配置」（共 %zu 档）", slots.size());

    std::printf(g_fail == 0 ? "全部通过\n" : "%d 项失败\n", g_fail);
    return g_fail == 0 ? 0 : 1;
}
