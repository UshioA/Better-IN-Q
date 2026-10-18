#pragma once
// 任务表、猫饭技能名（都从 INQ.CT 抽出来，见 tools/gen_quests.py）
#include <string>

struct QuestGroup { const char* zh; const char* en; const char* ja; };
struct QuestInfo { int group; int id; const char* zh; const char* en; const char* ja; const char* pinyin; };
struct FoodSkillName { int id; const char* zh; };

namespace gamedata {

const QuestGroup* Groups(int& count);
const QuestInfo* Quests(int& count);
const QuestInfo* FindQuest(int id);
// 搜任务（只认中文名）：
//   数字      ID 开头是它（668 → 66845、66846…）
//   字母      中文名的拼音：从任意一个字起，按顺序每个字打全拼或拼音开头都行，
//             heilong、hl、hlong、chuanshuo、cszdhl 都能搜到「传说中的黑龙」；多音字几个读音都认
//   其它      中文名里包含它
bool QuestMatches(const QuestInfo& q, const std::string& needle);

const FoodSkillName* FoodSkills(int& count);
const char* FoodSkillNameOf(int id);   // 找不到返回 "?"

} // namespace gamedata
