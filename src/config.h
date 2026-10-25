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
    // 删掉的猫饭留个墓碑：编号永不回收，以后新建的猫饭不会顶掉引用它的套装
    bool deleted = false;
};

// 全套：装备套装 + 道具套装 + 联动快捷栏 + 猫饭 + 任务（CT 里 allsetitem 的一行）
struct LoadoutSet {
    int id = 0;               // 编号：ini 里 [Set.编号] 的数字，重开套用按它找，永不复用
    std::string name;         // 面板里显示的名字，允许重名
    std::string equip;        // 装备套装编号（游戏里第几套，1..224），空 = 不套
    std::string item;         // 道具套装存放格 "#编号"，空 = 不套
    bool linkRadial = false;  // 套道具时联动快捷栏
    int food = -1;            // 猫饭编号，-1 = 跟全局那份走
    int quest = 0;            // 任务 ID，0 = 不改
};

struct Config {
    // [General]
    int questId = 0;            // 重开时接的任务，0 = 上一次的
    bool reset = false;         // 重置后从据点重接（慢，但完全走正规流程）
    bool acceptOnly = false;    // 只接任务不出发
    bool wyvern = false;        // 普通地图用翼龙起始
    bool enabled = true;        // [General] Enabled：1 = 装载态（功能生效）0 = 卸载态（功能全停，只保留面板）
    int activeSet = -1;         // 重开时套用的全套编号（[Set.编号] 的数字），-1 = 不套用
    int food = -1;              // 每次进任务都吃的猫饭，-1 = 不管
    bool quickBlackout = true;  // 缩短转场黑屏
    int panelOpacity = 75;      // 面板不透明度 %，越小越透
    // HDR：auto = 看交换链色域自己判断；off / scrgb / hdr10 可强制。SdrNits = 播放器里
    // 的「SDR 内容亮度」，HDR 下 UI 的白点按它对齐（一般 200~300）
    std::string hdr = "auto";
    int sdrNits = 203;      // SDR 白点亮度（nits）

    // 面板
    std::string language = "zh";  // 界面语言 zh / en / ja，auto = 跟系统界面语言
    int debug = 0;
    bool debugForceQuit = true;   // [Debug] ForceQuit：窗口销毁后若进程还不退，主动结束它（默认开）
    bool debugMealProbe = false;  // [Debug] MealProbe：只记录猫饭那片内存的变化、不写（排查“体力和耐力怎么加”用）
  bool debugNoWndHook = false;  // [Debug] NoWndHook：不去接管游戏窗口过程（排查用；接管后仍然收不到输入才需要它）
    std::string debugDisable;   // [Debug] Disable：排查用，关掉指定的过滤器（pad/cursor/kbd/din/overlay）

    // [Hotkeys] 手柄和键盘组合均可，多个组合用 | 隔开；L3 / R3 = 按下左右摇杆
    std::string hkRestart = "LT+RT+L3+R3 | Delete";
    std::string hkPanel = "LB+LT+X | Insert";  // 手柄组合或键盘 Insert 打开/关闭面板
    std::string hkResetOnly = "LB+RB+L3+R3 | Home";  // 只重置任务，不接任务、不出发

    std::vector<LoadoutSet> sets;
    std::map<int, FoodPreset> foods;
    // 猫饭在面板里的显示顺序（只存编号，纯 UI 顺序；猫饭本身的编号永远不变）
    std::vector<int> foodOrder;
    // 发号器：只增不减，删掉的编号留墓碑、以后不再分配
    int nextSetId = 1;
    int nextFoodId = 1;

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
    // 取一个新编号（引用这套的地方都只认编号，所以编号绝不能被后来的东西顶掉）
    int AllocSetId();
    int AllocFoodId();
    // 换存档时用：只清套装、猫饭和「当前选的是哪个」，面板设置一律不动
    void ClearLoadouts();
    // 每次进游戏都从「什么都没选」开始：清掉当前套用的套装/猫饭/任务预设（选项和列表不动）。
    // 返回 true = 有改动，调用方负责存盘。
    bool ResetSessionSelection();

    // 按 foodOrder 排好的猫饭编号：foodOrder 里没有的（新加的、手写 ini 没写的）补在后面，
    // 删掉的（墓碑）自动去掉。面板里一律按它列。
    std::vector<int> FoodIds() const;
    void NormalizeFoodOrder();
};

// 存档里的套装名单（装备/道具各一份），只有编号和名字，用来把老配置里按名字存的引用换成编号。
// number = 游戏里看到的编号（装备=套装号，道具=显示编号）；store = 道具的存放格（装备与 number 相同）。
struct RefName {
    int number = 0;
    std::string name;
    int store = 0;
};

// 把老配置里「按名字存的装备/道具套装」换成编号（一次性迁移用，不是读取兼容）。
// 名单为空表示「还没读到存档」，那一类原样跳过 —— 绝不在没有名单时把值清掉。
// 名字在名单里找不到就留空；已是编号的只做范围校验。返回值 = 改了几处，lost = 对不上留空的处数。
int MigrateLoadoutRefs(Config& cfg, const std::vector<RefName>& equips,
                       const std::vector<RefName>& items, int* lost = nullptr);
bool NeedsLoadoutMigration(const Config& cfg);

// 只读匹配工具：这 8 个数值（pfood+19A8 起的 hp/耐力/攻/防/属性耐 + 3 个技能）
// 对得上哪一份猫饭，对不上返回 0；不用于取消预设或清除游戏里的猫饭。
int MatchMealValues(const Config& cfg, const int v[8]);

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
