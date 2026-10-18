#include "kbdfilter.h"

#include "hook.h"
#include "i18n.h"
#include "log.h"

#include <windows.h>
#include <tlhelp32.h>

#include <atomic>
#include <cstdint>
#include <cstring>
#include <cwchar>
#include <string>
#include <vector>

namespace kbdfilter {
namespace {

typedef SHORT(WINAPI* AsyncKeyFn)(int);
typedef SHORT(WINAPI* KeyStateFn)(int);
typedef BOOL(WINAPI* KeyboardStateFn)(PBYTE);
typedef UINT(WINAPI* RawInputDataFn)(HRAWINPUT, UINT, LPVOID, PUINT, UINT);
typedef UINT(WINAPI* RawInputBufferFn)(PRAWINPUT, PUINT, UINT);

// 原函数（GetProcAddress 拿的）。换指针时要跳过这张表本身，理由同 padfilter。
struct Orig {
    AsyncKeyFn async;
    KeyStateFn state;
    KeyboardStateFn kbState;
    RawInputDataFn rid;
    RawInputBufferFn rib;
} g_orig = {};

std::atomic<bool> g_block{false};
bool g_installed = false;
// 面板开着期间每个函数被调了几次（诊断用：日志里能看出游戏到底走哪条路读键盘）
std::atomic<long> g_calls[5] = {};

SHORT WINAPI HkGetAsyncKeyState(int vk)
{
    if (!g_block.load(std::memory_order_relaxed)) return g_orig.async(vk);
    ++g_calls[0];
    return 0;
}

SHORT WINAPI HkGetKeyState(int vk)
{
    if (!g_block.load(std::memory_order_relaxed)) return g_orig.state(vk);
    ++g_calls[1];
    return 0;
}

BOOL WINAPI HkGetKeyboardState(PBYTE st)
{
    if (!g_block.load(std::memory_order_relaxed)) return g_orig.kbState(st);
    ++g_calls[2];
    if (st) std::memset(st, 0, 256);   // 全「没按下」
    return TRUE;
}

UINT WINAPI HkGetRawInputData(HRAWINPUT h, UINT cmd, LPVOID data, PUINT size, UINT hdr)
{
    if (!g_block.load(std::memory_order_relaxed)) return g_orig.rid(h, cmd, data, size, hdr);
    ++g_calls[3];
    if (size) *size = 0;               // 没数据：游戏这一帧读不到按键
    return 0;
}

UINT WINAPI HkGetRawInputBuffer(PRAWINPUT in, PUINT size, UINT hdr)
{
    if (!g_block.load(std::memory_order_relaxed)) return g_orig.rib(in, size, hdr);
    ++g_calls[4];
    if (size) *size = 0;
    return 0;                          // 0 个 RAWINPUT
}

// 自己的模块不能换：input.cpp 还在用 GetAsyncKeyState 读快捷键
HMODULE SelfModule()
{
    HMODULE self = nullptr;
    ::GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                         reinterpret_cast<LPCWSTR>(&Install), &self);
    return self;
}

bool UnderWindowsDir(const wchar_t* path)
{
    wchar_t win[MAX_PATH] = {};
    const UINT n = ::GetWindowsDirectoryW(win, MAX_PATH);
    if (!n || n >= MAX_PATH) return false;
    const std::size_t len = std::wcslen(win);
    return ::_wcsnicmp(path, win, len) == 0;
}

// 游戏本体 + 它带的那些 DLL（模组、中间件）都可能读键盘；系统 DLL 不用扫
std::vector<std::uintptr_t> Targets()
{
    std::vector<std::uintptr_t> out;
    const HMODULE self = SelfModule();
    const HANDLE snap = ::CreateToolhelp32Snapshot(TH32CS_SNAPMODULE, ::GetCurrentProcessId());
    if (snap == INVALID_HANDLE_VALUE) {
        out.push_back(reinterpret_cast<std::uintptr_t>(::GetModuleHandleW(nullptr)));
        return out;
    }
    MODULEENTRY32W me = {};
    me.dwSize = sizeof(me);
    for (BOOL ok = ::Module32FirstW(snap, &me); ok; ok = ::Module32NextW(snap, &me)) {
        if (me.hModule == self || UnderWindowsDir(me.szExePath)) continue;
        out.push_back(reinterpret_cast<std::uintptr_t>(me.hModule));
    }
    ::CloseHandle(snap);
    if (out.empty()) out.push_back(reinterpret_cast<std::uintptr_t>(::GetModuleHandleW(nullptr)));
    return out;
}

} // namespace

bool Install(std::string& info)
{
    if (g_installed) return true;
    HMODULE u = ::GetModuleHandleW(L"user32.dll");
    if (!u) { info = i18n::Tr("user32 还没加载", "user32 is not loaded yet", "user32 がまだ読み込まれていません"); return false; }
    // 先填好原函数再换指针：换完的下一瞬间游戏就可能调进来
    g_orig.async = reinterpret_cast<AsyncKeyFn>(::GetProcAddress(u, "GetAsyncKeyState"));
    g_orig.state = reinterpret_cast<KeyStateFn>(::GetProcAddress(u, "GetKeyState"));
    g_orig.kbState = reinterpret_cast<KeyboardStateFn>(::GetProcAddress(u, "GetKeyboardState"));
    g_orig.rid = reinterpret_cast<RawInputDataFn>(::GetProcAddress(u, "GetRawInputData"));
    g_orig.rib = reinterpret_cast<RawInputBufferFn>(::GetProcAddress(u, "GetRawInputBuffer"));
    if (!g_orig.async || !g_orig.state || !g_orig.kbState || !g_orig.rid || !g_orig.rib) {
        info = i18n::Tr("user32 里找不到按键函数", "no key functions found in user32", "user32 にキー関数が見つかりません");
        return false;
    }

    const struct { const char* name; void* fn; void* hk; } fns[] = {
        {"GetAsyncKeyState", reinterpret_cast<void*>(g_orig.async), reinterpret_cast<void*>(HkGetAsyncKeyState)},
        {"GetKeyState", reinterpret_cast<void*>(g_orig.state), reinterpret_cast<void*>(HkGetKeyState)},
        {"GetKeyboardState", reinterpret_cast<void*>(g_orig.kbState), reinterpret_cast<void*>(HkGetKeyboardState)},
        {"GetRawInputData", reinterpret_cast<void*>(g_orig.rid), reinterpret_cast<void*>(HkGetRawInputData)},
        {"GetRawInputBuffer", reinterpret_cast<void*>(g_orig.rib), reinterpret_cast<void*>(HkGetRawInputBuffer)},
    };
    const int n = static_cast<int>(sizeof(fns) / sizeof(fns[0]));
    static int hits[5] = {};   // 累计：没装成会重试，前几次换掉的不会再被找到
    const std::vector<std::uintptr_t> mods = Targets();
    info.clear();
    for (int i = 0; i < n; ++i) {
        for (std::uintptr_t base : mods) {
            std::string where;
            const int hit = hook::ReplaceModulePointers(base, fns[i].fn, fns[i].hk, &g_orig, &g_orig + 1, &where);
            if (hit) Log("kbdfilter: 换掉 %s处的 %s", where.c_str(), fns[i].name);
            hits[i] += hit;
        }
        if (i) info += std::string(i18n::Tr("，", ", ", "、"));
        info += std::string(fns[i].name) + " x" + std::to_string(hits[i]);
    }
    // 一处都没换到：游戏不是这么读键盘的（DirectInput / 自己按地址调），按键还会漏
    bool any = false;
    for (int i = 0; i < n; ++i) any = any || hits[i] > 0;
    if (!any) return false;
    if (!hits[0] && !hits[1] && !hits[2]) {
        info += i18n::Tr("（没换到按键状态函数：游戏可能用 DirectInput 读键盘，按键可能还会漏）",
                         " (no key-state function patched: the game may use DirectInput, keys may still leak)",
                         "（キー状態関数を差し替えられず：ゲームが DirectInput を使う場合、キーが漏れる可能性があります）");
    }
    g_installed = true;
    return true;
}

bool Installed() { return g_installed; }

void SetBlock(bool block) { g_block.store(block, std::memory_order_relaxed); }

void ResetStats()
{
    for (int i = 0; i < 5; ++i) g_calls[i].store(0, std::memory_order_relaxed);
}

std::string Stats()
{
    static const char* const names[5] = {"GetAsyncKeyState", "GetKeyState", "GetKeyboardState",
                                         "GetRawInputData", "GetRawInputBuffer"};
    std::string s;
    for (int i = 0; i < 5; ++i) {
        if (i) s += ' ';
        s += std::string(names[i]) + "=" + std::to_string(g_calls[i].load(std::memory_order_relaxed));
    }
    return s;
}

} // namespace kbdfilter
