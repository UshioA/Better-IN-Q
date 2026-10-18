#pragma once
#include <map>
#include <string>
#include <vector>

// 猫饭：HP、耐力、攻击、防御、属性耐性、技能×3（CT 里 fooditem 的 8 个数）
//   HP 0~5（+0 ~ +50）  耐力 0~2（+0/+25/+50）  攻击/防御/属性耐性 0~3（无/小/中/大）
//   技能 -1 = 无，0~56 见 food skill 表
struct FoodPreset {
    int hp = 0, stamina = 0, attack = 0, defense = 0, elemRes = 0;
    int skill[3] = {-1, -1, -1};
};

// 全套：装备套装 + 道具套装 + 联动快捷栏 + 猫饭 + 任务（CT 里 allsetitem 的一行）
struct LoadoutSet {
    std::string name;
    std::string equip;        // 编号 1..224，或游戏里起的名字
    std::string item;         // 编号 1..80，或游戏里起的名字
    bool linkRadial = false;  // 套道具时联动快捷栏
    int food = 0;             // 猫饭编号，0 = 不改
    int quest = 0;            // 任务 ID，0 = 不改
};

struct Config {
    // [General]
    int questId = 0;            // 重开时接的任务，0 = 上一次的
    bool reset = false;         // 重置后从据点重接（慢，但完全走正规流程）
    bool acceptOnly = false;    // 只接任务不出发
    bool wyvern = false;        // 普通地图用翼龙起始
    std::string activeSet;      // 重开时套用的全套，空 = 不套用
    int food = 0;               // 每次进任务都吃的猫饭，0 = 不管
    bool quickBlackout = true;  // 缩短转场黑屏
    int panelOpacity = 80;
    // HDR：auto = 看交换链色域自己判断；off / scrgb / hdr10 可强制。SdrNits = 播放器里
    // 的「SDR 内容亮度」，HDR 下 UI 的白点按它对齐（一般 200~300）
    std::string hdr = "auto";
    int sdrNits = 203;      // 面板不透明度 %，越小越透

    // 面板
    // 面板不透明度 %，越小越透
    std::string language = "auto";  // 界面语言 zh / en / ja，auto = 跟系统界面语言
    int debug = 0;
    std::string debugDisable;   // [Debug] Disable：排查用，关掉指定的过滤器（pad/cursor/kbd/din/overlay）

    // [Hotkeys]  多个组合用 | 隔开，例如  LB+RB+LT+RT | Ctrl+R
    std::string hkRestart = "LB+RB+LT+RT";
    std::string hkPanel = "Insert";   // 只认键盘：手柄开面板靠不住（要扣留按键）
    std::string hkResetOnly;    // 只重置任务（默认不设键，用面板上的按钮）

    std::vector<LoadoutSet> sets;
    std::map<int, FoodPreset> foods;

    const LoadoutSet* FindSet(const std::string& n) const {
        for (const auto& s : sets) if (s.name == n) return &s;
        return nullptr;
    }
};

// 读 ini（UTF-8，可带 BOM）。文件不在就写一份默认的。
bool LoadConfig(const std::wstring& path, Config& cfg, std::string& err);
bool SaveDefaultConfig(const std::wstring& path);
// 面板里改了设置后整份重写（注释是模板里的，手写的注释不会保留）。先写临时文件再替换，
// 写到一半崩了也不会留下半个文件。
bool SaveConfig(const std::wstring& path, const Config& cfg);
