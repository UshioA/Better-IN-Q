// 从原版 IN-Q 的 config.txt 导入全套：解析 + 合并规则的测试
#include "../src/config.h"
#include "../src/inqimport.h"

#include <windows.h>
#include <cstdio>
#include <cstring>

static int g_fail = 0;
#define CHECK(cond, ...) do { if (cond) std::printf("  ok   " __VA_ARGS__); \
    else { std::printf("  FAIL " __VA_ARGS__); ++g_fail; } std::printf("\n"); } while (0)

// 照原版 config.txt 的样子缩一段：中文套装名、重名（Data num 区分）、
// 字段里带转义引号、猫饭技能为 -1、快捷键前四行。
static const char* kSample =
    "Languege:Chinese\n"
    "──Selectable languages are Chinese, English, Japanese.──\n"
    "\n"
    "Reset:1\n"
    "Accept only:0\n"
    "\n"
    "Shortcuts:\n"
    "Insert\n"
    "LS+RS+LT+RT\n"
    "Delete\n"
    "LS+RS+LB+RB\n"
    "\n"
    "{\n"
    "  \"allsets\": [\n"
    "    {\n"
    "      \"Set name\": \"综合套装1\",\n"
    "      \"Equip loadout\": \"寒气回3黑龙回避\",\n"
    "      \"Item loadout\": \"黑？\",\n"
    "      \"Item shortcuts\": 1,\n"
    "      \"Quest ID\": 66845,\n"
    "      \"Meal\": 0,\n"
    "      \"Status UP\": 4,\n"
    "      \"Skill 1\": 32,\n"
    "      \"Skill 2\": 39,\n"
    "      \"Skill 3\": 43,\n"
    "      \"Data num\": 0,\n"
    "      \"User ID\": 1117123380\n"
    "    },\n"
    "    {\n"
    "      \"Set name\": \"综合套装1\",\n"
    "      \"Equip loadout\": \"default9\",\n"
    "      \"Item loadout\": \"default7\",\n"
    "      \"Item shortcuts\": 0,\n"
    "      \"Quest ID\": 66845,\n"
    "      \"Meal\": 0,\n"
    "      \"Status UP\": 4,\n"
    "      \"Skill 1\": 32,\n"
    "      \"Skill 2\": 9,\n"
    "      \"Skill 3\": 39,\n"
    "      \"Data num\": 1,\n"
    "      \"User ID\": 1117123380\n"
    "    },\n"
    "    {\n"
    "      \"Set name\": \"强杀来\",\n"
    "      \"Equip loadout\": \"强杀来\",\n"
    "      \"Item loadout\": \"煌？\",\n"
    "      \"Item shortcuts\": 1,\n"
    "      \"Quest ID\": 66844,\n"
    "      \"Meal\": 1,\n"
    "      \"Status UP\": 4,\n"
    "      \"Skill 1\": 39,\n"
    "      \"Skill 2\": 32,\n"
    "      \"Skill 3\": 9,\n"
    "      \"Data num\": 0,\n"
    "      \"User ID\": 1117123380\n"
    "    },\n"
    "    {\n"
    "      \"Set name\": \"死星\",\n"
    "      \"Equip loadout\": \"死星\\\"？\\\"\",\n"
    "      \"Item loadout\": \"煌？\",\n"
    "      \"Item shortcuts\": 1,\n"
    "      \"Quest ID\": 0,\n"
    "      \"Meal\": 0,\n"
    "      \"Status UP\": 4,\n"
    "      \"Skill 1\": -1,\n"
    "      \"Skill 2\": -1,\n"
    "      \"Skill 3\": -1,\n"
    "      \"Data num\": 0,\n"
    "      \"User ID\": 1117123380\n"
    "    }\n"
    "  ]\n"
    "}\n";

static std::wstring TempPath(const wchar_t* name)
{
    wchar_t dir[MAX_PATH];
    ::GetTempPathW(MAX_PATH, dir);
    return std::wstring(dir) + name;
}

static void WriteFile8(const std::wstring& p, const char* s)
{
    HANDLE h = ::CreateFileW(p.c_str(), GENERIC_WRITE, 0, nullptr, CREATE_ALWAYS, 0, nullptr);
    DWORD wr = 0;
    ::WriteFile(h, s, (DWORD)std::strlen(s), &wr, nullptr);
    ::CloseHandle(h);
}

int main()
{
    // ---- 解析 ----
    inqimport::Parsed parsed;
    std::string err;
    CHECK(inqimport::Parse(kSample, parsed, err), "解析样例（%s）", err.c_str());
    CHECK(parsed.sets.size() == 4, "读到 4 个全套（实际 %zu）", parsed.sets.size());
    CHECK(parsed.language == "Chinese", "语言 = %s", parsed.language.c_str());
    CHECK(parsed.hasReset && parsed.reset, "Reset 开关读到了");
    CHECK(parsed.hasAcceptOnly && !parsed.acceptOnly, "Accept only 开关读到了");
    CHECK(parsed.hkOpen == "Insert", "打开面板的键盘键 = %s", parsed.hkOpen.c_str());
    CHECK(!parsed.sets.empty() && parsed.sets[0].skill[2] == 43, "Skill 3 = %d", parsed.sets[0].skill[2]);
    CHECK(parsed.sets.size() > 3 && parsed.sets[3].equip == "死星\"？\"", "字段里的转义引号（%s）", parsed.sets[3].equip.c_str());
    CHECK(parsed.sets.size() > 1 && parsed.sets[1].linkRadial == false, "Item shortcuts 0 = 不联动");

    // ---- 导入 = 覆盖 ----
    const std::wstring file = TempPath(L"binq_import_sample.txt");
    WriteFile8(file, kSample);

    Config cfg;
    std::string notes;
    const int n = inqimport::Import(file, cfg, err, &notes);
    CHECK(n == 4, "导入 4 个全套（实际 %d，%s）", n, err.c_str());
    CHECK(cfg.language == "zh", "语言映射成 zh（得到 %s）", cfg.language.c_str());
    CHECK(cfg.reset && !cfg.acceptOnly, "Reset / Accept only 跟着导入");
    CHECK(cfg.sets.size() == 4, "配置里有 4 个全套（实际 %zu）", cfg.sets.size());
    CHECK(!cfg.sets.empty() && cfg.sets[0].name == "综合套装1", "第一个名字 = %s",
          cfg.sets.empty() ? "" : cfg.sets[0].name.c_str());
    CHECK(cfg.sets.size() > 1 && cfg.sets[1].name == "综合套装1 (2)", "重名的加序号 = %s",
          cfg.sets.size() > 1 ? cfg.sets[1].name.c_str() : "");
    CHECK(cfg.sets.size() > 1 && cfg.sets[1].equip == "default9" && cfg.sets[1].item == "default7", "装备/道具套装名跟着走");
    CHECK(cfg.sets.size() > 2 && cfg.sets[2].quest == 66844, "任务 ID = %d", cfg.sets.size() > 2 ? cfg.sets[2].quest : -1);
    CHECK(!cfg.sets.empty() && cfg.sets[0].linkRadial, "联动快捷栏");

    // 猫饭：Meal=0 是「体力+50,耐力+50」，Meal=1 是「猫饭无效」；
    // Status UP 是字节偏移（4=攻 / 8=防 / 12=属耐），写进游戏的档位只有「大」
    CHECK(cfg.sets.size() == 4 && cfg.sets[2].food == 0, "Meal=1（猫饭无效）不建猫饭");
    {
        const FoodPreset& f0 = cfg.foods.at(cfg.sets[0].food);   // 综合套装1：Meal 0 / Status 4
        CHECK(f0.hp == 5 && f0.stamina == 2, "Meal=0 → 体力+50 / 耐力+50（%d/%d）", f0.hp, f0.stamina);
        CHECK(f0.attack == 3 && f0.defense == 0 && f0.elemRes == 0, "Status UP=4 → 攻击 Up（大）");
        CHECK(f0.skill[0] == 32 || f0.skill[1] == 32 || f0.skill[2] == 32, "三个技能带上了");
    }
    CHECK(cfg.foods.size() == 3, "三套有猫饭的各不相同（实际 %zu）", cfg.foods.size());
    CHECK(notes.find("猫饭无效 ×1") != std::string::npos, "notes 里说明了猫饭无效（%s）", notes.c_str());

    // 面板键不导：原版没有面板，这项是插件自己加的
    {
        Config c;
        c.hkPanel = "F9";
        inqimport::Import(file, c, err, nullptr);
        CHECK(c.hkPanel == "F9", "面板快捷键不被覆盖（%s）", c.hkPanel.c_str());
    }

    // ---- 再导一次：直接覆盖，不叠加 ----
    {
        Config c;
        LoadoutSet oldSet;
        oldSet.name = "旧的全套";
        c.sets.push_back(oldSet);
        c.foods[7] = FoodPreset();
        std::string notes2;
        const int n2 = inqimport::Import(file, c, err, &notes2);
        CHECK(n2 == 4 && c.sets.size() == 4, "导入是覆盖，不叠加（%d 个，配置里 %zu 个）", n2, c.sets.size());
        CHECK(c.FindSet("旧的全套") == nullptr, "原来的全套被顶掉了");
        CHECK(c.foods.find(7) == c.foods.end(), "原来的猫饭也被顶掉了");
        CHECK(c.foods.size() == 3, "重导也不会多出来（%zu 份）", c.foods.size());
    }

    // ---- 手柄组合当面板键：不影响导入，也不报提示 ----
    {
        std::string padSample = std::string(kSample);
        const std::size_t at = padSample.find("Shortcuts:\nInsert\n");
        if (at != std::string::npos) padSample.replace(at + 11, 6, "LS+RS+LT+RT\n");
        Config c2;
        c2.hkPanel = "F9";
        std::string notes3;
        inqimport::Parsed p2;
        CHECK(inqimport::Parse(padSample, p2, err) && p2.hkOpen == "LS+RS+LT+RT", "手柄组合也读得出（%s）", p2.hkOpen.c_str());
        const std::wstring f2 = TempPath(L"binq_import_pad.txt");
        WriteFile8(f2, padSample.c_str());
        inqimport::Import(f2, c2, err, &notes3);
        CHECK(c2.hkPanel == "F9", "手柄组合没被当成面板键（%s）", c2.hkPanel.c_str());
        CHECK(c2.sets.size() == 4, "快捷键那行是什么都不影响导入");
        ::DeleteFileW(f2.c_str());
    }

    // ---- Resolve：文件夹 / 旁边自动找 ----
    {
        const std::wstring ini = TempPath(L"binq_import_dir\\BetterINQ.ini");
        CHECK(inqimport::Resolve("", ini).empty(), "旁边没有 config.txt 时返回空");

        const std::wstring dir = TempPath(L"binq_import_dir");
        ::CreateDirectoryW(dir.c_str(), nullptr);
        const std::wstring cfgInDir = dir + L"\\config.txt";
        WriteFile8(cfgInDir, kSample);
        const std::wstring found = inqimport::Resolve("", ini);
        CHECK(!found.empty(), "在 ini 旁边找到 config.txt（%s）", inqimport::Narrow(found).c_str());
        CHECK(inqimport::Resolve(inqimport::Narrow(dir), ini) == cfgInDir, "InqConfig 写文件夹也能找到");
        CHECK(inqimport::Resolve(inqimport::Narrow(cfgInDir), ini) == cfgInDir, "InqConfig 写文件也能找到");
        CHECK(inqimport::Resolve(inqimport::Narrow(dir + L"\\nope"), ini).empty(), "写错路径返回空");
        ::DeleteFileW(cfgInDir.c_str());
        ::RemoveDirectoryW(dir.c_str());
    }

    // ---- 真实文件（本机装了原版才有）：14 套 ----
    {
        const std::wstring real = L"E:\\IN-Q.CT\\config.txt";
        if (::GetFileAttributesW(real.c_str()) != INVALID_FILE_ATTRIBUTES) {
            Config c3;
            std::string notes4;
            const int n3 = inqimport::Import(real, c3, err, &notes4);
            CHECK(n3 == 14, "真实 config.txt 导入 14 个全套（实际 %d，%s）", n3, err.c_str());
            CHECK(c3.sets.size() == 14, "配置里 14 个（实际 %zu）", c3.sets.size());
            CHECK(c3.language == "zh", "真实文件语言 = %s", c3.language.c_str());
            CHECK(!c3.foods.empty(), "真实文件建了猫饭（%zu 份）", c3.foods.size());
            CHECK(c3.FindSet("综合套装1") != nullptr, "找到「综合套装1」");
            const LoadoutSet* s4 = c3.FindSet("综合套装4");
            CHECK(s4 && s4->food == 0, "真实文件里 Meal=1 的「综合套装4」没有猫饭");
            const LoadoutSet* s1 = c3.FindSet("综合套装1");
            CHECK(s1 && s1->food && c3.foods.at(s1->food).hp == 5 && c3.foods.at(s1->food).attack == 3,
                  "真实文件里 Meal=0 + Status UP=4 → 体力+50 且攻击 Up");
        } else {
            std::printf("  skip 本机没有原版 config.txt，跳过真实文件检查\n");
        }
    }

    ::DeleteFileW(file.c_str());

    if (g_fail) {
        std::printf("失败 %d 项\n", g_fail);
        return 1;
    }
    std::printf("全部通过\n");
    return 0;
}
