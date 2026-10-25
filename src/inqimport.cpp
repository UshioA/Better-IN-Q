#include "inqimport.h"

#include <windows.h>

#include <cctype>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <map>

namespace inqimport {
namespace {

std::string Trim(const std::string& s)
{
    std::size_t a = 0, b = s.size();
    while (a < b && (s[a] == ' ' || s[a] == '\t' || s[a] == '\r' || s[a] == '\n')) ++a;
    while (b > a && (s[b - 1] == ' ' || s[b - 1] == '\t' || s[b - 1] == '\r' || s[b - 1] == '\n')) --b;
    return s.substr(a, b - a);
}

std::string Lower(std::string s)
{
    for (char& c : s) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    return s;
}

// ---------------------------------------------------------------------------
// 一段够用就好的 JSON：config.txt 里只有对象、数组、字符串、数字和 true/false/null。

struct Reader {
    const std::string& s;
    std::size_t i = 0;
    explicit Reader(const std::string& text, std::size_t at) : s(text), i(at) {}

    void Ws()
    {
        while (i < s.size() && (s[i] == ' ' || s[i] == '\t' || s[i] == '\r' || s[i] == '\n')) ++i;
    }

    bool Take(char c)
    {
        Ws();
        if (i < s.size() && s[i] == c) {
            ++i;
            return true;
        }
        return false;
    }

    bool Look(char c)
    {
        Ws();
        return i < s.size() && s[i] == c;
    }

    static void AppendUtf8(std::string& out, unsigned cp)
    {
        if (cp < 0x80) {
            out += static_cast<char>(cp);
        } else if (cp < 0x800) {
            out += static_cast<char>(0xC0 | (cp >> 6));
            out += static_cast<char>(0x80 | (cp & 0x3F));
        } else if (cp < 0x10000) {
            out += static_cast<char>(0xE0 | (cp >> 12));
            out += static_cast<char>(0x80 | ((cp >> 6) & 0x3F));
            out += static_cast<char>(0x80 | (cp & 0x3F));
        } else {
            out += static_cast<char>(0xF0 | (cp >> 18));
            out += static_cast<char>(0x80 | ((cp >> 12) & 0x3F));
            out += static_cast<char>(0x80 | ((cp >> 6) & 0x3F));
            out += static_cast<char>(0x80 | (cp & 0x3F));
        }
    }

    bool Hex4(unsigned& v)
    {
        if (i + 4 > s.size()) return false;
        v = 0;
        for (int k = 0; k < 4; ++k) {
            const char c = s[i++];
            v <<= 4;
            if (c >= '0' && c <= '9') v |= static_cast<unsigned>(c - '0');
            else if (c >= 'a' && c <= 'f') v |= static_cast<unsigned>(c - 'a' + 10);
            else if (c >= 'A' && c <= 'F') v |= static_cast<unsigned>(c - 'A' + 10);
            else return false;
        }
        return true;
    }

    bool Str(std::string& out)
    {
        out.clear();
        if (!Take('"')) return false;
        while (i < s.size()) {
            const char c = s[i++];
            if (c == '"') return true;
            if (c != '\\') {
                out += c;
                continue;
            }
            if (i >= s.size()) return false;
            const char e = s[i++];
            switch (e) {
            case 'n': out += '\n'; break;
            case 't': out += '\t'; break;
            case 'r': out += '\r'; break;
            case 'b': out += '\b'; break;
            case 'f': out += '\f'; break;
            case '"': out += '"'; break;
            case '\\': out += '\\'; break;
            case '/': out += '/'; break;
            case 'u': {
                unsigned cp = 0;
                if (!Hex4(cp)) return false;
                if (cp >= 0xD800 && cp <= 0xDBFF && i + 1 < s.size() && s[i] == '\\' && s[i + 1] == 'u') {
                    i += 2;
                    unsigned lo = 0;
                    if (!Hex4(lo)) return false;
                    if (lo >= 0xDC00 && lo <= 0xDFFF)
                        cp = 0x10000 + ((cp - 0xD800) << 10) + (lo - 0xDC00);
                }
                AppendUtf8(out, cp);
                break;
            }
            default: out += e; break;
            }
        }
        return false;
    }

    bool Number(long& v)
    {
        Ws();
        std::size_t a = i;
        if (i < s.size() && (s[i] == '-' || s[i] == '+')) ++i;
        while (i < s.size() && std::isdigit(static_cast<unsigned char>(s[i]))) ++i;
        if (i == a) return false;
        v = std::strtol(s.substr(a, i - a).c_str(), nullptr, 10);
        return true;
    }

    // 跳过任何值的语法（不认的键用）
    bool Skip()
    {
        Ws();
        if (i >= s.size()) return false;
        if (s[i] == '"') {
            std::string tmp;
            return Str(tmp);
        }
        if (s[i] == '{' || s[i] == '[') {
            const char close = s[i] == '{' ? '}' : ']';
            ++i;
            while (true) {
                Ws();
                if (i >= s.size()) return false;
                if (s[i] == close) {
                    ++i;
                    return true;
                }
                if (s[i] == ',' || s[i] == ':') {
                    ++i;
                    continue;
                }
                if (!Skip()) return false;
            }
        }
        std::size_t a = i;
        while (i < s.size() && s[i] != ',' && s[i] != '}' && s[i] != ']') ++i;
        return i > a;
    }
};

// 原版全套里「Skill 1~3」是猫饭技能编号（-1 = 无），换个顺序也算同一份猫饭吗？
// 算——游戏里三格技能不区分顺序，去了重用户能少几份重复的猫饭。
bool SameSkills(const int a[3], const int b[3])
{
    int x[3] = {a[0], a[1], a[2]}, y[3] = {b[0], b[1], b[2]};
    auto sort3 = [](int* v) {
        for (int i = 0; i < 3; ++i)
            for (int j = i + 1; j < 3; ++j)
                if (v[j] < v[i]) {
                    const int t = v[i];
                    v[i] = v[j];
                    v[j] = t;
                }
    };
    sort3(x);
    sort3(y);
    return x[0] == y[0] && x[1] == y[1] && x[2] == y[2];
}

bool IsKeyboardSpec(const std::string& spec)
{
    static const char* const kPad[] = {"A",   "B",    "X",    "Y",    "LB",  "RB",  "LT",     "RT",      "LS",
                                       "RS",  "Up",   "Down", "Left", "Right", "View", "Menu"};
    static const char* const kKey[] = {"Ctrl",   "Shift", "Alt",   "Insert", "Delete", "Home", "End",
                                       "PageUp", "PageDown", "Space", "Tab", "Enter", "Backspace"};
    std::string cur;
    std::vector<std::string> tokens;
    for (char c : spec) {
        if (c == '+' || c == '|' || c == ' ') {
            if (!cur.empty()) tokens.push_back(cur);
            cur.clear();
        } else {
            cur += c;
        }
    }
    if (!cur.empty()) tokens.push_back(cur);
    if (tokens.empty()) return false;
    for (const std::string& t : tokens) {
        bool ok = false;
        for (const char* p : kPad)
            if (t == p) return false;   // 手柄键：面板现在只认键盘，这种组合不要
        for (const char* k : kKey)
            if (t == k) {
                ok = true;
                break;
            }
        if (!ok && t.size() == 1 && std::isalnum(static_cast<unsigned char>(t[0]))) ok = true;
        if (!ok && t.size() >= 2 && (t[0] == 'F' || t[0] == 'f')) {
            const int n = std::atoi(t.c_str() + 1);
            if (n >= 1 && n <= 12 && t.size() <= 3) ok = true;
        }
        if (!ok) return false;
    }
    return true;
}

bool FileExists(const std::wstring& path)
{
    const DWORD a = ::GetFileAttributesW(path.c_str());
    return a != INVALID_FILE_ATTRIBUTES && !(a & FILE_ATTRIBUTE_DIRECTORY);
}

std::wstring ParentDir(const std::wstring& path)
{
    const std::size_t p = path.find_last_of(L"\\/");
    return p == std::wstring::npos ? std::wstring() : path.substr(0, p);
}

std::wstring Widen(const std::string& utf8)
{
    if (utf8.empty()) return std::wstring();
    const int n = ::MultiByteToWideChar(CP_UTF8, 0, utf8.c_str(), static_cast<int>(utf8.size()), nullptr, 0);
    std::wstring out(static_cast<std::size_t>(n), L'\0');
    ::MultiByteToWideChar(CP_UTF8, 0, utf8.c_str(), static_cast<int>(utf8.size()), out.data(), n);
    return out;
}

} // namespace

std::string Narrow(const std::wstring& wide)
{
    if (wide.empty()) return std::string();
    const int n = ::WideCharToMultiByte(CP_UTF8, 0, wide.c_str(), static_cast<int>(wide.size()), nullptr, 0,
                                        nullptr, nullptr);
    std::string out(static_cast<std::size_t>(n), '\0');
    ::WideCharToMultiByte(CP_UTF8, 0, wide.c_str(), static_cast<int>(wide.size()), out.data(), n, nullptr, nullptr);
    return out;
}

bool Parse(const std::string& utf8, Parsed& out, std::string& err)
{
    out = Parsed();
    std::string text = utf8;
    if (text.size() >= 3 && static_cast<unsigned char>(text[0]) == 0xEF &&
        static_cast<unsigned char>(text[1]) == 0xBB && static_cast<unsigned char>(text[2]) == 0xBF)
        text.erase(0, 3);

    // 前半段是一行一项的设置，找到 JSON 的开头就收工
    std::vector<std::string> shortcuts;
    bool inShortcuts = false;
    std::size_t jsonAt = std::string::npos;
    std::size_t lineAt = 0;
    while (lineAt <= text.size()) {
        const std::size_t nl = text.find('\n', lineAt);
        const std::string line = text.substr(lineAt, nl == std::string::npos ? std::string::npos : nl - lineAt);
        const std::string t = Trim(line);
        if (!t.empty() && t[0] == '{') {
            jsonAt = lineAt + line.find_first_of('{');
            break;
        }
        if (!t.empty()) {
            const std::size_t colon = t.find(':');
            const std::string key = colon == std::string::npos ? t : Trim(t.substr(0, colon));
            const std::string val = colon == std::string::npos ? std::string() : Trim(t.substr(colon + 1));
            if (inShortcuts && colon == std::string::npos) {
                shortcuts.push_back(t);
            } else if (key == "Languege" || key == "Language") {
                out.language = val;
            } else if (key == "Reset") {
                out.reset = val != "0";
                out.hasReset = true;
            } else if (key == "Accept only" || key == "Acceptonly") {
                out.acceptOnly = val != "0";
                out.hasAcceptOnly = true;
            } else if (key == "Shortcuts") {
                inShortcuts = true;
            }
        }
        if (nl == std::string::npos) break;
        lineAt = nl + 1;
    }
    if (!shortcuts.empty()) out.hkOpen = shortcuts[0];
    if (jsonAt == std::string::npos) {
        err = "config.txt 里没找到 allsets 那段 JSON";
        return false;
    }

    Reader r(text, jsonAt);
    if (!r.Take('{')) {
        err = "allsets 前面的 JSON 开头不对";
        return false;
    }
    while (true) {
        r.Ws();
        if (r.Look('}')) {
            r.Take('}');
            break;
        }
        std::string key;
        if (!r.Str(key)) {
            err = "JSON 里有个键读不出来";
            return false;
        }
        if (!r.Take(':')) {
            err = "JSON 里键后面少了冒号";
            return false;
        }
        if (key == "allsets" && r.Look('[')) {
            r.Take('[');
            while (true) {
                r.Ws();
                if (r.Look(']')) {
                    r.Take(']');
                    break;
                }
                if (!r.Look('{')) {
                    err = "allsets 里有一项不是对象";
                    return false;
                }
                r.Take('{');
                Entry e;
                while (true) {
                    r.Ws();
                    if (r.Look('}')) {
                        r.Take('}');
                        break;
                    }
                    std::string k;
                    if (!r.Str(k)) {
                        err = "全套里有个字段名读不出来";
                        return false;
                    }
                    if (!r.Take(':')) {
                        err = "全套字段名后面少了冒号";
                        return false;
                    }
                    std::string sv;
                    long nv = 0;
                    bool got = r.Look('"') ? r.Str(sv) : r.Number(nv);
                    if (!got) {
                        // true / false / null 之类：跳过，接着读下一个字段
                        if (!r.Skip()) {
                            err = "全套字段的值读不出来";
                            return false;
                        }
                    }
                    if (!got) {
                        // 什么也没记，直接走到字段之间的分隔符
                    } else if (k == "Set name") e.name = sv;
                    else if (k == "Equip loadout") e.equip = sv;
                    else if (k == "Item loadout") e.item = sv;
                    else if (k == "Item shortcuts") e.linkRadial = nv != 0;
                    else if (k == "Quest ID") e.quest = static_cast<int>(nv);
                    else if (k == "Meal") e.meal = static_cast<int>(nv);
                    else if (k == "Status UP") e.statusUp = static_cast<int>(nv);
                    else if (k == "Skill 1") e.skill[0] = static_cast<int>(nv);
                    else if (k == "Skill 2") e.skill[1] = static_cast<int>(nv);
                    else if (k == "Skill 3") e.skill[2] = static_cast<int>(nv);
                    r.Ws();
                    if (r.Look(',')) {
                        r.Take(',');
                        continue;
                    }
                    if (r.Look('}')) {
                        r.Take('}');
                        break;
                    }
                    err = "全套里字段之间少了逗号";
                    return false;
                }
                out.sets.push_back(e);
                r.Ws();
                if (r.Look(',')) {
                    r.Take(',');
                    continue;
                }
                if (r.Look(']')) {
                    r.Take(']');
                    break;
                }
                err = "allsets 里两项之间少了逗号";
                return false;
            }
        } else if (!r.Skip()) {
            err = "JSON 里有个值读不出来";
            return false;
        }
        r.Ws();
        if (r.Look(',')) {
            r.Take(',');
            continue;
        }
        if (r.Look('}')) {
            r.Take('}');
            break;
        }
        err = "JSON 顶层字段之间少了逗号";
        return false;
    }
    if (out.sets.empty()) {
        err = "config.txt 里的 allsets 是空的";
        return false;
    }
    return true;
}

std::wstring Resolve(const std::string& hint, const std::wstring& iniPath)
{
    if (!hint.empty()) {
        std::wstring p = Widen(hint);
        const DWORD a = ::GetFileAttributesW(p.c_str());
        if (a != INVALID_FILE_ATTRIBUTES) {
            if (a & FILE_ATTRIBUTE_DIRECTORY) {
                if (!p.empty() && p.back() != L'\\' && p.back() != L'/') p += L'\\';
                p += L"config.txt";
            }
            if (FileExists(p)) return p;
        }
        return std::wstring();
    }
    const std::wstring dir = ParentDir(iniPath);
    const std::wstring candidates[] = {
        dir + L"\\config.txt",
        dir + L"\\IN-Q\\config.txt",
        ParentDir(dir) + L"\\config.txt",
        ParentDir(dir) + L"\\IN-Q\\config.txt",
    };
    for (const std::wstring& c : candidates)
        if (FileExists(c)) return c;
    return std::wstring();
}

int Import(const std::wstring& path, Config& cfg, std::string& err, std::string* notes)
{
    HANDLE f = ::CreateFileW(path.c_str(), GENERIC_READ, FILE_SHARE_READ, nullptr, OPEN_EXISTING,
                             FILE_ATTRIBUTE_NORMAL, nullptr);
    if (f == INVALID_HANDLE_VALUE) {
        err = "打不开 " + Narrow(path);
        return -1;
    }
    LARGE_INTEGER size;
    std::string text;
    if (::GetFileSizeEx(f, &size) && size.QuadPart > 0 && size.QuadPart < (16 << 20)) {
        text.resize(static_cast<std::size_t>(size.QuadPart));
        DWORD got = 0;
        if (!::ReadFile(f, text.data(), static_cast<DWORD>(text.size()), &got, nullptr)) got = 0;
        text.resize(got);
    }
    ::CloseHandle(f);
    if (text.empty()) {
        err = "config.txt 是空的";
        return -1;
    }

    Parsed p;
    if (!Parse(text, p, err)) return -1;

    // 导入就是覆盖：原版的全套和猫饭直接顶掉现在的，不做合并
    cfg.ClearLoadouts(); // IDs are reissued: never carry selections onto unrelated imports.

    std::string oddStatus, oddMeal;
    int withMeal = 0, noMeal = 0;
    if (!p.language.empty()) {
        const std::string l = Lower(p.language);
        if (l == "chinese" || l == "zh" || l == "chs" || l == "cn") cfg.language = "zh";
        else if (l == "english" || l == "en") cfg.language = "en";
        else if (l == "japanese" || l == "ja" || l == "jp") cfg.language = "ja";
    }
    if (p.hasReset) cfg.reset = p.reset;
    if (p.hasAcceptOnly) cfg.acceptOnly = p.acceptOnly;
    // 面板的打开快捷键不导：原版根本没有面板，这一项是插件自己加的
    (void)p.hkOpen;

    int imported = 0;
    int index = 0;
    for (const Entry& e : p.sets) {
        ++index;
        LoadoutSet s;
        s.id = cfg.AllocSetId();   // 发号只增不减：导入不会顶掉别处引用的旧编号
        s.name = Trim(e.name);
        if (s.name.empty()) s.name = "Set " + std::to_string(index);
        s.equip = e.equip;
        s.item = e.item;
        s.linkRadial = e.linkRadial;
        s.quest = e.quest;

        // 原版只有两档猫饭，对应 IN-Q.exe 0x14001061E 那段：`Meal == 0` 才写猫饭，
        // 写的是 HP=5、Stamina=2（UI 上的「体力+50,耐力+50」），再按 Status UP 的字节偏移
        // （4=攻击 / 8=防御 / 12=属性耐性）写 3——UI 上那三项只有「(大)」，没有低/中档；
        // `Meal == 1` 是「猫饭无效」，整块跳过（连技能也不写，游戏里保留原来的状态）。
        int sk[3] = {e.skill[0], e.skill[1], e.skill[2]};
        if (e.meal == 0) {
            FoodPreset f;
            f.hp = 5;
            f.stamina = 2;
            switch (e.statusUp) {
            case 0: break;
            case 4: f.attack = 3; break;      // 攻击 Up (大)
            case 8: f.defense = 3; break;     // 防御 Up (大)
            case 12: f.elemRes = 3; break;    // 属性耐性 Up (大)
            default:
                if (oddStatus.empty()) oddStatus = std::to_string(e.statusUp);
                break;
            }
            for (int i = 0; i < 3; ++i) f.skill[i] = sk[i];

            int key = 0;
            for (const auto& kv : cfg.foods) {
                const FoodPreset& o = kv.second;
                if (o.hp == f.hp && o.stamina == f.stamina && o.attack == f.attack && o.defense == f.defense &&
                    o.elemRes == f.elemRes && SameSkills(o.skill, f.skill)) {
                    key = kv.first;
                    break;
                }
            }
            if (!key) {
                key = cfg.AllocFoodId();
                cfg.foods[key] = f;
            }
            s.food = key;
            ++withMeal;
        } else {
            if (e.meal != 1 && oddMeal.empty()) oddMeal = std::to_string(e.meal);
            ++noMeal;
        }

        cfg.sets.push_back(s);
        ++imported;
    }

    if (notes) {
        std::string out;
        if (withMeal) out += "猫饭体力+50,耐力+50 ×" + std::to_string(withMeal);
        if (noMeal) {
            if (!out.empty()) out += "；";
            out += "猫饭无效 ×" + std::to_string(noMeal);
        }
        if (!oddStatus.empty())
            out += "（Status UP 有认不出的值 " + oddStatus +
                   "，只认 0 无 / 4 攻 / 8 防 / 12 属耐，那几套的能力档位没导）";
        if (!oddMeal.empty())
            out += "（Meal 有认不出的值 " + oddMeal + "，当猫饭无效处理）";
        *notes = out;
    }
    return imported;
}

} // namespace inqimport
