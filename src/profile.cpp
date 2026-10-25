#include "profile.h"

#include "log.h"

#include <windows.h>

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <cwchar>
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

// 只读 Steam 的配置；解析和身份选择与 I/O 分离。
std::string ReadLoginUsers()
{
    wchar_t steamPath[MAX_PATH * 2] = {};
    DWORD size = sizeof(steamPath) - sizeof(wchar_t);
    if (::RegGetValueW(HKEY_CURRENT_USER, L"Software\\Valve\\Steam", L"SteamPath", RRF_RT_REG_SZ, nullptr,
                       steamPath, &size) != ERROR_SUCCESS)
        return std::string();

    std::string text;
    if (!ReadFileUtf8(std::wstring(steamPath) + L"\\config\\loginusers.vdf", text)) return std::string();

    return text;
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

// ActiveUser is useful only while its advertised Steam process is alive and is steam.exe.
// This rejects common stale registry values; it is not a Steamworks authentication check.
unsigned long ActiveAccount()
{
    DWORD account = 0, pid = 0, size = sizeof(DWORD);
    const wchar_t* key = L"Software\\Valve\\Steam\\ActiveProcess";
    if (::RegGetValueW(HKEY_CURRENT_USER, key, L"ActiveUser", RRF_RT_REG_DWORD, nullptr,
                       &account, &size) != ERROR_SUCCESS || !account) return 0;
    size = sizeof(DWORD);
    if (::RegGetValueW(HKEY_CURRENT_USER, key, L"pid", RRF_RT_REG_DWORD, nullptr,
                       &pid, &size) != ERROR_SUCCESS || !pid) return 0;
    HANDLE process = ::OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION, FALSE, pid);
    if (!process) return 0;
    wchar_t path[32768] = {};
    DWORD length = sizeof(path) / sizeof(path[0]);
    DWORD exitCode = 0;
    const bool valid = ::GetExitCodeProcess(process, &exitCode) && exitCode == STILL_ACTIVE &&
                       ::QueryFullProcessImageNameW(process, 0, path, &length);
    ::CloseHandle(process);
    const wchar_t* name = wcsrchr(path, L'\\');
    return valid && _wcsicmp(name ? name + 1 : path, L"steam.exe") == 0 ? account : 0;
}

// Only enumerate candidates with the game's directory. Timestamps are not identity evidence.
std::vector<std::string> UserDataAccounts()
{
    std::vector<std::string> accounts;
    const std::wstring steam = SteamPath();
    if (steam.empty()) return accounts;
    const std::wstring base = steam + L"\\userdata\\";
    WIN32_FIND_DATAW fd{};
    HANDLE h = ::FindFirstFileW((base + L"*").c_str(), &fd);
    if (h == INVALID_HANDLE_VALUE) return accounts;
    do {
        if (!(fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) || fd.cFileName[0] == L'.') continue;
        const DWORD attr = ::GetFileAttributesW((base + fd.cFileName + L"\\582010").c_str());
        if (attr == INVALID_FILE_ATTRIBUTES || !(attr & FILE_ATTRIBUTE_DIRECTORY)) continue;
        accounts.push_back(Narrow(fd.cFileName));
    } while (::FindNextFileW(h, &fd));
    ::FindClose(h);
    return accounts;
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

// Decimal conversion with explicit bounds (no overflow or signed/partial input).
bool Decimal(const std::string& text, unsigned long long max, unsigned long long& value)
{
    value = 0;
    if (text.empty()) return false;
    for (char c : text) {
        if (c < '0' || c > '9' || value > (max - (c - '0')) / 10) return false;
        value = value * 10 + (c - '0');
    }
    return value != 0;
}

bool Tokenize(const std::string& text, std::vector<std::string>& tokens)
{
    for (size_t i = 0; i < text.size();) {
        const char c = text[i++];
        if (c == ' ' || c == '\t' || c == '\r' || c == '\n') continue;
        if (c == '/' && i < text.size() && text[i] == '/') {
            while (i < text.size() && text[i] != '\n') ++i;
            continue;
        }
        if (c == '{' || c == '}') { tokens.emplace_back(1, c); continue; }
        if (c != '"') return false;
        std::string token;
        bool closed = false;
        while (i < text.size()) {
            char next = text[i++];
            if (next == '"') { closed = true; break; }
            if (next == '\\') {
                if (i == text.size()) return false;
                next = text[i++];
            }
            token += next;
        }
        if (!closed || token == "{" || token == "}") return false;
        tokens.push_back(token);
    }
    return true;
}

// False means malformed or conflicting explicit identity: do not guess via userdata.
bool MostRecent(const std::string& text, std::string& selected)
{
    if (text.empty()) return true;
    std::vector<std::string> t;
    if (!Tokenize(text, t) || t.size() < 3 || t[0] != "users" || t[1] != "{") return false;
    size_t i = 2;
    const unsigned long long base = 76561197960265728ULL;
    while (i < t.size() && t[i] != "}") {
        const std::string sid = t[i++];
        unsigned long long numeric = 0;
        if (!Decimal(sid, base + 0xffffffffULL, numeric) || numeric <= base ||
            sid != std::to_string(numeric) || i == t.size() || t[i++] != "{") return false;
        bool recent = false, seenRecent = false;
        while (i < t.size() && t[i] != "}") {
            const std::string key = t[i++];
            if (i == t.size() || key == "{" || t[i] == "{" || t[i] == "}") return false;
            const std::string value = t[i++];
            if (key == "MostRecent") {
                if (seenRecent || (value != "0" && value != "1")) return false;
                seenRecent = true;
                recent = value == "1";
            }
        }
        if (i == t.size()) return false;
        ++i;
        if (recent) {
            if (!selected.empty()) return false;
            selected = sid;
        }
    }
    return i < t.size() && t[i] == "}" && i + 1 == t.size();
}

}  // namespace

namespace detail {
std::string SelectSteamId(unsigned long activeAccount, const std::string& loginUsers,
                          const std::vector<std::string>& userdataAccounts)
{
    const unsigned long long base = 76561197960265728ULL;
    if (activeAccount && activeAccount <= 0xffffffffULL) return std::to_string(base + activeAccount);
    std::string recent;
    if (!MostRecent(loginUsers, recent)) return std::string();
    if (!recent.empty()) return recent;
    std::string only;
    for (const std::string& account : userdataAccounts) {
        unsigned long long numeric = 0;
        if (!Decimal(account, 0xffffffffULL, numeric) || account != std::to_string(numeric)) continue;
        const std::string sid = std::to_string(base + numeric);
        if (!only.empty() && only != sid) return std::string();
        only = sid;
    }
    return only;
}
}  // namespace detail

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
        const unsigned long active = ActiveAccount();
        const std::string login = active ? std::string() : ReadLoginUsers();
        g_steamId = detail::SelectSteamId(active, login, {});
        const bool explicitIdentity = !g_steamId.empty();
        if (!explicitIdentity) g_steamId = detail::SelectSteamId(active, login, UserDataAccounts());
        Log("profile: SteamID=%s (%s)", g_steamId.empty() ? "(unknown)" : g_steamId.c_str(),
            active ? "ActiveUser (live Steam process)" : explicitIdentity ? "MostRecent (cached)" :
            g_steamId.empty() ? "missing/ambiguous identity" : "unique userdata fallback");
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
