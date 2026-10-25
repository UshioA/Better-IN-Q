// 任务搜索：中文 / 英文 / 日文字面，中文拼音，日文假名，日文罗马音。
#include <cstdio>
#include <cstring>
#include <limits>
#include <set>
#include <string>
#include <vector>
#include "../src/gamedata.h"
#include "../src/i18n.h"

static int g_fail = 0;
#define CHECK(cond, ...) do { if (cond) std::printf("  ok   " __VA_ARGS__); \
    else { std::printf("  FAIL " __VA_ARGS__); ++g_fail; } std::printf("\n"); } while (0)

static bool Has(int id, const char* needle)
{
    const QuestInfo* q = gamedata::FindQuest(id);
    return q && gamedata::QuestMatches(*q, needle);
}

static void TestFoodSkillShortNames()
{
    const i18n::Lang saved = i18n::Current();
    int count = 0;
    const FoodSkillName* skills = gamedata::FoodSkills(count);
    std::set<int> ids;
    for (int i = 0; i < count; ++i) ids.insert(skills[i].id);
    std::set<int> expectedIds;
    for (int id = -1; id <= 56; ++id) if (id != 37) expectedIds.insert(id);
    // -1 + 0..56 - 37 = 57；不为原表缺失的 37 编造技能。
    CHECK(count == 57 && ids == expectedIds, "猫饭原表完整 ID 集合（含 -1、缺 37，共 %d 条）", count);

    const i18n::Lang langs[] = {i18n::kZh, i18n::kEn, i18n::kJa};
    const char* none[] = {"无", "None", "なし"};
    std::vector<std::string> localized[i18n::kCount];
    for (int l = 0; l < i18n::kCount; ++l) {
        i18n::Set(langs[l]);
        std::set<std::string> names;
        int missing = 0, duplicate = 0, prefixed = 0, changedFullName = 0;
        for (int i = 0; i < count; ++i) {
            const char* name = gamedata::FoodSkillShortNameOf(skills[i].id);
            const std::string shortName = name ? name : "";
            localized[l].push_back(shortName);
            if (shortName.empty() || shortName == "?") ++missing;
            if (!names.insert(shortName).second) ++duplicate;
            if (shortName.find("Felyne") != std::string::npos ||
                shortName.find("猫的") != std::string::npos ||
                shortName.find("ネコの") != std::string::npos) ++prefixed;
            if (std::strcmp(gamedata::FoodSkillNameOf(skills[i].id),
                            i18n::Tr(skills[i].zh, skills[i].en, skills[i].ja)) != 0)
                ++changedFullName;
        }
        CHECK(missing == 0 && duplicate == 0 && prefixed == 0,
              "%s 短名全覆盖、非空、不重复且无公共前缀（缺 %d、重 %d、前缀 %d）",
              i18n::ToString(langs[l]), missing, duplicate, prefixed);
        CHECK(changedFullName == 0, "%s 原完整技能名不变", i18n::ToString(langs[l]));
        CHECK(std::strcmp(gamedata::FoodSkillShortNameOf(-1), none[l]) == 0,
              "%s 的 -1 本地化", i18n::ToString(langs[l]));
        const int unknown[] = {-2, 37, 57, 9999, std::numeric_limits<int>::min(), std::numeric_limits<int>::max()};
        for (int id : unknown)
            CHECK(std::strcmp(gamedata::FoodSkillShortNameOf(id), "?") == 0,
                  "%s 未知技能 %d 返回 ?", i18n::ToString(langs[l]), id);
        const int distinct[][2] = {{3, 4}, {9, 10}, {20, 21}, {43, 50}, {53, 54}, {55, 56}};
        for (const auto& pair : distinct)
            CHECK(std::strcmp(gamedata::FoodSkillShortNameOf(pair[0]),
                              gamedata::FoodSkillShortNameOf(pair[1])) != 0,
                  "%s 技能 %d / %d 可区分", i18n::ToString(langs[l]), pair[0], pair[1]);
    }

    // 明确保留等级、两类保险、生物学者/博士和大小的语义，不能只靠不同的编号区分。
    const FoodSkillName examples[] = {
        {3, "解体大", "Carver Hi", "解体大"}, {4, "解体小", "Carver Lo", "解体小"},
        {9, "防御大", "Defender Hi", "防御大"}, {10, "防御小", "Defender Lo", "防御小"},
        {20, "起身大", "Riser Hi", "起上り大"}, {21, "起身小", "Riser Lo", "起上り小"},
        {32, "大毅力", "Moxie", "ド根性"}, {39, "短催", "Booster", "短期催眠"},
        {43, "报酬保险", "Insurance", "報酬金保険"}, {50, "生命保险", "Safeguard", "生命保険"},
        {53, "生物博士", "Zoomaster", "生物博士"}, {54, "生物学者", "Biologist", "生物学者"},
        {55, "大型吸引", "Large Life", "大きさ自慢"}, {56, "小型吸引", "Small Life", "小ささ自慢"},
    };
    // 非顺序往返切换，检查每一个 ID，而非只检查一个示例（防止按 ID 缓存旧语言）。
    const i18n::Lang switches[] = {i18n::kEn, i18n::kZh, i18n::kJa, i18n::kEn, i18n::kJa, i18n::kZh};
    for (i18n::Lang lang : switches) {
        i18n::Set(lang);
        int stale = 0, wrongExample = 0;
        for (int i = 0; i < count; ++i)
            if (localized[lang][i] != gamedata::FoodSkillShortNameOf(skills[i].id)) ++stale;
        for (const auto& f : examples)
            if (std::strcmp(gamedata::FoodSkillShortNameOf(f.id), i18n::Tr(f.zh, f.en, f.ja)) != 0)
                ++wrongExample;
        CHECK(stale == 0 && wrongExample == 0, "即时切换 %s：全部短名及必要技能语义正确",
              i18n::ToString(lang));
    }
    i18n::Set(saved);
}

int main()
{
    i18n::Set(i18n::kZh);

    // 66845 传说中的黑龙 / Fade to Black / 伝説の黒龍（でんせつのこくりゅう）
    CHECK(Has(66845, "densetsu"), "罗马音前缀 densetsu");
    CHECK(Has(66845, "kokuryuu"), "罗马音 kokuryuu");
    CHECK(Has(66845, "kokuryu"), "罗马音少一个 u（长音折叠）");
    CHECK(Has(66845, "KOKURYUU"), "罗马音大写");
    CHECK(Has(66845, "densetsunokokuryuu"), "罗马音整串");
    CHECK(!Has(66845, "varuhazaku"), "罗马音不该串到别的任务");
    CHECK(Has(66845, "でんせつ"), "平假名");
    CHECK(Has(66845, "デンセツ"), "片假名当平假名");
    CHECK(Has(66845, "こくりゅう"), "汉字部分也能用假名搜");
    CHECK(Has(66845, "でん・せつ"), "假名中间的 ・ 不算");
    CHECK(Has(66845, "chuanshuo"), "拼音照旧");
    CHECK(Has(66845, "hl"), "拼音首字母照旧");
    CHECK(Has(66845, "fade to black"), "英文名照旧（带空格）");
    CHECK(Has(66845, "传说中的"), "中文名照旧");
    CHECK(Has(66845, "伝説"), "日文字面照旧");

    // 1503 死を纏うヴァルハザク（しをまとうゔぁるはざく / varuhazaku）
    CHECK(Has(1503, "varuhazaku"), "片假名任务的罗马音");
    CHECK(Has(1503, "ヴァルハザク"), "片假名字面（输入也是片假名）");
    CHECK(Has(1503, "まとう"), "假名搜汉字部分");

    // 3002 闘技大会03（とうぎたいかい03 / tougitaikai03）
    CHECK(Has(3002, "tougitaikai"), "闘技大会的罗马音");
    CHECK(Has(3002, "togitaikai"), "长音 ou 折叠");
    CHECK(Has(3002, "とうぎ"), "假名前缀");
    CHECK(Has(3002, "3002"), "ID 照旧");

    // 数据自查：每个任务都有读音，且用自己的读音搜得到自己
    int n = 0;
    const QuestInfo* qs = gamedata::Quests(n);
    int emptyKana = 0, emptyRomaji = 0;
    std::string kanaBad, romajiBad;
    for (int i = 0; i < n; ++i) {
        if (!qs[i].jaKana || !*qs[i].jaKana) ++emptyKana;
        if (!qs[i].jaRomaji || !*qs[i].jaRomaji) ++emptyRomaji;
        if (!gamedata::QuestMatches(qs[i], qs[i].jaKana)) kanaBad += " " + std::to_string(qs[i].id);
        if (!gamedata::QuestMatches(qs[i], qs[i].jaRomaji)) romajiBad += " " + std::to_string(qs[i].id);
    }
    CHECK(n == 512, "任务数 %d", n);
    CHECK(emptyKana == 0, "每个任务都有假名读音（空 %d 个）", emptyKana);
    CHECK(emptyRomaji == 0, "每个任务都有罗马音（空 %d 个）", emptyRomaji);
    CHECK(kanaBad.empty(), "每个任务用自己的假名搜得到（失败:%s）", kanaBad.c_str());
    CHECK(romajiBad.empty(), "每个任务用自己的罗马音搜得到（失败:%s）", romajiBad.c_str());

    // 乱打的假名不该命中任何任务（字母搜是拼音前缀匹配，zzzz 本来就可能命中，不作反例）
    int hits = 0;
    for (int i = 0; i < n; ++i) if (gamedata::QuestMatches(qs[i], "ぱぴぷぺぽ")) ++hits;
    CHECK(hits == 0, "乱打的假名 ぱぴぷぺぽ 命中 %d 个", hits);

    // 任务 ID 范围：列表里挑得到的任务，套装里也得认。0.7.1 里 inq.cpp 有两处边界（101..67809 闭区间
    // 和 100..67809 开区间），套装任务填 67809 会被静默吞掉、重开进的是别的任务
    CHECK(gamedata::ValidQuestId(101) && gamedata::ValidQuestId(67809), "范围两端 101 / 67809 都算合法任务");
    CHECK(!gamedata::ValidQuestId(0) && !gamedata::ValidQuestId(100) && !gamedata::ValidQuestId(67810),
          "0 / 100 / 67810 都不算任务（0 是「不指定」的意思）");
    int outOfRange = 0;
    for (int i = 0; i < n; ++i) if (!gamedata::ValidQuestId(qs[i].id)) ++outOfRange;
    CHECK(outOfRange == 0, "表里 %d 个任务全在合法范围内（越界 %d 个）", n, outOfRange);

    TestFoodSkillShortNames();

    std::printf(g_fail ? "\n%d 项失败\n" : "\n全部通过（0 项失败）\n", g_fail);
    return g_fail ? 1 : 0;
}
