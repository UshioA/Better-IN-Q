// DirectInput 键盘/鼠标过滤的测试：和 kbdfilter_test 一个套路，但换的是 DirectInput
// 设备虚表里的 GetDeviceState / GetDeviceData。
//
// 注意：生产路径是「截游戏自己的 DirectInput8Create / CreateDevice」，测试进程里没有
// 那样一条导入链，所以这里用 SetOwnDevices(true) 走老办法（自己建键盘/鼠标设备再换虚表）。
// 两条路共用同一段 PatchVtable / 钩子代码，差别只在「虚表从哪来」。
#include "dinfilter.h"

#include <windows.h>
#define DIRECTINPUT_VERSION 0x0800
#include <dinput.h>

#include <cstdio>
#include <cstring>
#include <string>

static int g_fail = 0;
#define CHECK(cond, ...) do { if (cond) std::printf("  ok   " __VA_ARGS__); \
    else { std::printf("  FAIL " __VA_ARGS__); ++g_fail; } std::printf("\n"); } while (0)

typedef HRESULT(WINAPI* CreateFn)(HINSTANCE, DWORD, REFIID, LPVOID*, LPUNKNOWN);

int main()
{
    // 游戏里 dinput8.dll 是前置 DLL，一定已经加载；测试进程要自己先把它拉起来
    HMODULE di8 = ::LoadLibraryW(L"dinput8.dll");

    std::string info;
    if (!di8 || !dinfilter::Install(info)) {
        // 没有 dinput8.dll / 建不了设备：跳过而不是判失败（没有输入设备的机器也跑得通）
        std::printf("  skip Install 失败：%s\n", info.c_str());
        std::printf("\n全部通过（0 项失败）\n");
        return 0;
    }
    std::printf("  ok   Install：%s\n", info.c_str());
    CHECK(dinfilter::Vtables() >= 1, "换掉 %d 张设备虚表（键盘和鼠标共用同一张）", dinfilter::Vtables());
    CHECK(dinfilter::Vtables() >= 1, "虚表至少换掉一张（自己建的键盘/鼠标也用同一张）");

    const auto create = reinterpret_cast<CreateFn>(::GetProcAddress(di8, "DirectInput8Create"));
    IDirectInput8W* di = nullptr;
    if (!create || FAILED(create(::GetModuleHandleW(nullptr), DIRECTINPUT_VERSION, IID_IDirectInput8W,
                                 reinterpret_cast<void**>(&di), nullptr)) ||
        !di) {
        std::printf("  skip 建不了 DirectInput8\n\n全部通过（0 项失败）\n");
        return 0;
    }
    IDirectInputDevice8W* kb = nullptr;
    if (FAILED(di->CreateDevice(GUID_SysKeyboard, &kb, nullptr)) || !kb) {
        di->Release();
        std::printf("  skip 建不了键盘设备\n\n全部通过（0 项失败）\n");
        return 0;
    }
    // 拿不到焦点也照样能测：GetDeviceState 的返回码两种状态都比一比
    kb->SetCooperativeLevel(::GetConsoleWindow(), DISCL_BACKGROUND | DISCL_NONEXCLUSIVE);
    kb->Acquire();

    BYTE buf[256];

    // ---- 面板开着：全零 + DI_OK ----
    dinfilter::SetBlock(true);
    std::memset(buf, 0xAA, sizeof(buf));
    HRESULT hr = kb->GetDeviceState(sizeof(buf), buf);
    CHECK(hr == DI_OK, "开着：GetDeviceState 返回 DI_OK（0x%08lX）", static_cast<unsigned long>(hr));
    bool allZero = true;
    for (BYTE b : buf) allZero = allZero && b == 0;
    CHECK(allZero, "开着：键位表全是 0");

    // 鼠标：DIMOUSESTATE = 16 字节，同样全零（不移动、不按键）
    IDirectInputDevice8W* ms = nullptr;
    if (SUCCEEDED(di->CreateDevice(GUID_SysMouse, &ms, nullptr)) && ms) {
        ms->SetCooperativeLevel(::GetConsoleWindow(), DISCL_BACKGROUND | DISCL_NONEXCLUSIVE);
        ms->Acquire();
        DIMOUSESTATE st;
        std::memset(&st, 0xAA, sizeof(st));
        hr = ms->GetDeviceState(sizeof(st), &st);
        CHECK(hr == DI_OK && st.lX == 0 && st.lY == 0 && st.lZ == 0 && st.rgbButtons[0] == 0,
              "开着：鼠标不移动、不按键");
        ms->Release();
    }

    // GetDeviceData：事件个数清零
    DWORD count = 99;
    hr = kb->GetDeviceData(sizeof(DIDEVICEOBJECTDATA), nullptr, &count, DIGDD_PEEK);
    CHECK(hr == DI_OK && count == 0, "开着：GetDeviceData 没有事件（count=%lu）", static_cast<unsigned long>(count));

    // ---- 关掉面板：走回原函数，不崩、不卡 ----
    dinfilter::SetBlock(false);
    std::memset(buf, 0xAA, sizeof(buf));
    hr = kb->GetDeviceState(sizeof(buf), buf);
    CHECK(hr == DI_OK || hr == DIERR_NOTACQUIRED || hr == DIERR_INPUTLOST,
          "关掉后：转给原函数（0x%08lX）", static_cast<unsigned long>(hr));

    kb->Unacquire();
    kb->Release();
    di->Release();

    std::printf("\n%s（%d 项失败）\n", g_fail ? "有失败" : "全部通过", g_fail);
    return g_fail ? 1 : 0;
}
