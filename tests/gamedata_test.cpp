// 任务搜索：中文 / 英文 / 日文字面，中文拼音，日文假名，日文罗马音。
#include <cstdio>
#include <string>
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

    std::printf(g_fail ? "\n%d 项失败\n" : "\n全部通过（0 项失败）\n", g_fail);
    return g_fail ? 1 : 0;
}
