#pragma once
#include <map>
#include <string>
#include <vector>

// 猫饭：HP、耐力、攻击、防御、属性耐性、技能×3（CT 里 fooditem 的 8 个数）
//   HP 0~5（+0 ~ +50）  耐力 0~2（+0/+25/+50）  攻击/防御/属性耐性 0~3（无/小/中/大）
//   技能 -1 = 无，0~56 见 food skill 表
struct FoodPreset {
    std::string name;   // 面板里起的名字，空 = 显示成「猫饭 N」
    int hp = 0, stamina = 0, attack = 0, defense = 0, elemRes = 0;
    int skill[3] = {-1, -1, -1};
};

// 全套：装备套装 + 道具套装 + 联动快捷栏 + 猫饭 + 任务（CT 里 allsetitem 的一行）
struct LoadoutSet {
    int id = 0;               // 编号：ini 里 [Set.编号] 的数字，重开套用按它找
    std::string name;         // 面板里显示的名字，允许重名
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
    int activeSet = 0;          // 重开时套用的全套编号（[Set.编号] 的数字），0 = 不套用
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
    bool debugForceQuit = true;   // [Debug] ForceQuit：窗口销毁后若进程还不退，主动结束它（默认开）
    std::string debugDisable;   // [Debug] Disable：排查用，关掉指定的过滤器（pad/cursor/kbd/din/overlay）

    // [Hotkeys]  多个组合用 | 隔开，例如  LB+RB+LT+RT | Ctrl+R
    std::string hkRestart = "LB+RB+LT+RT";
    std::string hkPanel = "Insert";   // 只认键盘：手柄开面板靠不住（要扣留按键）
    std::string hkResetOnly;    // 只重置任务（默认不设键，用面板上的按钮）

    std::vector<LoadoutSet> sets;
    std::map<int, FoodPreset> foods;
    // 猫饭在面板里的显示顺序（只存编号，纯 UI 顺序；猫饭本身的编号永远不变）
    std::vector<int> foodOrder;

    const LoadoutSet* FindSet(const std::string& n) const {
        for (const auto& s : sets) if (s.name == n) return &s;
        return nullptr;
    }
    const LoadoutSet* FindSetById(int id) const {
        for (const auto& s : sets) if (s.id == id) return &s;
        return nullptr;
    }
    int NextSetId() const {
        int id = 1;
        for (const auto& s : sets) if (s.id >= id) id = s.id + 1;
        return id;
    }
    // 换存档时用：只清套装、猫饭和「当前选的是哪个」，面板设置一律不动
    void ClearLoadouts();

    // 按 foodOrder 排好的猫饭编号：foodOrder 里没有的（新加的、手写 ini 没写的）补在后面，
    // 已经删掉的自动去掉。面板里一律按它列。
    std::vector<int> FoodIds() const;
    void NormalizeFoodOrder();
};

// 切存档档案：先把 cur 存回 fromPath，再把 toPath 里那一档的套装/猫饭接过来。
// 面板设置（快捷键、语言、不透明度、HDR…）以 cur 为准，不从 toPath 读。
void SwitchProfile(Config& cur, const std::wstring& fromPath, const std::wstring& toPath);

// 读 ini（UTF-8，可带 BOM）。文件不在就写一份默认的。
bool LoadConfig(const std::wstring& path, Config& cfg, std::string& err);
bool SaveDefaultConfig(const std::wstring& path);
// 面板里改了设置后整份重写（注释是模板里的，手写的注释不会保留）。先写临时文件再替换，
// 写到一半崩了也不会留下半个文件。
bool SaveConfig(const std::wstring& path, const Config& cfg);

// 分两份存：base（面板设置、快捷键、语言、HDR、调试…）和 profile（只放套装、猫饭、
// 当前套用/当前猫饭、猫饭顺序）。读的时候先 base 再 profile，写的时候各自只写自己那段。
bool LoadSplit(const std::wstring& basePath, const std::wstring& profPath, Config& cfg, std::string& err);
bool SaveSplit(const std::wstring& basePath, const std::wstring& profPath, const Config& cfg);
