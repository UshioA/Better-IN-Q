// 配置文件和快捷键写法的解析测试
#include "../src/config.h"
#include "../src/input.h"

#include <windows.h>
#include <xinput.h>
#include <cstdio>
#include <fstream>
#include <iterator>

static int g_fail = 0;
static bool ReadAllForTest(const std::wstring& path, std::string& out)
{
    HANDLE h = ::CreateFileW(path.c_str(), GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE, nullptr,
                             OPEN_EXISTING, 0, nullptr);
    if (h == INVALID_HANDLE_VALUE) return false;
    char buf[4096];
    DWORD n = 0;
    out.clear();
    while (::ReadFile(h, buf, sizeof(buf), &n, nullptr) && n > 0) out.append(buf, n);
    ::CloseHandle(h);
    return true;
}

#define CHECK(cond, ...) do { if (cond) std::printf("  ok   " __VA_ARGS__); \
    else { std::printf("  FAIL " __VA_ARGS__); ++g_fail; } std::printf("\n"); } while (0)

static std::wstring TempFile(const wchar_t* name)
{
    wchar_t dir[MAX_PATH];
    ::GetTempPathW(MAX_PATH, dir);
    return std::wstring(dir) + name;
}

static void WriteFile8(const std::wstring& p, const char* s)
{
    HANDLE h = ::CreateFileW(p.c_str(), GENERIC_WRITE, 0, nullptr, CREATE_ALWAYS, 0, nullptr);
    DWORD wr = 0;
    ::WriteFile(h, s, (DWORD)strlen(s), &wr, nullptr);
    ::CloseHandle(h);
}

int main()
{
    // ---- 默认配置：文件不存在时自动生成，生成的内容要能读回来 ----
    const std::wstring def = TempFile(L"binq_default_test.ini");
    ::DeleteFileW(def.c_str());
    Config c;
    std::string err;
    CHECK(LoadConfig(def, c, err), "不存在时生成默认配置并读入");
    CHECK(c.hkRestart == "LB+RB+LT+RT", "默认重开键 = %s", c.hkRestart.c_str());
    CHECK(c.hkPanel == "Insert", "默认面板键 = %s", c.hkPanel.c_str());
    CHECK(c.hkResetOnly.empty(), "默认没有只重置键（用面板按钮）");
    CHECK(c.quickBlackout && c.questId == 0 && c.sets.empty(), "默认开关");
    ::DeleteFileW(def.c_str());

    // ---- 你的「米拉」配置（从 IN-Q 的 config.txt 搬过来的样子），带 BOM 和行内注释 ----
    const std::wstring p = TempFile(L"binq_cfg_test.ini");
    WriteFile8(p,
        "\xEF\xBB\xBF; 注释\r\n"
        "[General]\r\n"
        "QuestID=66845   ; 黑龙\r\n"
        "Reset=1\r\n"
        "AcceptOnly=0\r\n"
        "WyvernStart=1\r\n"
        "ActiveSet=米拉\r\n"
        "Food=1\r\n"
        "QuickBlackout=0\r\n"
        "MantleFix=0\r\n"             // 老版本的开关，现在一直开着：读到也不理
        "[Hotkeys]\r\n"
        "Restart=LB+RB+LT+RT | Ctrl+R\r\n"
        "ResetOnly=Ctrl+Shift+R\r\n"  // 老版本的快捷键，已经去掉：读到也不理
        "[Set.米拉]\r\n"
        "Equip=米拉\r\n"
        "Item=米拉2\r\n"
        "LinkRadial=1\r\n"
        "Food=1\r\n"
        "Quest=66845\r\n"
        "[Set.备用]\r\n"
        "Equip=12\r\n"
        "[Food.1]\r\n"
        "HP=5\r\n"
        "Stamina=2\r\n"
        "Attack=3\r\n"
        "Skills=32, 9 ,39\r\n");
    CHECK(LoadConfig(p, c, err), "读入示例配置");
    CHECK(c.questId == 66845 && c.reset && !c.acceptOnly && c.wyvern && !c.quickBlackout, "[General] 各项");
    CHECK(c.activeSet == 1 && c.food == 1, "ActiveSet / Food（老格式按名字迁到编号 %d）", c.activeSet);
    CHECK(c.sets.size() == 2, "两套全套，实际 %zu", c.sets.size());
    const LoadoutSet* s = c.FindSet("米拉");
    CHECK(s && s->equip == "米拉" && s->item == "米拉2" && s->linkRadial && s->food == 1 && s->quest == 66845,
          "[Set.米拉] 各项");
    CHECK(c.FindSet("备用") && c.FindSet("备用")->equip == "12", "[Set.备用] Equip=12");
    CHECK(c.foods.count(1) == 1, "[Food.1] 存在");
    const FoodPreset& f = c.foods[1];
    CHECK(f.hp == 5 && f.stamina == 2 && f.attack == 3 && f.defense == 0 && f.elemRes == 0, "猫饭能力");
    CHECK(f.skill[0] == 32 && f.skill[1] == 9 && f.skill[2] == 39, "猫饭技能 %d,%d,%d", f.skill[0], f.skill[1], f.skill[2]);
    ::DeleteFileW(p.c_str());

    // ---- 随项目发的那份配置（装进游戏的就是它）----
    {
        wchar_t exe[MAX_PATH];
        ::GetModuleFileNameW(nullptr, exe, MAX_PATH);
        std::wstring root(exe);
        root = root.substr(0, root.find_last_of(L"\\/"));          // out
        root = root.substr(0, root.find_last_of(L"\\/") + 1);      // 项目根
        Config r;
        CHECK(LoadConfig(root + L"config\\BetterINQ.ini", r, err), "读入 config/BetterINQ.ini");
        const LoadoutSet* m = r.FindSet("米拉");
        CHECK(r.questId == 66845 && r.activeSet == 1 && r.food == 1 && !r.reset && !r.acceptOnly, "General");
        CHECK(m && m->equip == "米拉" && m->item == "米拉2" && m->linkRadial && m->food == 1 && m->quest == 66845, "米拉套装");
        CHECK(r.foods.count(1) && r.foods[1].hp == 5 && r.foods[1].skill[0] == 32 && r.foods[1].skill[2] == 39, "猫饭 1");
        CHECK(r.hkRestart == "LB+RB+LT+RT" && ParseHotkey(r.hkRestart).alts.size() == 1, "重开键");
    }

    // ---- 存盘再读回来：面板改完会整份重写，不能丢东西 ----
    {
        Config a;
        a.questId = 66845; a.reset = true; a.acceptOnly = false; a.wyvern = true;
        a.activeSet = 2; a.food = 2; a.quickBlackout = false; a.panelOpacity = 65; a.language = "ja";
        a.hkRestart = "LB+RB+LT+RT | Ctrl+R"; a.hkPanel = "View+Menu | Insert"; a.hkResetOnly = "Ctrl+Alt+R";
        a.debugDisable = "kbd,din";
        LoadoutSet s1; s1.id = 1; s1.name = "米拉"; s1.equip = "米拉"; s1.item = "米拉2"; s1.linkRadial = true; s1.food = 2; s1.quest = 66845;
        LoadoutSet s2; s2.id = 2; s2.name = "空的";
        LoadoutSet s3; s3.id = 3; s3.name = "米拉";   // 重名：允许
        a.sets = {s1, s2, s3};
        FoodPreset f; f.hp = 5; f.stamina = 2; f.attack = 3; f.defense = 1; f.elemRes = 2; f.skill[0] = 32; f.skill[1] = -1; f.skill[2] = 39;
        a.foods[2] = f;
        a.foods[2].name = "夜宵";
        a.foods[7] = FoodPreset();
        a.foodOrder = {7, 2};       // 显示顺序：7 排前面
        const std::wstring rp = TempFile(L"binq_roundtrip.ini");
        CHECK(SaveConfig(rp, a), "存盘");
        Config b;
        CHECK(LoadConfig(rp, b, err), "读回来");
        CHECK(b.questId == 66845 && b.reset && !b.acceptOnly && b.wyvern && b.activeSet == 2 && b.food == 2 &&
              !b.quickBlackout && b.panelOpacity == 65 && b.language == "ja", "[General] 一致（含 Language）");
        CHECK(b.hkRestart == a.hkRestart && b.hkPanel == a.hkPanel && b.hkResetOnly == a.hkResetOnly &&
              b.debugDisable == a.debugDisable,
              "[Hotkeys] 一致（含 ResetOnly）");
        CHECK(b.sets.size() == 3 && b.sets[0].id == 1 && b.sets[0].name == "米拉" && b.sets[0].equip == "米拉" &&
              b.sets[0].item == "米拉2" && b.sets[0].linkRadial && b.sets[0].food == 2 && b.sets[0].quest == 66845 &&
              b.sets[1].id == 2 && b.sets[1].name == "空的" && b.sets[1].equip.empty() &&
              b.sets[2].id == 3 && b.sets[2].name == "米拉", "全套一致（含空套装、重名套装）");
        {
            std::ifstream in(rp.c_str(), std::ios::binary);
            const std::string txt((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
            CHECK(txt.find("[Set.1]") != std::string::npos && txt.find("Name=米拉") != std::string::npos &&
                  txt.find("ActiveSet=2") != std::string::npos, "存盘写成 [Set.编号] + Name=");
        }
        CHECK(b.foods.size() == 2 && b.foods[2].hp == 5 && b.foods[2].defense == 1 && b.foods[2].elemRes == 2 &&
              b.foods[2].skill[0] == 32 && b.foods[2].skill[1] == -1 && b.foods[2].skill[2] == 39 &&
              b.foods[2].name == "夜宵" &&
              b.foods.count(7), "猫饭一致（含 -1 技能）");
    CHECK(b.foodOrder.size() == 2 && b.foodOrder[0] == 7 && b.foodOrder[1] == 2,
          "猫饭显示顺序也存下来了（%s）", ([&] {
              std::string s;
              for (int id : b.foodOrder) { if (!s.empty()) s += ','; s += std::to_string(id); }
              return s;
          })().c_str());
        Config c2;
        CHECK(SaveConfig(rp, b) && LoadConfig(rp, c2, err) && c2.sets.size() == 3 && c2.foods.size() == 2, "再存一遍还是一样");
        ::DeleteFileW(rp.c_str());
    }

    // ---- 快捷键写法 ----
    Hotkey h = ParseHotkey("LB+RB+LT+RT");
    CHECK(h.alts.size() == 1 && h.alts[0].pad == (XINPUT_GAMEPAD_LEFT_SHOULDER | XINPUT_GAMEPAD_RIGHT_SHOULDER) &&
          h.alts[0].lt && h.alts[0].rt && h.alts[0].keys.empty(), "LB+RB+LT+RT");
    h = ParseHotkey("View+Menu | Insert");
    CHECK(h.alts.size() == 2 && h.alts[0].pad == (XINPUT_GAMEPAD_BACK | XINPUT_GAMEPAD_START) &&
          h.alts[1].keys.size() == 1 && h.alts[1].keys[0] == VK_INSERT, "View+Menu | Insert");
    h = ParseHotkey("Ctrl+A");
    CHECK(h.alts.size() == 1 && h.alts[0].pad == 0 && h.alts[0].keys.size() == 2 &&
          h.alts[0].keys[0] == VK_CONTROL && h.alts[0].keys[1] == 'A', "Ctrl+A 里的 A 是键盘字母");
    h = ParseHotkey("A+B");
    CHECK(h.alts.size() == 1 && h.alts[0].pad == (XINPUT_GAMEPAD_A | XINPUT_GAMEPAD_B), "A+B 是手柄键");
    h = ParseHotkey("Share+Options");
    CHECK(h.alts.size() == 1 && h.alts[0].pad == (XINPUT_GAMEPAD_BACK | XINPUT_GAMEPAD_START), "PS 叫法 Share+Options");
    h = ParseHotkey("F5");
    CHECK(h.alts.size() == 1 && h.alts[0].keys.size() == 1 && h.alts[0].keys[0] == VK_F5, "F5");
    h = ParseHotkey("LB+Foo | RT");
    CHECK(h.alts.size() == 1 && h.alts[0].rt && h.bad == "Foo ", "认不出的组合整组丢掉并记下：'%s'", h.bad.c_str());
    h = ParseHotkey("");
    CHECK(h.alts.empty(), "空字符串 = 不设快捷键");

    // 换存档：套装和猫饭清空，面板设置一个都不能动
    {
        Config c2;
        c2.language = "ja";
        c2.hkRestart = "LB+RB+LT+RT";
        c2.panelOpacity = 65;
        c2.questId = 66845;
        LoadoutSet s;
        s.id = 3;
        s.name = "要清掉的";
        c2.sets.push_back(s);
        c2.foods[2] = FoodPreset();
        c2.foodOrder = {2};
        c2.activeSet = 3;
        c2.food = 2;
        c2.ClearLoadouts();
        CHECK(c2.sets.empty() && c2.foods.empty() && c2.foodOrder.empty(), "ClearLoadouts 清掉套装和猫饭");
        CHECK(c2.activeSet == 0 && c2.food == 0, "ClearLoadouts 清掉当前选中");
        CHECK(c2.language == "ja" && c2.hkRestart == "LB+RB+LT+RT" && c2.panelOpacity == 65,
              "ClearLoadouts 不动面板设置");
        CHECK(c2.questId == 66845, "ClearLoadouts 不动重开用的任务");
    }

    // 切档的往返：1 档有套装 → 切到 2 档（空）→ 切回 1 档，套装要还在
    {
        const std::wstring s1 = TempFile(L"biq_slot1.ini");
        const std::wstring s2 = TempFile(L"biq_slot2.ini");
        ::DeleteFileW(s1.c_str());
        ::DeleteFileW(s2.c_str());

        Config cur;
        cur.hkRestart = "LB+RB+LT+RT";
        cur.language = "ja";
        LoadoutSet mine;
        mine.id = 1;
        mine.name = "米拉";
        cur.sets.push_back(mine);
        cur.foods[3] = FoodPreset();
        cur.foodOrder = {3};
        cur.activeSet = 1;
        cur.food = 3;
        SaveConfig(s1, cur);                 // 1 档已经有配置

        SwitchProfile(cur, s1, s2);          // 1 → 2
        CHECK(cur.sets.empty() && cur.foods.empty(), "切到空档：套装和猫饭清空");
        CHECK(cur.sets.size() == 0 && cur.activeSet == 0 && cur.food == 0, "切到空档：选中项也清掉");
        CHECK(cur.hkRestart == "LB+RB+LT+RT" && cur.language == "ja", "切档不动面板设置");

        SwitchProfile(cur, s2, s1);          // 2 → 1
        CHECK(cur.sets.size() == 1 && cur.sets[0].name == "米拉", "切回来：1 档的套装还在");
        CHECK(cur.foods.size() == 1 && cur.foodOrder.size() == 1, "切回来：猫饭和顺序还在");
        CHECK(cur.activeSet == 1 && cur.food == 3, "切回来：原来选的那套/那份也还在");

        Config two;
        std::string err2;
        CHECK(LoadConfig(s2, two, err2) && two.sets.empty(), "2 档没有被 1 档的套装污染");

        SwitchProfile(cur, s1, s1);          // 自己切自己
        CHECK(cur.sets.size() == 1 && cur.sets[0].name == "米拉", "同一档切到自己：什么都不丢");

        ::DeleteFileW(s1.c_str());
        ::DeleteFileW(s2.c_str());
    }

    // ---- 分两份存：base（面板设置） + profile（套装/猫饭） ----
    {
        wchar_t dir2[MAX_PATH];
        ::GetTempPathW(MAX_PATH, dir2);
        const std::wstring baseP = std::wstring(dir2) + L"biq_split_base.ini";
        const std::wstring profP = std::wstring(dir2) + L"biq_split_prof.ini";
        ::DeleteFileW(baseP.c_str());
        ::DeleteFileW(profP.c_str());

        Config a;
        a.language = "en";
        a.panelOpacity = 42;
        a.hkRestart = "Ctrl+R";
        LoadoutSet s;
        s.id = 7;
        s.name = "夜班";
        s.equip = "米拉";
        s.food = 3;
        a.sets.push_back(s);
        FoodPreset f;
        f.name = "日常";
        f.hp = 5;
        a.foods[3] = f;
        a.foodOrder = {3};
        a.activeSet = 7;
        a.food = 3;

        CHECK(SaveSplit(baseP, profP, a), "分两份写盘");
        std::string bt, pt;
        CHECK(ReadAllForTest(baseP, bt) && ReadAllForTest(profP, pt), "两份都读回来");
        CHECK(bt.find("[Set.") == std::string::npos && bt.find("[Food.") == std::string::npos,
              "base 里没有套装/猫饭段");
        CHECK(bt.find("Restart=Ctrl+R") != std::string::npos && bt.find("Language=en") != std::string::npos,
              "base 里有快捷键和语言");
        CHECK(pt.find("[Set.7]") != std::string::npos && pt.find("ActiveSet=7") != std::string::npos,
              "profile 里有套装段和 ActiveSet");
        CHECK(pt.find("[Hotkeys]") == std::string::npos && pt.find("Language=") == std::string::npos,
              "profile 里没有快捷键/语言");

        Config b;
        std::string err3;
        CHECK(LoadSplit(baseP, profP, b, err3), "分两份读回");
        CHECK(b.language == "en" && b.panelOpacity == 42 && b.hkRestart == "Ctrl+R", "读回：面板设置来自 base");
        CHECK(b.sets.size() == 1 && b.sets[0].id == 7 && b.sets[0].name == "夜班", "读回：套装来自 profile");
        CHECK(b.activeSet == 7, "读回：ActiveSet=%d（期望 7）", b.activeSet);
        CHECK(b.food == 3, "读回：Food=%d（期望 3）", b.food);
        CHECK(b.foods.count(3) == 1, "读回：猫饭 3 存在，数量 %zu", b.foods.size());

        ::DeleteFileW(baseP.c_str());
        ::DeleteFileW(profP.c_str());
    }

    std::printf("\n%s（%d 项失败）\n", g_fail ? "有失败" : "全部通过", g_fail);
    return g_fail ? 1 : 0;
}
