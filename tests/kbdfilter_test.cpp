// 键盘过滤的测试：过滤器编进测试程序自己，被换掉的就是测试程序导入表里的
// GetAsyncKeyState / GetKeyState / GetKeyboardState —— 直接调 ::GetAsyncKeyState 就经过钩子。
//
// 不碰真实键盘：只验证「关着时原样放行、开着时一律读不到（返回 0 / 全零）」。
#include "kbdfilter.h"

#include <windows.h>
#include <cstdio>
#include <cstring>
#include <string>

static int g_fail = 0;
#define CHECK(cond, ...) do { if (cond) std::printf("  ok   " __VA_ARGS__); \
    else { std::printf("  FAIL " __VA_ARGS__); ++g_fail; } std::printf("\n"); } while (0)

typedef SHORT(WINAPI* AsyncFn)(int);
typedef SHORT(WINAPI* StateFn)(int);
typedef BOOL(WINAPI* KbStateFn)(PBYTE);

int main()
{
    HMODULE u = ::GetModuleHandleW(L"user32.dll");
    const auto realAsync = reinterpret_cast<AsyncFn>(::GetProcAddress(u, "GetAsyncKeyState"));
    const auto realState = reinterpret_cast<StateFn>(::GetProcAddress(u, "GetKeyState"));
    const auto realKb = reinterpret_cast<KbStateFn>(::GetProcAddress(u, "GetKeyboardState"));

    std::string info;
    CHECK(kbdfilter::Install(info), "Install：%s", info.c_str());
    CHECK(reinterpret_cast<void*>(&::GetAsyncKeyState) != reinterpret_cast<void*>(realAsync) &&
          reinterpret_cast<void*>(&::GetKeyState) != reinterpret_cast<void*>(realState) &&
          reinterpret_cast<void*>(&::GetKeyboardState) != reinterpret_cast<void*>(realKb),
          "导入表里的按键函数都换成了钩子");

    // ---- 面板关着：原样放行 ----
    const int probe[] = {VK_SHIFT, VK_ESCAPE, 'W'};
    for (int vk : probe) {
        const SHORT a = ::GetAsyncKeyState(vk), b = realAsync(vk);
        CHECK((a & 0x8000) == (b & 0x8000), "关着：VK %d 和原函数一致（0x%04X）", vk, static_cast<unsigned short>(a));
    }
    {
        BYTE a[256] = {}, b[256] = {};
        CHECK(::GetKeyboardState(a) && realKb(b) && std::memcmp(a, b, sizeof(a)) == 0,
              "关着：GetKeyboardState 和原函数一致");
    }

    // ---- 面板开着：什么都读不到 ----
    kbdfilter::SetBlock(true);
    bool any = false;
    const int probe2[] = {VK_SHIFT, VK_ESCAPE, 'W', VK_LBUTTON};
    for (int vk : probe2) any = any || ::GetAsyncKeyState(vk) != 0;
    CHECK(!any, "开着：GetAsyncKeyState 一律 0（按键还是鼠标键）");
    CHECK(::GetKeyState(VK_SHIFT) == 0 && ::GetKeyState(VK_LBUTTON) == 0, "开着：GetKeyState 一律 0");
    {
        BYTE s[256];
        std::memset(s, 0xAA, sizeof(s));
        CHECK(::GetKeyboardState(s) && s[0] == 0 && s[VK_SHIFT] == 0 && s[255] == 0, "开着：GetKeyboardState 全零");

        BYTE* nullp = nullptr;
        CHECK(::GetKeyboardState(nullp) == TRUE, "开着：传空指针也不炸");
    }

    // ---- 关掉面板：又原样放行 ----
    kbdfilter::SetBlock(false);
    {
        const SHORT a = ::GetAsyncKeyState(VK_SHIFT), b = realAsync(VK_SHIFT);
        CHECK((a & 0x8000) == (b & 0x8000), "关掉后：GetAsyncKeyState 又和原函数一致（0x%04X）", static_cast<unsigned short>(a));
        BYTE s[256] = {}, t[256] = {};
        CHECK(::GetKeyboardState(s) && realKb(t) && std::memcmp(s, t, sizeof(s)) == 0, "关掉后：GetKeyboardState 又和原函数一致");
    }

    std::printf("\n%s（%d 项失败）\n", g_fail ? "有失败" : "全部通过", g_fail);
    return g_fail ? 1 : 0;
}
