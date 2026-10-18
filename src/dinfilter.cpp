#include "dinfilter.h"

#include "hook.h"
#include "i18n.h"
#include "log.h"

#include <windows.h>
#define DIRECTINPUT_VERSION 0x0800
#include <dinput.h>
#include <tlhelp32.h>

#include <atomic>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <string>
#include <vector>

//  面板开着时让游戏读不到 DirectInput 键盘/鼠标。
//
//  做法（0.7.6 起）：不自己建设备，而是把游戏模块里 `DirectInput8Create` 的指针换成我们的。
//  游戏调它拿到 IDirectInput8 后，把那张虚表的 CreateDevice（第 4 项）也换成我们的，这样
//  游戏每建一台键盘/鼠标设备，就把这张设备虚表的 GetDeviceState（第 10 项）和
//  GetDeviceData（第 11 项）换掉。
//
//  为什么不自己建设备：自己 CreateDevice 会让进程里多注册一路 DirectInput（内部走 Raw
//  Input，多一个隐藏窗口、多一条输入通道），实测会让键盘鼠标整体变钝——这是 0.7.5 及以前
//  「无差别输入延迟」的来源。`[Debug] DinCreate=1` 可以退回老办法，只为排查用。
//
//  虚表是接口级、整个进程共用的，换一次游戏先前建的同类设备也一起生效。

namespace dinfilter {
namespace {

typedef HRESULT(WINAPI* CreateFn)(HINSTANCE, DWORD, REFIID, LPVOID*, LPUNKNOWN);
typedef HRESULT(WINAPI* CreateDeviceFn)(void*, const GUID&, void**, IUnknown*);
typedef HRESULT(WINAPI* GetStateFn)(void*, DWORD, LPVOID);
typedef HRESULT(WINAPI* GetDataFn)(void*, DWORD, LPVOID, LPDWORD, DWORD);

CreateFn g_realCreate = nullptr;
CreateDeviceFn g_realCreateDevice = nullptr;
void** g_diVt = nullptr;          // 游戏那个 IDirectInput8 的虚表（只打一次）

// 每张设备虚表各自的原函数：A/W 接口、键盘、鼠标各一张
struct Slot {
    void** vt;
    GetStateFn state;
    GetDataFn data;
};
Slot g_slots[8] = {};
int g_nslots = 0;

std::atomic<bool> g_block{false};
bool g_installed = false;
std::atomic<int> g_devices{0};           // 截到几台游戏设备

Slot* Find(void* self)
{
    void** vt = *reinterpret_cast<void***>(self);
    for (int i = 0; i < g_nslots; ++i)
        if (g_slots[i].vt == vt) return &g_slots[i];
    return nullptr;
}

// 键盘：cb=256，全零 = 所有键都松开；鼠标：DIMOUSESTATE，全零 = 没动也没按
HRESULT WINAPI HkGetDeviceState(void* self, DWORD cb, LPVOID data)
{
    Slot* s = Find(self);
    if (g_block.load(std::memory_order_relaxed)) {
        if (data && cb) std::memset(data, 0, cb);
        return DI_OK;
    }
    return s ? s->state(self, cb, data) : DIERR_NOTACQUIRED;
}

HRESULT WINAPI HkGetDeviceData(void* self, DWORD cb, LPVOID data, LPDWORD inout, DWORD flags)
{
    Slot* s = Find(self);
    if (g_block.load(std::memory_order_relaxed)) {
        if (inout) *inout = 0;   // 没有输入事件
        return DI_OK;
    }
    return s ? s->data(self, cb, data, inout, flags) : DI_OK;
}

bool PatchSlot(void** slot, void* fn)
{
    DWORD old = 0;
    if (!::VirtualProtect(slot, sizeof(void*), PAGE_READWRITE, &old)) return false;
    *slot = fn;
    ::VirtualProtect(slot, sizeof(void*), old, &old);
    ::FlushInstructionCache(::GetCurrentProcess(), slot, sizeof(void*));
    return true;
}

// 第 9 项 GetDeviceState、第 10 项 GetDeviceData（IUnknown 占 0~2，GetCapabilities 是第 4 项）
bool PatchVtable(void** vt)
{
    if (!vt || !vt[9] || !vt[10]) return false;
    for (int i = 0; i < g_nslots; ++i)
        if (g_slots[i].vt == vt) return true;
    if (g_nslots >= 8) return false;
    Slot& s = g_slots[g_nslots++];
    s.vt = vt;
    s.state = reinterpret_cast<GetStateFn>(vt[9]);
    s.data = reinterpret_cast<GetDataFn>(vt[10]);
    return PatchSlot(&vt[9], reinterpret_cast<void*>(HkGetDeviceState)) &&
           PatchSlot(&vt[10], reinterpret_cast<void*>(HkGetDeviceData));
}

// 游戏建键盘/鼠标设备时：拿它返回的对象，换那张虚表
HRESULT WINAPI HkCreateDevice(void* self, const GUID& guid, void** out, IUnknown* outer)
{
    const HRESULT hr = g_realCreateDevice(self, guid, out, outer);
    if (SUCCEEDED(hr) && out && *out) {
        void** vt = *reinterpret_cast<void***>(*out);
        if (PatchVtable(vt)) ++g_devices;
    }
    return hr;
}

void PatchDi(void* di)
{
    if (g_diVt) return;
    void** vt = *reinterpret_cast<void***>(di);
    if (!vt || !vt[3]) return;
    g_diVt = vt;
    g_realCreateDevice = reinterpret_cast<CreateDeviceFn>(vt[3]);
    if (PatchSlot(&vt[3], reinterpret_cast<void*>(HkCreateDevice)))
        Log("dinfilter: 挂上游戏的 IDirectInput8::CreateDevice");
}

HRESULT WINAPI HkCreate(HINSTANCE inst, DWORD ver, REFIID iid, void** out, IUnknown* outer)
{
    const HRESULT hr = g_realCreate(inst, ver, iid, out, outer);
    if (SUCCEEDED(hr) && out && *out) PatchDi(*out);
    return hr;
}

bool UnderWindowsDir(const wchar_t* path)
{
    wchar_t win[MAX_PATH] = {};
    const UINT n = ::GetWindowsDirectoryW(win, MAX_PATH);
    if (!n || n >= MAX_PATH) return false;
    return ::_wcsnicmp(path, win, std::wcslen(win)) == 0;
}

std::vector<std::uintptr_t> Targets()
{
    std::vector<std::uintptr_t> out;
    HMODULE self = nullptr;
    ::GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                         reinterpret_cast<LPCWSTR>(&Install), &self);
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

std::string Hex(long hr)
{
    char b[16];
    std::snprintf(b, sizeof(b), "0x%08lX", static_cast<unsigned long>(hr));
    return b;
}

// 老办法（[Debug] DinCreate=1）：自己建一台键盘、鼠标设备，直接换它们的虚表。
// 简单，但会让进程多出一路 DirectInput 输入通道（延迟就是这么来的）。
bool CreateOwn(const CreateFn create, std::string& info)
{
    IDirectInput8W* di = nullptr;
    const HRESULT hr = create(::GetModuleHandleW(nullptr), DIRECTINPUT_VERSION, IID_IDirectInput8W,
                              reinterpret_cast<void**>(&di), nullptr);
    if (FAILED(hr) || !di) {
        info = i18n::Fmt(i18n::Tr("建不了 DirectInput8（hr=%s）", "cannot create DirectInput8 (hr=%s)",
                                  "DirectInput8 を作成できません（hr=%s）"),
                         Hex(hr).c_str());
        return false;
    }
    IDirectInputDevice8W* kb = nullptr;
    IDirectInputDevice8W* ms = nullptr;
    if (SUCCEEDED(di->CreateDevice(GUID_SysKeyboard, &kb, nullptr)) && kb) PatchVtable(*reinterpret_cast<void***>(kb));
    if (SUCCEEDED(di->CreateDevice(GUID_SysMouse, &ms, nullptr)) && ms) PatchVtable(*reinterpret_cast<void***>(ms));
    if (kb) kb->Release();
    if (ms) ms->Release();
    di->Release();
    if (!g_nslots) {
        info = i18n::Tr("建不了 DirectInput 键盘/鼠标设备", "cannot create a DirectInput keyboard/mouse device",
                        "DirectInput のキーボード／マウスを作成できません");
        return false;
    }
    Log("dinfilter: 自己建了设备，换掉 %d 张虚表（DinCreate=1）", g_nslots);
    info = i18n::Fmt(i18n::Tr("设备虚表 x%d（自建）", "device vtables x%d (own)", "デバイス仮想表 x%d（自前）"), g_nslots);
    g_installed = true;
    return true;
}

} // namespace

bool Install(std::string& info)
{
    if (g_installed) return true;
    HMODULE di8 = ::GetModuleHandleW(L"dinput8.dll");
    if (!di8) {
        info = i18n::Tr("dinput8.dll 还没加载", "dinput8.dll is not loaded yet", "dinput8.dll がまだ読み込まれていません");
        return false;
    }
    const FARPROC fn = ::GetProcAddress(di8, "DirectInput8Create");
    if (!fn) {
        info = i18n::Tr("dinput8.dll 里没有 DirectInput8Create", "no DirectInput8Create in dinput8.dll",
                        "dinput8.dll に DirectInput8Create がありません");
        return false;
    }
    g_realCreate = reinterpret_cast<CreateFn>(fn);

    // 两路一起上：
    // 1) 换掉游戏模块里的 DirectInput8Create 指针，游戏之后建的设备都会被我们接住；
    // 2) 自己建一台键盘、鼠标——设备虚表是接口级、进程内共用的，所以游戏**先前**建好的
    //    设备也会被一起换掉。只做 1) 的时候，游戏在插件装好之前就建了设备，就会整段漏掉。
    int hits = 0;
    for (std::uintptr_t base : Targets()) {
        std::string where;
        const int hit = hook::ReplaceModulePointers(base, reinterpret_cast<void*>(fn),
                                                   reinterpret_cast<void*>(HkCreate),
                                                   &g_realCreate, &g_realCreate + 1, &where);
        if (hit) {
            Log("dinfilter: 换掉 %s处的 DirectInput8Create", where.c_str());
            hits += hit;
        }
    }

    std::string own;
    const bool ownOk = CreateOwn(g_realCreate, own);
    if (ownOk) g_installed = true;
    if (!hits && !ownOk) {
        info = i18n::Tr("游戏模块里没找到 DirectInput8Create，也建不了自己的设备",
                        "no DirectInput8Create pointer in game modules, and cannot create our own device",
                        "ゲームのモジュールに DirectInput8Create のポインタがなく、自前のデバイスも作れません");
        return false;
    }
    if (g_nslots) g_installed = true;
    info = ownOk
               ? i18n::Fmt(i18n::Tr("DirectInput8Create x%d + 设备虚表 x%d", "DirectInput8Create x%d + vtables x%d",
                                    "DirectInput8Create x%d + 仮想表 x%d"),
                           hits, g_nslots)
               : i18n::Fmt(i18n::Tr("DirectInput8Create x%d", "DirectInput8Create x%d", "DirectInput8Create x%d"), hits);
    return true;
}

bool Installed() { return g_installed; }

void SetBlock(bool block) { g_block.store(block, std::memory_order_relaxed); }

void ResetStats() { g_devices.store(0); }

int Vtables() { return g_nslots; }

int GameDevices() { return g_devices.load(); }

std::string Stats()
{
    std::string s = i18n::Fmt(i18n::Tr("设备虚表 %d 张（游戏建设备 %d 次）", "%d device vtable(s), %d created by the game",
                                       "デバイス仮想表 %d 枚（ゲームが %d 回作成）"),
                              g_nslots, g_devices.load());
    return s;
}

} // namespace dinfilter
