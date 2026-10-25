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
    CHECK(c.hkRestart == "LT+RT+L3+R3 | Delete", "默认重开键 = %s", c.hkRestart.c_str());
    CHECK(c.hkPanel == "LB+LT+X | Insert", "默认面板键 = %s", c.hkPanel.c_str());
    CHECK(c.hkResetOnly == "LB+RB+L3+R3 | Home", "默认只重置键 = %s", c.hkResetOnly.c_str());
    CHECK(c.enabled && !c.reset && !c.acceptOnly && !c.wyvern && c.quickBlackout, "默认功能开关");
    CHECK(c.panelOpacity == 75 && c.language == "zh" && c.hdr == "auto" && c.sdrNits == 203,
          "默认面板：不透明度 75、中文、HDR auto、SDR 白点 203");
    CHECK(c.debug == 0 && c.debugDisable.empty() && c.debugForceQuit && !c.debugMealProbe && !c.debugNoWndHook,
          "默认调试：Disable 留空、ForceQuit 开、MealProbe / NoWndHook 关");
    CHECK(c.questId == 0 && c.activeSet == -1 && c.food == -1 && c.sets.empty() && c.foods.empty() && c.foodOrder.empty(),
          "默认不指定个人任务、套装或猫饭预设");
    CHECK(c.nextSetId == 1 && c.nextFoodId == 1, "默认预设编号从 1 开始");
    const Hotkey defaultRestart = ParseHotkey(c.hkRestart);
    const Hotkey defaultPanel = ParseHotkey(c.hkPanel);
    const Hotkey defaultResetOnly = ParseHotkey(c.hkResetOnly);
    CHECK(defaultRestart.bad.empty() && defaultRestart.alts.size() == 2 &&
          defaultRestart.alts[0].lt && defaultRestart.alts[0].rt &&
          defaultRestart.alts[0].pad == (XINPUT_GAMEPAD_LEFT_THUMB | XINPUT_GAMEPAD_RIGHT_THUMB) &&
          defaultRestart.alts[0].keys.empty() && !defaultRestart.alts[1].IsPad() &&
          defaultRestart.alts[1].keys.size() == 1 && defaultRestart.alts[1].keys[0] == VK_DELETE,
          "默认重开组合保留双扳机 + 双摇杆，并支持独立 Delete 入口");
    CHECK(defaultPanel.alts.size() == 2 && defaultPanel.alts[0].lt &&
          defaultPanel.alts[0].pad == (XINPUT_GAMEPAD_LEFT_SHOULDER | XINPUT_GAMEPAD_X) &&
          defaultPanel.alts[1].keys.size() == 1 && defaultPanel.alts[1].keys[0] == VK_INSERT,
          "默认面板组合保留手柄与 Insert 两个入口");
    CHECK(defaultResetOnly.bad.empty() && defaultResetOnly.alts.size() == 2 &&
          defaultResetOnly.alts[0].pad ==
          (XINPUT_GAMEPAD_LEFT_SHOULDER | XINPUT_GAMEPAD_RIGHT_SHOULDER | XINPUT_GAMEPAD_LEFT_THUMB | XINPUT_GAMEPAD_RIGHT_THUMB) &&
          !defaultResetOnly.alts[0].lt && !defaultResetOnly.alts[0].rt && defaultResetOnly.alts[0].keys.empty() &&
          !defaultResetOnly.alts[1].IsPad() && defaultResetOnly.alts[1].keys.size() == 1 &&
          defaultResetOnly.alts[1].keys[0] == VK_HOME,
          "默认只重置组合保留双肩键 + 双摇杆，并支持独立 Home 入口");
    ::DeleteFileW(def.c_str());

    // ---- 你的「米拉」配置（从 IN-Q 的 config.txt 搬过来之后、面板存过一次的样子），带 BOM 和行内注释 ----
    const std::wstring p = TempFile(L"binq_cfg_test.ini");
    WriteFile8(p,
        "\xEF\xBB\xBF; 注释\r\n"
        "[General]\r\n"
        "QuestID=66845   ; 黑龙\r\n"
        "Reset=1\r\n"
        "AcceptOnly=0\r\n"
        "WyvernStart=1\r\n"
        "ActiveSet=1\r\n"
        "Food=1\r\n"
        "QuickBlackout=0\r\n"
        "[Hotkeys]\r\n"
        "Restart=LB+RB+LT+RT | Ctrl+R\r\n"
        "Panel=Insert\r\n"
        "[Set.1]\r\n"
        "Name=米拉\r\n"
        "Equip=7\r\n"
        "Item=#3\r\n"
        "LinkRadial=1\r\n"
        "Food=1\r\n"
        "Quest=66845\r\n"
        "[Set.2]\r\n"
        "Name=备用\r\n"
        "Equip=12\r\n"
        "[Food.1]\r\n"
        "HP=5\r\n"
        "Stamina=2\r\n"
        "Attack=3\r\n"
        "Skills=32, 9 ,39\r\n");
    CHECK(LoadConfig(p, c, err), "读入示例配置");
    CHECK(c.questId == 66845 && c.reset && !c.acceptOnly && c.wyvern && !c.quickBlackout, "[General] 各项");
    CHECK(c.activeSet == 1 && c.food == 1, "ActiveSet / Food（就是编号 %d）", c.activeSet);
    CHECK(c.sets.size() == 2, "两套全套，实际 %zu", c.sets.size());
    const LoadoutSet* s = c.FindSet("米拉");
    CHECK(s && s->equip == "7" && s->item == "#3" && s->linkRadial && s->food == 1 && s->quest == 66845,
          "[Set.1] 各项");
    CHECK(c.FindSet("备用") && c.FindSet("备用")->equip == "12", "[Set.2] Equip=12");
    CHECK(c.foods.count(1) == 1, "[Food.1] 存在");
    const FoodPreset& f = c.foods[1];
    CHECK(f.hp == 5 && f.stamina == 2 && f.attack == 3 && f.defense == 0 && f.elemRes == 0, "猫饭能力");
    CHECK(f.skill[0] == 32 && f.skill[1] == 9 && f.skill[2] == 39, "猫饭技能 %d,%d,%d", f.skill[0], f.skill[1], f.skill[2]);
    ::DeleteFileW(p.c_str());

    // ---- 仓库中的示例配置（仅作读取 fixture；发布包不带 ini，也不带这些个人预设）----
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
        CHECK(m && m->equip == "7" && m->item == "#3" && m->linkRadial && m->food == 1 && m->quest == 66845, "米拉套装");
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
        LoadoutSet s1; s1.id = 1; s1.name = "米拉"; s1.equip = "7"; s1.item = "#3"; s1.linkRadial = true; s1.food = 2; s1.quest = 66845;
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
        CHECK(b.sets.size() == 3 && b.sets[0].id == 1 && b.sets[0].name == "米拉" && b.sets[0].equip == "7" &&
              b.sets[0].item == "#3" && b.sets[0].linkRadial && b.sets[0].food == 2 && b.sets[0].quest == 66845 &&
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

    // ---- 只认新格式：段名不是编号的段直接跳过，ActiveSet 指不到人就当没选 ----
    {
        const std::wstring strict = TempFile((L"biq_strict_" + std::to_wstring(::GetCurrentProcessId()) + L".ini").c_str());
        // 旧格式的段名（名字、0、负数）都不再当套装读
        WriteFile8(strict,
            "[General]\nActiveSet=1\n"
            "[Set.米拉]\nEquip=first\n"
            "[Set.0]\nEquip=zero\n"
            "[Set.-2]\nEquip=neg\n"
            "[Set.1]\nName=new\nEquip=7\n"
            "[Food.喵]\nHP=5\n"
            "[Food.1]\nName=日常\nHP=5\n");
        Config b;
        CHECK(LoadConfig(strict, b, err) && b.sets.size() == 1, "只读 [Set.编号]，实际 %zu 套", b.sets.size());
        CHECK(b.sets[0].id == 1 && b.sets[0].name == "new" && b.sets[0].equip == "7", "留下的是 [Set.1]");
        CHECK(b.foods.size() == 1 && b.foods.count(1) == 1 && b.foods[1].hp == 5, "只读 [Food.编号]");

        // ActiveSet 指向不存在的编号：按「不套用」处理，不能留一个悬空编号
        WriteFile8(strict, "[General]\nActiveSet=9\n[Set.1]\nName=a\n");
        CHECK(LoadConfig(strict, b, err) && b.activeSet == -1, "ActiveSet 指不到人 → -1（实际 %d）", b.activeSet);
        WriteFile8(strict, "[General]\nActiveSet=0\n[Set.1]\nName=a\n");
        CHECK(LoadConfig(strict, b, err) && b.activeSet == -1, "ActiveSet=0 也是不套用");
        WriteFile8(strict, "[General]\nActiveSet=-1\n[Set.1]\nName=a\n");
        CHECK(LoadConfig(strict, b, err) && b.activeSet == -1, "ActiveSet=-1 就是不套用");

        // 同一编号写两遍：留第一份，不重排、不改号
        WriteFile8(strict, "[Set.3]\nName=第一份\n[Set.3]\nName=第二份\n[Set.4]\nName=另一套\n");
        CHECK(LoadConfig(strict, b, err) && b.sets.size() == 2 && b.sets[0].id == 3 && b.sets[0].name == "第一份" &&
              b.sets[1].id == 4, "编号重复时留第一份（%zu 套）", b.sets.size());

        // Name= 空着就是空名字，不会被当成旧格式的段名
        WriteFile8(strict, "[General]\nActiveSet=7\n[Set.7]\nName=\nEquip=7\n[Set.8]\nName=0\n");
        CHECK(LoadConfig(strict, b, err) && b.sets.size() == 2 && b.sets[0].id == 7 &&
              b.sets[0].name.empty() && b.activeSet == 7 && b.sets[1].name == "0", "空 Name 也是新格式");
        CHECK(SaveConfig(strict, b) && LoadConfig(strict, b, err) && b.sets[0].name.empty() &&
              b.sets[0].id == 7 && b.activeSet == 7, "存回去还是同一套");
        ::DeleteFileW(strict.c_str());
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
        CHECK(c2.activeSet == -1 && c2.food == -1, "ClearLoadouts 清掉当前选中");
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
        CHECK(cur.sets.size() == 0 && cur.activeSet == -1 && cur.food == -1, "切到空档：选中项也清掉");
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
        s.equip = "12";
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

    // ---- Real filesystem failures: neither staging nor failed base commit may lose old data. ----
    {
        const std::wstring stem = TempFile((L"biq_transaction_" + std::to_wstring(::GetCurrentProcessId())).c_str());
        const std::wstring base = stem + L"_base.ini", prof = stem + L"_profile.ini";
        const std::wstring missing = stem + L"_missing\\profile.ini";
        const char* original = "\xEF\xBB\xBF[General]\r\nActiveSet=1\r\nLanguage=ja\r\n[Set.1]\r\nName=old\r\nEquip=7\r\n";
        WriteFile8(base, original);
        Config a;
        CHECK(LoadConfig(base, a, err), "load migration source fixture");
        std::string before, after, profBefore, profAfter;
        ReadAllForTest(base, before);
        CHECK(!SaveSplit(base, missing, a), "profile staging failure is reported");
        CHECK(ReadAllForTest(base, after) && after == before, "profile staging failure leaves source byte-for-byte intact");
        CHECK(!LoadSplit(base, missing, a, err), "failed first migration is reported");
        CHECK(ReadAllForTest(base, after) && after == before && a.FindSet("old"), "failed migration retains source and loaded data");

        CHECK(SaveSplit(base, base, a), "same-path SaveSplit stores full config");
        Config same;
        CHECK(LoadConfig(base, same, err) && same.FindSet("old"), "same-path save never strips loadouts");
        WriteFile8(base, original);
        CHECK(LoadSplit(base, prof, a, err), "first migration succeeds");
        CHECK(ReadAllForTest(prof, profBefore) && profBefore.find("Equip=7") != std::string::npos,
              "migration commits profile data");
        profBefore = "\xEF\xBB\xBF" + profBefore;
        WriteFile8(prof, profBefore.c_str()); // Rollback must preserve BOM and comments too.
        ReadAllForTest(base, before);
        CHECK(before.find("[Set.1]") == std::string::npos, "migration strips base only after profile saved");

        // Holding a handle without FILE_SHARE_DELETE makes the atomic base rename fail,
        // after both staging writes and the profile commit have succeeded.
        HANDLE lock = ::CreateFileW(base.c_str(), GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE,
                                    nullptr, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
        CHECK(lock != INVALID_HANDLE_VALUE, "lock base against replacement");
        a.language = "en";
        if (!a.sets.empty()) a.sets[0].equip = "9";
        CHECK(!SaveSplit(base, prof, a), "base replacement failure is reported");
        CHECK(ReadAllForTest(base, after) && after == before, "failed base commit leaves old base intact");
        CHECK(ReadAllForTest(prof, profAfter) && profAfter == profBefore, "failed base commit rolls profile back byte-for-byte");
        if (lock != INVALID_HANDLE_VALUE) ::CloseHandle(lock);

        lock = ::CreateFileW(prof.c_str(), GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE,
                             nullptr, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
        CHECK(lock != INVALID_HANDLE_VALUE && !SaveSplit(base, prof, a), "profile replacement failure is reported");
        CHECK(ReadAllForTest(base, after) && after == before && ReadAllForTest(prof, profAfter) &&
              profAfter == profBefore, "profile replacement failure changes neither file");
        if (lock != INVALID_HANDLE_VALUE) ::CloseHandle(lock);

        WriteFile8(base, original);
        ReadAllForTest(base, before);
        CHECK(LoadSplit(base, prof, a, err) && ReadAllForTest(base, after) && after == before &&
              ReadAllForTest(prof, profAfter) && profAfter == profBefore,
              "existing profile load never overwrites either user file during migration");
        ::DeleteFileW(prof.c_str());

        WriteFile8(base, original);
        ReadAllForTest(base, before);
        lock = ::CreateFileW(base.c_str(), GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE,
                             nullptr, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
        CHECK(lock != INVALID_HANDLE_VALUE && !LoadSplit(base, prof, a, err), "migration base commit failure is reported");
        CHECK(ReadAllForTest(base, after) && after == before &&
              ::GetFileAttributesW(prof.c_str()) == INVALID_FILE_ATTRIBUTES, "failed migration keeps source and removes newly created profile");
        if (lock != INVALID_HANDLE_VALUE) ::CloseHandle(lock);
        ::DeleteFileW(base.c_str());
    }

    // ---- 老配置一次性迁移：名字 -> 编号（这是改写，不是读取兼容） ----
    {
        Config mc;
        auto mk = [](int n, const char* name, int store) {
            RefName r; r.number = n; r.name = name; r.store = store; return r;
        };
        const std::vector<RefName> eq = {mk(1, "米拉", 1), mk(3, "死星", 3)};
        const std::vector<RefName> it = {mk(2, "黑？", 5), mk(4, "煌？", 7)};

        LoadoutSet a; a.id = 1; a.name = "A"; a.equip = "米拉"; a.item = "黑？";
        LoadoutSet b; b.id = 2; b.name = "B"; b.equip = "7";    b.item = "4";
        LoadoutSet c; c.id = 3; c.name = "C"; c.equip = "不存在"; c.item = "#5";
        LoadoutSet d; d.id = 4; d.name = "D"; d.equip = "999";  d.item = "未知";
        mc.sets = {a, b, c, d};
        CHECK(NeedsLoadoutMigration(mc), "有按名字存的值时才知道要迁移");

        int lost = 0;
        const int changed = MigrateLoadoutRefs(mc, eq, it, &lost);
        CHECK(changed == 6 && lost == 2, "迁移：6 处改动、2 处对不上");
        CHECK(mc.sets[0].equip == "1" && mc.sets[0].item == "#5", "名字换成编号（装备按套装号、道具按存放格）");
        CHECK(mc.sets[1].equip == "7" && mc.sets[1].item == "#7", "已是编号的保留；纯数字道具按显示编号换成存放格");
        CHECK(mc.sets[2].equip.empty() && mc.sets[2].item == "#5", "名字对不上留空；#存放格原样");
        CHECK(mc.sets[3].equip.empty() && mc.sets[3].item.empty(), "超范围编号与未知名字都清掉");
        CHECK(!NeedsLoadoutMigration(mc), "迁移完就没东西可迁了");

        Config keep;
        LoadoutSet k; k.id = 1; k.equip = "米拉"; k.item = "黑？";
        keep.sets = {k};
        CHECK(MigrateLoadoutRefs(keep, {}, {}, nullptr) == 0 && keep.sets[0].equip == "米拉",
              "名单还没读到时一个字都不改");
    }

    // ---- 编号 1 起：0 不是合法编号，-1 才是「不指定」 ----
    {
        const std::wstring p0 = TempFile(L"cfg_food_zero.ini");
        WriteFile8(p0,
                   "[General]\nActiveSet=-1\nFood=0\n"
                   "[Food.0]\nName=ZeroMeal\nHP=4\nStamina=2\nAttack=3\nDefense=0\nElemRes=0\nSkills=-1,-1,-1\n"
                   "[Set.1]\nName=A\nEquip=\nItem=\nLinkRadial=0\nFood=0\nQuest=0\n");
        Config c0;
        std::string e0;
        CHECK(LoadConfig(p0, c0, e0), "Food=0 读得进来（不当成读失败）");
        CHECK(c0.foods.empty(), "[Food.0] 不再读，编号从 1 起");
        CHECK(c0.food == -1 && c0.sets.size() == 1 && c0.sets[0].food == -1,
              "0 当「不指定」（0.7.1 删猫饭时就是这么写的）");
        CHECK(c0.FoodIds().empty(), "猫饭列表空");
        ::DeleteFileW(p0.c_str());

        const std::wstring p1 = TempFile(L"cfg_food_named_section.ini");
        WriteFile8(p1, "[General]\nFood=-1\n[Food.甜品]\nName=old\n[Food.0x1]\nName=bad\n[Food.2]\nName=good\n");
        Config c1;
        std::string e1;
        CHECK(LoadConfig(p1, c1, e1) && c1.foods.size() == 1 && c1.foods.count(2) == 1,
              "[Food.名字] 与 [Food.0x1] 都不当猫饭读，只读 [Food.2]");
        ::DeleteFileW(p1.c_str());
    }

    // ---- 指向墓碑（Deleted=1）的引用要保留：内容还在，照旧吃得到 ----
    {
        const std::wstring p = TempFile(L"cfg_food_tombstone.ini");
        WriteFile8(p,
                   "[General]\nActiveSet=-1\nFood=2\n"
                   "[Food.2]\nName=Old\nHP=5\nStamina=0\nAttack=0\nDefense=0\nElemRes=0\nSkills=-1,-1,-1\nDeleted=1\n"
                   "[Set.1]\nName=A\nEquip=\nItem=\nLinkRadial=0\nFood=2\nQuest=0\n");
        Config c;
        std::string e;
        CHECK(LoadConfig(p, c, e), "带墓碑的配置读得进来");
        CHECK(c.foods.count(2) && c.foods[2].deleted && c.foods[2].hp == 5, "墓碑保留内容（HP=%d）", c.foods[2].hp);
        CHECK(c.food == 2 && c.sets.size() == 1 && c.sets[0].food == 2,
              "指向墓碑的引用不会被清成 -1（food=%d set=%d）", c.food, c.sets[0].food);
        CHECK(c.FoodIds().empty(), "墓碑不出现在猫饭列表里");
        ::DeleteFileW(p.c_str());
    }

    // ---- 认出「存档里这组数值就是我们上次写的那份猫饭」 ----
    {
        Config c;
        FoodPreset f;
        f.hp = 50; f.stamina = 25; f.attack = 7; f.defense = 0; f.elemRes = 3;
        f.skill[0] = 11; f.skill[1] = -1; f.skill[2] = -1;
        c.foods[4] = f;
        const int same[8] = {50, 25, 7, 0, 3, 11, -1, -1};
        const int other[8] = {50, 25, 7, 0, 3, 12, -1, -1};
        const int none[8] = {0, 0, 0, 0, 0, -1, -1, -1};
        CHECK(MatchMealValues(c, same) == 4, "数值对得上第 4 份猫饭（%d）", MatchMealValues(c, same));
        CHECK(MatchMealValues(c, other) == 0, "差一个技能就不算同一份");
        CHECK(MatchMealValues(c, none) == 0, "全是 0（没吃）不会被认成某份猫饭");
    }

    // ---- 装载态开关：Enabled=0 能存能读；ResetSessionSelection 只清“当前选择” ----
    {
        const std::wstring p = TempFile(L"cfg_enabled.ini");
        WriteFile8(p,
                   "[General]\nEnabled=0\nActiveSet=1\nFood=-1\nQuestID=1602\n"
                   "[Set.1]\nName=A\nEquip=0\nItem=\nLinkRadial=0\nFood=-1\nQuest=0\n");
        Config c;
        std::string e;
        CHECK(LoadConfig(p, c, e), "Enabled=0 读得进来");
        CHECK(!c.enabled, "Enabled=0 = 卸载态");
        CHECK(c.activeSet == 1 && c.questId == 1602, "读配置本身不动当前选择");
        CHECK(c.ResetSessionSelection(), "有选择的时候要说“清了”");
        CHECK(c.activeSet == -1 && c.food == -1 && c.questId == 0, "清完就是什么都没选");
        CHECK(!c.ResetSessionSelection(), "已经空了就不再算改动");
        CHECK(c.sets.size() == 1, "清的是“当前选择”，列表还在");
        c.enabled = false;
        CHECK(SaveConfig(p, c), "存盘成功");
        Config d;
        CHECK(LoadConfig(p, d, e) && !d.enabled, "Enabled=0 存得下去也读得回来");
        ::DeleteFileW(p.c_str());

        const std::wstring emptyPanel = TempFile(L"cfg_empty_panel.ini");
        WriteFile8(emptyPanel, "[General]\nEnabled=0\n[Hotkeys]\nPanel=\n");
        Config safe;
        CHECK(LoadConfig(emptyPanel, safe, e), "空面板键配置读得进来");
        CHECK(safe.hkPanel == "Insert", "空面板键回退 Insert，卸载态不会永久自锁");
        ::DeleteFileW(emptyPanel.c_str());
    }

    std::printf("\n%s（%d 项失败）\n", g_fail ? "有失败" : "全部通过", g_fail);
    return g_fail ? 1 : 0;
}
