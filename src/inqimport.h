#pragma once
#include <string>
#include <vector>

#include "config.h"

// 从原版 IN-Q（IN-Q.exe）的 config.txt 导入全套和猫饭。
//
// 那个文件长这样：前面几行是设置（语言、Reset、Accept only、快捷键），
// 后面跟一段 JSON 的 allsets 数组。我们只认这些字段，别的（Data num、
// User ID 之类）一律忽略。
namespace inqimport {

struct Entry {
    std::string name;
    std::string equip;
    std::string item;
    int quest = 0;
    int meal = -1;        // 原版猫饭编号：原版是脚本里 fooditem 的下标，含义对不上，只记日志
    int statusUp = -1;    // 原版「Status UP」档位：含义没查清，不导入
    int skill[3] = {-1, -1, -1};
    bool linkRadial = false;
};

struct Parsed {
    std::string language;   // Chinese / English / Japanese，空 = 文件里没写
    bool reset = false, acceptOnly = false;
    bool hasReset = false, hasAcceptOnly = false;
    std::string hkOpen;     // Shortcuts 第一行（键盘开面板）
    std::vector<Entry> sets;
};

// 解析一份 config.txt 的文本（UTF-8，带不带 BOM 都行）。
bool Parse(const std::string& utf8, Parsed& out, std::string& err);

// 顺手找原版 config.txt：hint 可以是原版文件夹或文件本身；传空就在插件目录旁边找。
// 返回空 = 没找到（设置页的文件选择框不靠它，用户自己挑）。
std::wstring Resolve(const std::string& hint, const std::wstring& iniPath);

// 读文件 → 解析 → 合并进 cfg：同名全套覆盖，猫饭按技能组合去重后复用/新建。
// 返回导入的全套数量；失败返回 -1（原因写 err），notes 里写跳过了什么（可以传 nullptr）。
int Import(const std::wstring& path, Config& cfg, std::string& err, std::string* notes = nullptr);

// 路径转 UTF-8（给日志和界面显示用）
std::string Narrow(const std::wstring& wide);

} // namespace inqimport
