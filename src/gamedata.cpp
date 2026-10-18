#include "gamedata.h"

#include <algorithm>
#include <cctype>
#include <cstdlib>
#include <cstring>
#include <vector>

namespace {
#include "quests.inc"
#include "foodskills.inc"

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

bool QuestMatches(const QuestInfo& q, const std::string& needle)
{
    // 去掉空格和隔音符，字母转小写
    std::string n;
    bool ascii = true, digits = true;
    for (unsigned char ch : needle) {
        if (ch == ' ' || ch == '\'') continue;
        if (ch >= 0x80) ascii = false;
        if (!std::isdigit(ch)) digits = false;
        n += (char)std::tolower(ch);
    }
    if (n.empty()) return true;
    if (digits) return std::to_string(q.id).compare(0, n.size(), n) == 0;
    if (!ascii) return std::strstr(q.zh, n.c_str()) != nullptr;
    // 拼音按任务顺序缓存（只解析一次）
    static std::vector<Syllables> cache;
    if (cache.empty())
        for (const auto& k : kQuests) cache.push_back(ParsePinyin(k.pinyin));
    return PinyinMatches(cache[&q - kQuests], n);
}

const FoodSkillName* FoodSkills(int& count)
{
    count = (int)(sizeof(kFoodSkills) / sizeof(kFoodSkills[0]));
    return kFoodSkills;
}

const char* FoodSkillNameOf(int id)
{
    for (const auto& f : kFoodSkills) if (f.id == id) return f.zh;
    return "?";
}

} // namespace gamedata
