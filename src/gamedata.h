#pragma once
// 任务表、猫饭技能名（都从 INQ.CT 抽出来，见 tools/gen_quests.py、tools/gen_foodskills.py）
#include <string>

struct QuestGroup { const char* zh; const char* en; const char* ja; };
struct QuestInfo {
    int group; int id;
    const char* zh; const char* en; const char* ja;
    const char* pinyin;      // 中文名的拼音，每字一段，多音字用 / 隔开
    const char* jaKana;      // 日文名的假名读音（片假名已转平假名）
    const char* jaRomaji;    // 日文名的罗马字读音（赫本式，不带分隔符）
};
struct FoodSkillName { int id; const char* zh; const char* en; const char* ja; };

namespace gamedata {

const QuestGroup* Groups(int& count);
const QuestInfo* Quests(int& count);
const QuestInfo* FindQuest(int id);

// 按当前语言取名字（i18n::Current()）
const char* QuestName(const QuestInfo& q);
const char* GroupName(const QuestGroup& g);

// 搜任务，三种语言的名字、拼音、假名、罗马音都认：
//   数字      ID 开头是它（668 → 66845、66846…）
//   字母      英文名包含它，或中文名的拼音：从任意一个字起，按顺序每个字打全拼或拼音开头都行，
//             heilong、hl、hlong、chuanshuo、cszdhl 都能搜到「传说中的黑龙」；多音字几个读音都认
//             也是日文名的罗马音：densetsu、kokuryuu、kokuryu 都能搜到「伝説の黒龍」
//   假名      日文名的读音里包含它（片假名算平假名，ー・、。这些不参与比较）：でんせつ、こくりゅう
//   其它      中文 / 英文 / 日文名里包含它
bool QuestMatches(const QuestInfo& q, const std::string& needle);

const FoodSkillName* FoodSkills(int& count);
const char* FoodSkillNameOf(int id);   // 按当前语言；找不到返回 "?"

} // namespace gamedata
