#include "config.h"
#include "log.h"

#include <windows.h>
#include <cstdlib>
#include <cstring>

#include "i18n.h"

namespace {

std::string Trim(const std::string& s)
{
    std::size_t b = 0, e = s.size();
    while (b < e && (s[b] == ' ' || s[b] == '\t' || s[b] == '\r')) ++b;
    while (e > b && (s[e - 1] == ' ' || s[e - 1] == '\t' || s[e - 1] == '\r')) --e;
    return s.substr(b, e - b);
}

// 去掉行内注释：「 ;」或「 #」之后的都算注释（值里本身不会出现分号）
std::string StripComment(const std::string& v)
{
    for (std::size_t i = 0; i < v.size(); ++i)
        if ((v[i] == ';' || v[i] == '#') && (i == 0 || v[i - 1] == ' ' || v[i - 1] == '\t'))
            return v.substr(0, i);
    return v;
}

bool ReadAll(const std::wstring& path, std::string& out)
{
    HANDLE h = ::CreateFileW(path.c_str(), GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE, nullptr,
                             OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (h == INVALID_HANDLE_VALUE) return false;
    LARGE_INTEGER sz{};
    ::GetFileSizeEx(h, &sz);
    out.resize((std::size_t)sz.QuadPart);
    DWORD rd = 0;
    const BOOL ok = out.empty() || ::ReadFile(h, &out[0], (DWORD)out.size(), &rd, nullptr);
    ::CloseHandle(h);
    if (!ok) return false;
    out.resize(rd);
    if (out.compare(0, 3, "\xEF\xBB\xBF") == 0) out.erase(0, 3);
    return true;
}

bool Bool(const std::string& v) { return v == "1" || v == "true" || v == "on" || v == "yes"; }

} // namespace

namespace {

// 配置 → ini 文本。默认配置和面板保存共用这一份模板。
std::string Render(const Config& c)
{
    auto kv = [](const char* k, const std::string& v, const char* note) {
        std::string line = std::string(k) + "=" + v;
        if (note && *note) {
            if (line.size() < 21) line.append(21 - line.size(), ' ');
            line += " ; ";
            line += note;
        }
        return line + "\r\n";
    };
    auto num = [](int v) { return std::to_string(v); };
    std::string o = "\xEF\xBB\xBF";
    o += i18n::Tr("; Better-IN-Q 配置。面板里改的设置会自动存回这里；手改的话下次进游戏生效。\r\n\r\n",
                  "; Better-IN-Q settings. Changes made in the panel are written back here;\r\n"
                  "; if you edit by hand they take effect on the next launch.\r\n\r\n",
                  "; Better-IN-Q の設定。パネルで変えた内容はここに保存されます。\r\n"
                  "; 手で編集した場合は次回起動時に反映されます。\r\n\r\n");
    o += "[General]\r\n";
    o += kv("QuestID", num(c.questId), i18n::Tr("重开时接的任务 ID，0 = 用上一次的任务",
                                                "Quest to take when restarting, 0 = last one",
                                                "再開時に受注するクエスト ID、0 = 前回のもの"));
    o += kv("Reset", num(c.reset), i18n::Tr("1 = 重置后回据点再接（慢，完全走正规流程）",
                                            "1 = go back to base and re-accept (slow, fully legit)",
                                            "1 = リセット後に拠点へ戻って受注（遅いが正規の手順）"));
    o += kv("AcceptOnly", num(c.acceptOnly), i18n::Tr("1 = 只接任务，不出发",
                                                      "1 = accept the quest but do not depart",
                                                      "1 = 受注のみ、出発しない"));
    o += kv("WyvernStart", num(c.wyvern), i18n::Tr("1 = 普通地图用翼龙起始",
                                                   "1 = start normal maps with the wingdrake",
                                                   "1 = 通常マップは翼竜で開始"));
    o += kv("ActiveSet", num(c.activeSet), i18n::Tr("重开时套用的全套编号（下面 [Set.编号] 的数字），0 = 不套",
                                                    "Number of the loadout to apply on restart (the n in [Set.n]), 0 = none",
                                                    "再開時に適用するセット番号（下の [Set.番号] の数字）、0 = 適用しない"));
    o += kv("Food", num(c.food), i18n::Tr("每次进任务都吃的猫饭（下面 [Food.编号]），0 = 不管",
                                          "Meal eaten on every quest start (a [Food.n] below), 0 = leave it",
                                          "クエスト開始ごとに食べる猫飯（下の [Food.番号]）、0 = 変更しない"));
    o += kv("QuickBlackout", num(c.quickBlackout), i18n::Tr("缩短转场黑屏",
                                                           "Shorten the transition fade",
                                                           "遷移時の暗転を短縮"));
    o += kv("PanelOpacity", num(c.panelOpacity), i18n::Tr("面板不透明度 30~100",
                                                          "Panel opacity, 30~100",
                                                          "パネルの不透明度 30~100"));
    o += kv("Hdr", c.hdr, i18n::Tr("HDR 面板颜色：auto = 按交换链色域判断，off / scrgb / hdr10 可强制",
                                   "HDR panel colors: auto detects from the swap chain; off / scrgb / hdr10 force",
                                   "HDR パネル色：auto はスワップチェーンから判定、off / scrgb / hdr10 で強制"));
    o += kv("SdrNits", num(c.sdrNits), i18n::Tr("SDR 白点亮度（Windows 显示设置里的 SDR 内容亮度），默认 203",
                                                 "SDR white level in nits (Windows SDR content brightness), default 203",
                                                 "SDR 白の輝度（Windows の SDR コンテンツの明るさ）、既定 203"));
    {
        std::string order;
        for (int id : c.FoodIds()) {
            if (!order.empty()) order += ',';
            order += std::to_string(id);
        }
        o += kv("FoodOrder", order, i18n::Tr("猫饭在面板里的排列顺序（编号用逗号隔开；留空 = 按编号排）",
                                             "Order of the meals in the panel (comma separated ids; empty = by id)",
                                             "パネル内の猫飯の並び順（ID をカンマ区切り；空 = ID 順）"));
    }
    o += kv("Language", c.language, i18n::Tr("界面语言 zh / en / ja，auto = 跟系统界面语言",
                                            "UI language: zh / en / ja, auto = follow the system UI language",
                                            "表示言語 zh / en / ja、auto = OS の表示言語に従う"));
    if (c.debug) o += kv("Debug", num(c.debug), "");
    o += "\r\n[Debug]\r\n";
    o += i18n::Tr("; 排查用：关掉指定的过滤器（pad / cursor / kbd / din / overlay，逗号分隔；all = 前四个）。\r\n"
                  "; 平时留空。关掉后对应那类输入会漏进游戏，但别的功能照常。\r\n",
                  "; Diagnostics: turn off individual filters (pad / cursor / kbd / din / overlay, comma separated;"
                  " all = the first four).\r\n; Leave empty normally. Input of that kind leaks through while off.\r\n",
                  "; 切り分け用：指定したフィルタを切る（pad / cursor / kbd / din / overlay、カンマ区切り；all = 前四つ）。\r\n"
                  "; 普段は空のまま。切った種類の入力はゲームに漏れます。\r\n");
    o += kv("Disable", c.debugDisable, "");
    o += kv("ForceQuit", c.debugForceQuit ? "1" : "0",
            i18n::Tr("窗口销毁后若进程还不退出，4 秒后主动结束（默认 1；排查退出问题时可以设 0）",
                     "End the process 4s after the window is gone if it has not exited (default 1)",
                     "ウィンドウが消えて 4 秒後も終了しない場合にプロセスを終了（既定 1）"));

    o += "\r\n[Hotkeys]\r\n";
    o += i18n::Tr("; 手柄：A B X Y LB RB LT RT LS RS Up Down Left Right View Menu\r\n",
                  "; Gamepad: A B X Y LB RB LT RT LS RS Up Down Left Right View Menu\r\n",
                  "; ゲームパッド：A B X Y LB RB LT RT LS RS Up Down Left Right View Menu\r\n");
    o += i18n::Tr("; 键盘：Ctrl Shift Alt F1~F12 A~Z 0~9 Insert Delete Home End PageUp PageDown\r\n",
                  "; Keyboard: Ctrl Shift Alt F1~F12 A~Z 0~9 Insert Delete Home End PageUp PageDown\r\n",
                  "; キーボード：Ctrl Shift Alt F1~F12 A~Z 0~9 Insert Delete Home End PageUp PageDown\r\n");
    o += i18n::Tr("; 多个组合用 | 隔开\r\n",
                  "; separate combinations with |\r\n",
                  "; 複数の組み合わせは | で区切る\r\n");
    o += kv("Restart", c.hkRestart, i18n::Tr("重开", "Restart", "再開"));
    o += kv("Panel", c.hkPanel, i18n::Tr("打开/关闭面板", "Open/close the panel", "パネルの開閉"));
    o += kv("ResetOnly", c.hkResetOnly, i18n::Tr("只重置任务（不接任务、不出发），留空 = 用面板上的按钮",
                                                  "Reset the quest only (no accept, no depart); empty = use the panel button",
                                                  "クエストのリセットのみ（受注・出発なし）。空 = パネルのボタンを使う"));
    o += i18n::Tr("\r\n; 全套：Equip/Item 写游戏里的套装名字，或游戏列表里看到的编号\r\n",
                  "\r\n; Loadout: Equip/Item takes the in-game loadout name, or the number shown in the game\r\n",
                  "\r\n; セット：Equip/Item にはゲーム内のセット名か、ゲームの一覧に出る番号を書く\r\n");
    if (c.sets.empty())
        o += i18n::Tr(";[Set.1]\r\n;Name=示例\r\n;Equip=1\r\n;Item=1\r\n;LinkRadial=0\r\n;Food=1\r\n;Quest=66845\r\n",
                      ";[Set.1]\r\n;Name=example\r\n;Equip=1\r\n;Item=1\r\n;LinkRadial=0\r\n;Food=1\r\n;Quest=66845\r\n",
                      ";[Set.1]\r\n;Name=例\r\n;Equip=1\r\n;Item=1\r\n;LinkRadial=0\r\n;Food=1\r\n;Quest=66845\r\n");
    int writeId = 0;
    for (const auto& s : c.sets) {
        ++writeId;
        const int id = s.id > 0 ? s.id : writeId;
        o += "[Set." + std::to_string(id) + "]\r\n";
        o += kv("Name", s.name, i18n::Tr("面板里显示的名字，可以重名",
                                          "shown in the panel; duplicates are allowed",
                                          "パネルに表示する名前。重複可"));
        o += kv("Equip", s.equip, "");
        o += kv("Item", s.item, "");
        o += kv("LinkRadial", num(s.linkRadial), i18n::Tr("套道具时联动快捷栏",
                                                          "Link the shortcut bar when swapping items",
                                                          "アイテム切替時にショートカットを連動"));
        o += kv("Food", num(s.food), "");
        o += kv("Quest", num(s.quest), "");
        o += "\r\n";
    }
    o += i18n::Tr("; 猫饭：HP 0~5（+0~+50） Stamina 0~2（+0/+25/+50） Attack/Defense/ElemRes 0~3（无/小/中/大）"
                  " Skills 三个技能编号，-1 = 无\r\n",
                  "; Meal: HP 0~5 (+0~+50), Stamina 0~2 (+0/+25/+50), Attack/Defense/ElemRes 0~3 (none/small/medium/large)"
                  " Skills takes three skill numbers, -1 = none\r\n",
                  "; 猫飯：HP 0~5（+0~+50） Stamina 0~2（+0/+25/+50） Attack/Defense/ElemRes 0~3（なし/小/中/大）"
                  " Skills はスキル番号 3 つ、-1 = なし\r\n");
    if (c.foods.empty())
        o += ";[Food.1]\r\n;Name=日常\r\n;HP=5\r\n;Stamina=2\r\n;Attack=3\r\n;Defense=0\r\n;ElemRes=0\r\n;Skills=9,32,39\r\n";
    for (const auto& kvp : c.foods) {
        const FoodPreset& f = kvp.second;
        o += "[Food." + num(kvp.first) + "]\r\n";
        o += kv("Name", f.name, i18n::Tr("名字，空 = 就叫「猫饭 N」", "Name; empty = \"Meal N\"", "名前。空 = 「猫飯 N」"));
        o += kv("HP", num(f.hp), "");
        o += kv("Stamina", num(f.stamina), "");
        o += kv("Attack", num(f.attack), "");
        o += kv("Defense", num(f.defense), "");
        o += kv("ElemRes", num(f.elemRes), "");
        o += kv("Skills", num(f.skill[0]) + "," + num(f.skill[1]) + "," + num(f.skill[2]), "");
        o += "\r\n";
    }
    return o;
}

bool WriteAll(const std::wstring& path, const std::string& s, DWORD disposition)
{
    HANDLE h = ::CreateFileW(path.c_str(), GENERIC_WRITE, 0, nullptr, disposition, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (h == INVALID_HANDLE_VALUE) return false;
    DWORD wr = 0;
    const BOOL ok = ::WriteFile(h, s.data(), (DWORD)s.size(), &wr, nullptr) && wr == s.size();
    ::CloseHandle(h);
    return ok != FALSE;
}

} // namespace

bool SaveDefaultConfig(const std::wstring& path)
{
    return WriteAll(path, Render(Config()), CREATE_NEW);
}

std::vector<int> Config::FoodIds() const
{
    std::vector<int> out;
    for (int id : foodOrder) {
        if (!foods.count(id)) continue;
        bool dup = false;
        for (int o : out) if (o == id) { dup = true; break; }
        if (!dup) out.push_back(id);
    }
    for (const auto& kv : foods) {
        bool dup = false;
        for (int o : out) if (o == kv.first) { dup = true; break; }
        if (!dup) out.push_back(kv.first);
    }
    return out;
}

void SwitchProfile(Config& cur, const std::wstring& fromPath, const std::wstring& toPath)
{
    if (fromPath != toPath) SaveConfig(fromPath, cur);   // 先把这一档的改动落盘

    Config other;
    std::string err;
    if (!LoadConfig(toPath, other, err)) Log("config: %s", err.c_str());

    cur.ClearLoadouts();
    cur.sets = other.sets;
    cur.foods = other.foods;
    cur.foodOrder = other.FoodIds();
    cur.activeSet = other.activeSet;
    cur.food = other.food;
    SaveConfig(toPath, cur);
}

void Config::ClearLoadouts()
{
    sets.clear();
    foods.clear();
    foodOrder.clear();
    activeSet = 0;
    food = 0;
}

void Config::NormalizeFoodOrder() { foodOrder = FoodIds(); }

bool SaveConfig(const std::wstring& path, const Config& cfg)
{
    const std::wstring tmp = path + L".tmp";
    if (!WriteAll(tmp, Render(cfg), CREATE_ALWAYS)) return false;
    return ::MoveFileExW(tmp.c_str(), path.c_str(), MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH) != FALSE;
}

// 把 Render() 出来的整份文本拆成「base 段」和「profile 段」。
// profile 段 = 所有 [Set.*] / [Food.*] 段，加上 [General] 里的 ActiveSet / Food / FoodOrder。
namespace {
void SplitText(const std::string& full, std::string& baseOut, std::string& profOut)
{
    baseOut.clear();
    profOut.clear();
    std::string sec;
    bool baseGeneral = false;   // base 里已经出现过 [General]
    bool profGeneral = false;   // profile 里已经出现过 [General]
    std::size_t pos = 0;
    while (pos <= full.size()) {
        const std::size_t eol = full.find('\n', pos);
        const std::size_t end = (eol == std::string::npos) ? full.size() : eol;
        std::string line = full.substr(pos, end - pos);
        if (!line.empty() && line.back() == '\r') line.pop_back();
        pos = (eol == std::string::npos) ? full.size() + 1 : eol + 1;
        if (eol == std::string::npos && line.empty()) break;

        const bool isHeader = !line.empty() && line[0] == '[' && line.back() == ']';
        if (isHeader) {
            sec = line.substr(1, line.size() - 2);
            const bool loadoutSec = sec.rfind("Set.", 0) == 0 || sec.rfind("Food.", 0) == 0;
            if (loadoutSec) { profOut += line; profOut += "\r\n"; }
            else {
                baseOut += line; baseOut += "\r\n";
                if (sec == "General") { baseGeneral = true; profGeneral = false; }
            }
            continue;
        }
        const bool loadoutSec = sec.rfind("Set.", 0) == 0 || sec.rfind("Food.", 0) == 0;
        if (loadoutSec) { profOut += line; profOut += "\r\n"; continue; }

        bool toProf = false;
        if (sec == "General") {
            const std::size_t eq = line.find('=');
            if (eq != std::string::npos && !line.empty() && line[0] != ';' && line[0] != '#') {
                const std::string key = Trim(line.substr(0, eq));
                toProf = key == "ActiveSet" || key == "Food" || key == "FoodOrder";
            }
        }
        if (toProf) {
            if (!profGeneral) { profOut += "[General]\r\n"; profGeneral = true; }
            profOut += line; profOut += "\r\n";
        } else {
            if (sec == "General") baseGeneral = true;
            baseOut += line; baseOut += "\r\n";
        }
    }
    if (profOut.empty()) return;
    profOut = "; 这个文件只放「套装」和「猫饭」（每个存档单独一份）。\r\n"
              "; 面板设置、快捷键、语言、HDR 这些在同一个插件目录的 BetterINQ.ini 里，那份是所有存档共用的。\r\n\r\n" + profOut;
}
}  // namespace

bool SaveSplit(const std::wstring& basePath, const std::wstring& profPath, const Config& cfg)
{
    std::string base, prof;
    SplitText(Render(cfg), base, prof);
    if (!WriteAll(basePath, base, CREATE_ALWAYS)) return false;
    if (profPath == basePath) return true;
    return WriteAll(profPath, prof, CREATE_ALWAYS);
}

bool LoadSplit(const std::wstring& basePath, const std::wstring& profPath, Config& cfg, std::string& err)
{
    if (!LoadConfig(basePath, cfg, err)) return false;
    const Config fromBase = cfg;
    if (profPath == basePath) return true;
    if (::GetFileAttributesW(profPath.c_str()) == INVALID_FILE_ATTRIBUTES) {
        // 第一次用：把 base 里已有的套装/猫饭拆到 profile，两份都写一遍
        return SaveSplit(basePath, profPath, cfg);
    }
    if (!LoadConfig(profPath, cfg, err)) { cfg = fromBase; return true; }
    const Config fromProf = cfg;
    // 旧格式（0.7.34 及以前）：profile 里连面板设置一起存着。这时以 profile 为准，
    // 首次存盘会自动把面板那段拆回 base。
    {
        std::string profTxt;
        if (ReadAll(profPath, profTxt) && profTxt.find("[Hotkeys]") != std::string::npos) return true;
    }
    cfg = fromBase;
    cfg.sets = fromProf.sets;
    cfg.foods = fromProf.foods;
    cfg.foodOrder = fromProf.foodOrder;
    cfg.activeSet = fromProf.activeSet;
    cfg.food = fromProf.food;
    return true;
}

bool LoadConfig(const std::wstring& path, Config& cfg, std::string& err)
{
    std::string txt;
    if (!ReadAll(path, txt)) {
        SaveDefaultConfig(path);
        if (!ReadAll(path, txt)) { err = i18n::Tr("读不到也建不了配置文件",
                                                  "cannot read or create the config file",
                                                  "設定ファイルを読めず、作成もできません"); return false; }
    }
    cfg = Config();
    std::string sec;
    std::string activeSetRaw;   // [General] ActiveSet 原样收着，读完再解析（可能是编号，也可能是老格式的名字）
    LoadoutSet* curSet = nullptr;
    FoodPreset* curFood = nullptr;
    std::size_t pos = 0;
    while (pos < txt.size()) {
        std::size_t eol = txt.find('\n', pos);
        if (eol == std::string::npos) eol = txt.size();
        std::string line = Trim(txt.substr(pos, eol - pos));
        pos = eol + 1;
        if (line.empty() || line[0] == ';' || line[0] == '#') continue;
        if (line[0] == '[' && line.back() == ']') {
            sec = Trim(line.substr(1, line.size() - 2));
            curSet = nullptr;
            curFood = nullptr;
            if (sec.compare(0, 4, "Set.") == 0) {
                const std::string key = Trim(sec.substr(4));
                cfg.sets.push_back(LoadoutSet());
                curSet = &cfg.sets.back();
                if (!key.empty() && key.find_first_not_of("0123456789") == std::string::npos) {
                    curSet->id = std::atoi(key.c_str());      // 新格式：[Set.1]
                } else {
                    curSet->id = cfg.NextSetId();             // 老格式：[Set.名字]，编号自己发一个
                    curSet->name = key;
                }
            } else if (sec.compare(0, 5, "Food.") == 0) {
                curFood = &cfg.foods[std::atoi(sec.c_str() + 5)];
            }
            continue;
        }
        const std::size_t eq = line.find('=');
        if (eq == std::string::npos) continue;
        const std::string k = Trim(line.substr(0, eq));
        const std::string v = Trim(StripComment(line.substr(eq + 1)));
        const int n = std::atoi(v.c_str());
        if (sec == "General") {
            if (k == "QuestID") cfg.questId = n;
            else if (k == "Reset") cfg.reset = Bool(v);
            else if (k == "AcceptOnly") cfg.acceptOnly = Bool(v);
            else if (k == "WyvernStart") cfg.wyvern = Bool(v);
            else if (k == "ActiveSet") activeSetRaw = v;
            else if (k == "Food") cfg.food = n;
        else if (k == "FoodOrder") {
            cfg.foodOrder.clear();
            std::string rest = v;
            while (!rest.empty()) {
                const std::size_t p = rest.find(',');
                const std::string one = rest.substr(0, p);
                if (!one.empty()) cfg.foodOrder.push_back(std::atoi(one.c_str()));
                if (p == std::string::npos) break;
                rest = rest.substr(p + 1);
            }
        }
            else if (k == "QuickBlackout") cfg.quickBlackout = Bool(v);
            else if (k == "PanelOpacity") cfg.panelOpacity = n < 30 ? 30 : (n > 100 ? 100 : n);
            else if (k == "Language") cfg.language = v;
            else if (k == "Hdr") cfg.hdr = v;
            else if (k == "SdrNits") cfg.sdrNits = n < 80 ? 80 : (n > 10000 ? 10000 : n);
            else if (k == "Debug") cfg.debug = n;
        } else if (sec == "Debug") {
            if (k == "Disable") cfg.debugDisable = v;
            else if (k == "ForceQuit") cfg.debugForceQuit = Bool(v);
            else Log("ini: [Debug] 里不认识的键 %s（拼写对吗？）", k.c_str());
        } else if (sec == "Hotkeys") {
            if (k == "Restart") cfg.hkRestart = v;
            else if (k == "Panel") cfg.hkPanel = v;
            else if (k == "ResetOnly") cfg.hkResetOnly = v;
        } else if (curSet) {
            if (k == "Name") curSet->name = v;
            else if (k == "Equip") curSet->equip = v;
            else if (k == "Item") curSet->item = v;
            else if (k == "LinkRadial") curSet->linkRadial = Bool(v);
            else if (k == "Food") curSet->food = n;
            else if (k == "Quest") curSet->quest = n;
        } else if (curFood) {
            if (k == "Name") curFood->name = v;
            else if (k == "HP") curFood->hp = n;
            else if (k == "Stamina") curFood->stamina = n;
            else if (k == "Attack") curFood->attack = n;
            else if (k == "Defense") curFood->defense = n;
            else if (k == "ElemRes") curFood->elemRes = n;
            else if (k == "Skills") {
                int i = 0;
                for (std::size_t p = 0; p <= v.size() && i < 3;) {
                    std::size_t c = v.find(',', p);
                    if (c == std::string::npos) c = v.size();
                    const std::string t = Trim(v.substr(p, c - p));
                    if (!t.empty()) curFood->skill[i++] = std::atoi(t.c_str());
                    p = c + 1;
                }
            }
        }
    }
    // ActiveSet：新格式是编号，老格式是套装名字
    if (!activeSetRaw.empty()) {
        if (activeSetRaw.find_first_not_of("0123456789") == std::string::npos) {
            cfg.activeSet = std::atoi(activeSetRaw.c_str());
        } else if (const LoadoutSet* s = cfg.FindSet(activeSetRaw)) {
            cfg.activeSet = s->id;
        }
    }
    // 编号必须唯一：没写、写 0、或者两份都写了 [Set.1] 的，重新发一个没用过的号
    {
        std::vector<int> used;
        int next = 1;
        for (const auto& s : cfg.sets) if (s.id >= next) next = s.id + 1;
        for (auto& s : cfg.sets) {
            bool clash = s.id <= 0;
            for (int u : used) if (u == s.id) clash = true;
            if (clash) {
                for (;;) {
                    bool usedAlready = false;
                    for (int u : used) if (u == next) { usedAlready = true; ++next; break; }
                    if (!usedAlready) break;
                }
                s.id = next++;
            }
            used.push_back(s.id);
        }
    }
    cfg.NormalizeFoodOrder();
    return true;
}
