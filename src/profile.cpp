#include "profile.h"

#include "log.h"

#include <windows.h>

#include <cstdio>
#include <cstring>
#include <string>
#include <vector>

namespace profile {
namespace {

std::wstring g_dir;        // 插件目录，带结尾反斜杠
std::string g_steamId;
bool g_probed = false;
int g_slot = 1;

std::string Narrow(const std::wstring& w)
{
    if (w.empty()) return std::string();
    const int n = ::WideCharToMultiByte(CP_UTF8, 0, w.c_str(), (int)w.size(), nullptr, 0, nullptr, nullptr);
    std::string s((size_t)n, '\0');
    if (n) ::WideCharToMultiByte(CP_UTF8, 0, w.c_str(), (int)w.size(), &s[0], n, nullptr, nullptr);
    return s;
}

bool ReadFileUtf8(const std::wstring& path, std::string& out)
{
    HANDLE h = ::CreateFileW(path.c_str(), GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE, nullptr,
                             OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (h == INVALID_HANDLE_VALUE) return false;
    LARGE_INTEGER size{};
    if (!::GetFileSizeEx(h, &size) || size.QuadPart <= 0 || size.QuadPart > (1 << 20)) {
        ::CloseHandle(h);
        return false;
    }
    out.assign((size_t)size.QuadPart, '\0');
    DWORD got = 0;
    const BOOL ok = ::ReadFile(h, &out[0], (DWORD)out.size(), &got, nullptr);
    ::CloseHandle(h);
    if (!ok) return false;
    out.resize(got);
    return true;
}

bool WriteFileUtf8(const std::wstring& path, const std::string& text)
{
    HANDLE h = ::CreateFileW(path.c_str(), GENERIC_WRITE, 0, nullptr, CREATE_ALWAYS,
                             FILE_ATTRIBUTE_NORMAL, nullptr);
    if (h == INVALID_HANDLE_VALUE) return false;
    DWORD wrote = 0;
    const BOOL ok = ::WriteFile(h, text.data(), (DWORD)text.size(), &wrote, nullptr);
    ::CloseHandle(h);
    return ok && wrote == text.size();
}

// loginusers.vdf 里标了 MostRecent 的（或者第一个）17 位数字账号
std::string SidFromLoginUsers()
{
    wchar_t steamPath[MAX_PATH * 2] = {};
    DWORD size = sizeof(steamPath) - sizeof(wchar_t);
    if (::RegGetValueW(HKEY_CURRENT_USER, L"Software\\Valve\\Steam", L"SteamPath", RRF_RT_REG_SZ, nullptr,
                       steamPath, &size) != ERROR_SUCCESS)
        return std::string();

    std::string text;
    if (!ReadFileUtf8(std::wstring(steamPath) + L"\\config\\loginusers.vdf", text)) return std::string();

    std::string first, cur;
    size_t pos = 0;
    while (pos < text.size()) {
        size_t eol = text.find('\n', pos);
        const std::string line = text.substr(pos, eol == std::string::npos ? std::string::npos : eol - pos);
        pos = eol == std::string::npos ? text.size() : eol + 1;

        std::vector<std::string> tok;
        for (size_t i = 0; i < line.size(); ++i) {
            if (line[i] != '"') continue;
            const size_t end = line.find('"', i + 1);
            if (end == std::string::npos) break;
            tok.push_back(line.substr(i + 1, end - i - 1));
            i = end;
        }
        if (tok.size() == 1 && tok[0].size() == 17) {
            bool digits = true;
            for (char ch : tok[0]) digits = digits && ch >= '0' && ch <= '9';
            if (digits) {
                cur = tok[0];
                if (first.empty()) first = cur;
            }
        } else if (tok.size() == 2 && tok[0] == "MostRecent" && tok[1] == "1") {
            return cur;
        }
    }
    return first;
}

// Steam 安装目录（注册表里那个）
std::wstring SteamPath()
{
    wchar_t buf[MAX_PATH * 2] = {};
    DWORD size = sizeof(buf) - sizeof(wchar_t);
    if (::RegGetValueW(HKEY_CURRENT_USER, L"Software\\Valve\\Steam", L"SteamPath", RRF_RT_REG_SZ, nullptr, buf,
                       &size) != ERROR_SUCCESS)
        return std::wstring();
    return std::wstring(buf);
}

// 从 <Steam>\userdata\<账号 ID>\582010 里挑最近改过的那个：账号 ID 是 32 位，
// SteamID64 = 76561197960265728 + 账号 ID。这样完全不碰 Steamworks 的接口。
std::string SidFromUserData()
{
    const std::wstring steam = SteamPath();
    if (steam.empty()) return std::string();
    const std::wstring base = steam + L"\\userdata\\";

    std::string best;
    unsigned long long bestAt = 0;
    WIN32_FIND_DATAW fd{};
    HANDLE h = ::FindFirstFileW((base + L"*").c_str(), &fd);
    if (h == INVALID_HANDLE_VALUE) return std::string();
    do {
        if (!(fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY)) continue;
        if (fd.cFileName[0] == L'.') continue;
        const std::wstring app = base + fd.cFileName + L"\\582010";
        if (::GetFileAttributesW(app.c_str()) == INVALID_FILE_ATTRIBUTES) continue;
        unsigned long long at = 0;
        WIN32_FILE_ATTRIBUTE_DATA fad{};
        if (::GetFileAttributesExW((app + L"\\remote").c_str(), GetFileExInfoStandard, &fad))
            at = (static_cast<unsigned long long>(fad.ftLastWriteTime.dwHighDateTime) << 32) |
                 fad.ftLastWriteTime.dwLowDateTime;
        if (at >= bestAt) {
            bestAt = at;
            char name[MAX_PATH] = {};
            ::WideCharToMultiByte(CP_ACP, 0, fd.cFileName, -1, name, MAX_PATH, nullptr, nullptr);
            best = name;
        }
    } while (::FindNextFileW(h, &fd));
    ::FindClose(h);
    if (best.empty()) return std::string();
    // 账号 ID 是十进制数字，转成 SteamID64
    unsigned long long acc = 0;
    for (char ch : best) {
        if (ch < '0' || ch > '9') return std::string();
        acc = acc * 10 + (unsigned long long)(ch - '0');
    }
    if (!acc) return std::string();
    return std::to_string(acc + 76561197960265728ULL);
}

std::wstring ProfilesDir()
{
    return g_dir + L"profiles\\";
}

std::wstring SlotFile(int slot)
{
    const std::string sid = SteamId().empty() ? std::string("default") : SteamId();
    return ProfilesDir() + std::wstring(sid.begin(), sid.end()) + L"_" + std::to_wstring(slot) + L".ini";
}

}  // namespace

void Init(const std::wstring& pluginDir)
{
    g_dir = pluginDir;
    if (!g_dir.empty() && g_dir.back() != L'\\') g_dir += L'\\';
    ::CreateDirectoryW(g_dir.c_str(), nullptr);
    ::CreateDirectoryW(ProfilesDir().c_str(), nullptr);

    // 机器级的「当前存档位」
    std::string txt;
    if (ReadFileUtf8(ProfilesDir() + L"profile.txt", txt)) {
        const size_t at = txt.find("Slot=");
        if (at != std::string::npos) {
            const int n = std::atoi(txt.c_str() + at + 5);
            g_slot = (n >= 1 && n <= 3) ? n : 1;
        }
    }
    Log("profile: dir=%s slot=%d", Narrow(g_dir).c_str(), g_slot);
}

const std::string& SteamId()
{
    if (!g_probed) {
        g_probed = true;
        g_steamId = SidFromUserData();
        const bool byFolder = !g_steamId.empty();
        if (g_steamId.empty()) g_steamId = SidFromLoginUsers();
        Log("profile: SteamID=%s（%s）", g_steamId.empty() ? "(unknown)" : g_steamId.c_str(),
            byFolder ? "按 userdata 目录" : "按 loginusers.vdf");
    }
    return g_steamId;
}

std::string AccountLabel()
{
    const std::string& sid = SteamId();
    return sid.empty() ? std::string("?") : sid;
}

int Slot()
{
    if (!g_probed) SteamId();   // 初始化账号（路径里要用）
    return g_slot;
}

void SetSlot(int slot)
{
    if (slot < 1 || slot > 3) slot = 1;
    g_slot = slot;
    std::string txt = "Account=" + AccountLabel() + "\r\nSlot=" + std::to_string(slot) + "\r\n";
    if (!WriteFileUtf8(ProfilesDir() + L"profile.txt", txt)) Log("profile: 写 profile.txt 失败");
    Log("profile: 切到存档 %d（%s）", slot, Narrow(SlotFile(slot)).c_str());
}

std::wstring BasePath()
{
    return g_dir + L"BetterINQ.ini";
}

std::wstring ConfigPath()
{
    (void)Slot();   // 顺带初始化账号，路径里要用
    return SlotFile(g_slot);
}

std::wstring LegacyPath()
{
    return g_dir + L"BetterINQ.ini";
}

bool EnsureMigrated()
{
    const std::wstring target = ConfigPath();
    if (::GetFileAttributesW(target.c_str()) != INVALID_FILE_ATTRIBUTES) return false;

    const std::wstring legacy = LegacyPath();
    if (::GetFileAttributesW(legacy.c_str()) == INVALID_FILE_ATTRIBUTES) return false;

    if (!::CopyFileW(legacy.c_str(), target.c_str(), TRUE)) {
        Log("profile: 复制老配置失败（%lu）", (unsigned long)::GetLastError());
        return false;
    }
    Log("profile: 把老的 BetterINQ.ini 复制成档案 %s（老文件保留）", Narrow(target).c_str());
    return true;
}

std::vector<int> SlotsWithData()
{
    std::vector<int> slots;
    const std::string prefix = (SteamId().empty() ? std::string("default") : SteamId()) + "_";
    WIN32_FIND_DATAW fd{};
    HANDLE h = ::FindFirstFileW((ProfilesDir() + L"*.ini").c_str(), &fd);
    if (h == INVALID_HANDLE_VALUE) return slots;
    do {
        const std::string name = Narrow(fd.cFileName);
        if (name.compare(0, prefix.size(), prefix) != 0) continue;
        if (name.size() < prefix.size() + 5) continue;
        const int n = std::atoi(name.c_str() + prefix.size());
        if (n >= 1 && n <= 3) slots.push_back(n);
    } while (::FindNextFileW(h, &fd));
    ::FindClose(h);
    return slots;
}

}  // namespace profile
