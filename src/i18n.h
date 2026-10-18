#pragma once
// 面板与游戏内提示的多语言：三语并排写在调用处，中文永远放第一个参数。
// 中文是默认语言，也是读这份代码的基准，所以翻译丢了也不会空着。
//
// 只翻「玩家看得见」的东西（面板文字、聊天栏提示、ini 里的说明）。
// Log() 写进日志文件的诊断信息保持原样，那是给作者看的。
#include <string>

namespace i18n {

enum Lang { kZh = 0, kEn = 1, kJa = 2, kCount = 3 };

Lang Current();                          // 当前生效的语言（不会是「跟系统」这个中间态）
void Set(Lang l);
Lang SystemLang();                       // 按 Windows 界面语言猜：中文→中文，日文→日文，其它→英文
Lang FromString(const std::string& s);   // "zh" / "en" / "ja" / "auto"（空也算 auto）
const char* ToString(Lang l);            // "zh" / "en" / "ja"

// 三语选一。en / ja 传 nullptr 就当英文缺失，退回中文。
const char* Tr(const char* zh, const char* en, const char* ja);

// 只认 %s 和 %d，按出现顺序替换。够用了，也免得把 varargs 传进 snprintf 时重入。
std::string Fmt(const char* f);
std::string Fmt(const char* f, const char* a);
std::string Fmt(const char* f, const char* a, const char* b);
std::string Fmt(const char* f, int a);
std::string Fmt(const char* f, int a, const char* b);
std::string Fmt(const char* f, int a, int b);
std::string Fmt(const char* f, int a, int b, int c);

} // namespace i18n
