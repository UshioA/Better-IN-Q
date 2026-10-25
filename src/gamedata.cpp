#include "gamedata.h"

#include <algorithm>
#include <cctype>
#include <cstdlib>
#include <cstring>
#include <vector>

#include "i18n.h"

namespace {
#include "quests.inc"
#include "foodskills.inc"
#include "foodskill_shortnames.inc"

static_assert(sizeof(kFoodSkillShortNames) / sizeof(kFoodSkillShortNames[0]) ==
              sizeof(kFoodSkills) / sizeof(kFoodSkills[0]), "Food skill short-name coverage changed");

// 一个任务名的拼音：每个字一组读音
using Syllables = std::vector<std::vector<std::string>>;

Syllables ParsePinyin(const char* py)
{
    Syllables out;
    std::string cur;
    out.emplace_back();
    for (const char* p = py;; ++p) {
        if (*p == '/' || *p == ' ' || *p == 0) {
            if (!cur.empty()) out.back().push_back(cur);
            cur.clear();
            if (*p == 0) break;
            if (*p == ' ') out.emplace_back();
        } else {
            cur += *p;
        }
    }
    if (out.back().empty()) out.pop_back();
    return out;
}

// needle[pos..] 能不能从第 j 个字起，按顺序用每个字的全拼或拼音开头拼出来。
// dead 记已经试过拼不出来的 (j, pos)，免得长名字反复试。
bool MatchFrom(const Syllables& w, std::size_t j, const std::string& n, std::size_t pos, std::vector<char>& dead)
{
    if (pos == n.size()) return true;
    if (j == w.size()) return false;
    char& d = dead[j * (n.size() + 1) + pos];
    if (d) return false;
    for (const std::string& r : w[j])
        for (std::size_t k = std::min(r.size(), n.size() - pos); k > 0; --k)
            if (n.compare(pos, k, r, 0, k) == 0 && MatchFrom(w, j + 1, n, pos + k, dead)) return true;
    d = 1;
    return false;
}

bool PinyinMatches(const Syllables& w, const std::string& n)
{
    std::vector<char> dead(w.size() * (n.size() + 1), 0);
    for (std::size_t j = 0; j < w.size(); ++j)
        if (MatchFrom(w, j, n, 0, dead)) return true;
    return false;
}

// 英文名里包含 n 吗：两边都不看空格、不看大小写（英文名带空格，输入时玩家不会打空格）
bool ContainsFolded(const char* hay, const std::string& n)
{
    if (!hay || !*hay || n.empty()) return false;
    std::size_t j = 0;
    for (const char* p = hay; *p; ++p) {
        const unsigned char ch = (unsigned char)*p;
        if (ch == ' ' || ch == '\'') continue;
        if ((char)std::tolower(ch) == n[j]) {
            if (++j == n.size()) return true;
        } else {
            j = 0;
        }
    }
    return false;
}

// ---- 日文：假名与罗马音 ----
// 日文名带汉字（伝説の黒龍），光比字面搜不到「でんせつ」「densetsu」，
// 所以生成 quests.inc 时一并存了读音（tools/gen_ja_reading.py）。

// 从 UTF-8 里读一个码点；坏字节当单字节
unsigned NextCp(const std::string& s, std::size_t& i)
{
    const unsigned char c = (unsigned char)s[i];
    if (c < 0x80) { ++i; return c; }
    std::size_t extra;
    unsigned cp;
    if ((c & 0xE0) == 0xC0) { extra = 1; cp = c & 0x1Fu; }
    else if ((c & 0xF0) == 0xE0) { extra = 2; cp = c & 0x0Fu; }
    else if ((c & 0xF8) == 0xF0) { extra = 3; cp = c & 0x07u; }
    else { ++i; return c; }
    if (i + extra >= s.size()) { ++i; return c; }
    for (std::size_t k = 1; k <= extra; ++k) cp = (cp << 6) | ((unsigned char)s[i + k] & 0x3Fu);
    i += extra + 1;
    return cp;
}

void AppendCp(std::string& out, unsigned cp)
{
    if (cp < 0x80) { out += (char)cp; return; }
    if (cp < 0x800) {
        out += (char)(0xC0 | (cp >> 6));
        out += (char)(0x80 | (cp & 0x3F));
        return;
    }
    if (cp < 0x10000) {
        out += (char)(0xE0 | (cp >> 12));
        out += (char)(0x80 | ((cp >> 6) & 0x3F));
        out += (char)(0x80 | (cp & 0x3F));
        return;
    }
    out += (char)(0xF0 | (cp >> 18));
    out += (char)(0x80 | ((cp >> 12) & 0x3F));
    out += (char)(0x80 | ((cp >> 6) & 0x3F));
    out += (char)(0x80 | (cp & 0x3F));
}

// 片假名当平假名，去掉不发声的标点：デンセツ / でんせつ / でん・せつ / でんせつー 都一样
// （读音里可能夹着拉丁字母，比如 MHWイベント，这里一并小写）
std::string NormKana(const std::string& s)
{
    std::string out;
    for (std::size_t i = 0; i < s.size();) {
        unsigned cp = NextCp(s, i);
        if (cp >= 0x30A1 && cp <= 0x30F6) cp -= 0x60;   // 片假名 → 平假名
        if (cp == ' ' || cp == 0x30FB || cp == 0x30FC || cp == 0x3000 || cp == 0x3001 ||
            cp == 0x3002 || cp == 0x30FD || cp == 0x30FE || cp == 0xFF01 || cp == 0xFF1F)
            continue;                                    // 空格 ・ ー 　 、 。 ヽ ヾ ！ ？
        if (cp < 0x80) cp = (unsigned)std::tolower((int)cp);
        AppendCp(out, cp);
    }
    return out;
}

void ReplaceAll(std::string& s, const char* from, const char* to)
{
    const std::size_t fl = std::strlen(from);
    const std::size_t tl = std::strlen(to);
    for (std::size_t p = s.find(from); p != std::string::npos; p = s.find(from, p + tl))
        s.replace(p, fl, to);
}

// 罗马字统一成一种写法：训令式（si tu hu zi）、赫本式（shi tsu fu ji）算一样，
// 长音（kokuryuu / kokuryu / koukuryuu）和促音（gakki / gaki）也折叠掉
std::string FoldRomaji(const std::string& s)
{
    std::string r;
    for (unsigned char ch : s)
        if (std::isalnum(ch)) r += (char)std::tolower(ch);
    static const char* const kTri[][2] = {
        {"sya", "sha"}, {"syu", "shu"}, {"syo", "sho"},
        {"zya", "ja"},  {"zyu", "ju"},  {"zyo", "jo"},
        {"tya", "cha"}, {"tyu", "chu"}, {"tyo", "cho"},
        {"dya", "ja"},  {"dyu", "ju"},  {"dyo", "jo"},
        {"dzu", "zu"},
    };
    for (const auto& m : kTri) ReplaceAll(r, m[0], m[1]);
    static const char* const kDuo[][2] = {
        {"si", "shi"}, {"ti", "chi"}, {"tu", "tsu"}, {"hu", "fu"},
        {"zi", "ji"},  {"di", "ji"},  {"du", "zu"},  {"wo", "o"},
    };
    for (const auto& m : kDuo) ReplaceAll(r, m[0], m[1]);
    std::string out;
    for (char ch : r) {
        if (!out.empty()) {
            const char prev = out.back();
            const bool vowel = std::strchr("aiueo", ch) != nullptr;
            if (vowel && prev == ch) continue;          // 同元音连写：aa uu
            if (prev == 'o' && ch == 'u') continue;     // おう / おお
            if (!vowel && prev == ch) continue;         // 促音、ん 连写：kk nn
        }
        out += ch;
    }
    return out;
}

struct JaIndex { std::string kana; std::string romaji; };

// 读音的归一化结果按任务下标缓存（只算一次）
const JaIndex& JaOf(const QuestInfo& q)
{
    static std::vector<JaIndex> cache;
    if (cache.empty()) {
        cache.reserve(sizeof(kQuests) / sizeof(kQuests[0]));
        for (const auto& k : kQuests) cache.push_back({NormKana(k.jaKana), FoldRomaji(k.jaRomaji)});
    }
    return cache[(std::size_t)(&q - kQuests)];
}
} // namespace

namespace gamedata {

const QuestGroup* Groups(int& count)
{
    count = (int)(sizeof(kQuestGroups) / sizeof(kQuestGroups[0]));
    return kQuestGroups;
}

const QuestInfo* Quests(int& count)
{
    count = (int)(sizeof(kQuests) / sizeof(kQuests[0]));
    return kQuests;
}

const QuestInfo* FindQuest(int id)
{
    for (const auto& q : kQuests) if (q.id == id) return &q;
    return nullptr;
}

const char* QuestName(const QuestInfo& q) { return i18n::Tr(q.zh, q.en, q.ja); }

const char* GroupName(const QuestGroup& g) { return i18n::Tr(g.zh, g.en, g.ja); }

bool QuestMatches(const QuestInfo& q, const std::string& needle)
{
    // 去掉空格和隔音符，字母转小写
    std::string n;
    bool digits = true;
    for (unsigned char ch : needle) {
        if (ch == ' ' || ch == '\'') continue;
        if (!std::isdigit(ch)) digits = false;
        n += (char)std::tolower(ch);
    }
    if (n.empty()) return true;
    if (digits) return std::to_string(q.id).compare(0, n.size(), n) == 0;
    if (ContainsFolded(q.en, n)) return true;
    // 拼音按任务顺序缓存（只解析一次）
    static std::vector<Syllables> cache;
    if (cache.empty())
        for (const auto& k : kQuests) cache.push_back(ParsePinyin(k.pinyin));
    if (PinyinMatches(cache[&q - kQuests], n)) return true;
    // 中文 / 日文的字面包含（这两种没有大小写）
    if (std::strstr(q.zh, n.c_str()) != nullptr || std::strstr(q.ja, n.c_str()) != nullptr) return true;
    // 日文读音：假名（でんせつ）和罗马音（densetsu / kokuryuu）
    const JaIndex& ja = JaOf(q);
    const std::string kana = NormKana(n);
    if (!kana.empty() && ja.kana.find(kana) != std::string::npos) return true;
    const std::string rj = FoldRomaji(n);
    if (rj.size() >= 2 && ja.romaji.find(rj) != std::string::npos) return true;
    return false;
}

const FoodSkillName* FoodSkills(int& count)
{
    count = (int)(sizeof(kFoodSkills) / sizeof(kFoodSkills[0]));
    return kFoodSkills;
}

const char* FoodSkillNameOf(int id)
{
    for (const auto& f : kFoodSkills)
        if (f.id == id) return i18n::Tr(f.zh, f.en, f.ja);
    return "?";
}

const char* FoodSkillShortNameOf(int id)
{
    for (const auto& f : kFoodSkillShortNames)
        if (f.id == id) return i18n::Tr(f.zh, f.en, f.ja);
    return "?";
}

} // namespace gamedata
