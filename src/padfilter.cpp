#include "padfilter.h"

#include "hook.h"
#include "i18n.h"
#include "log.h"

#include <windows.h>
#include <xinput.h>
#include <atomic>
#include <cstdio>
#include <cstring>

namespace padfilter {
namespace {

typedef DWORD(WINAPI* GetStateFn)(DWORD, XINPUT_STATE*);

// 进程里可能同时有好几个 XInput 库（游戏用 xinput9_1_0，我们自己的快捷键和 ImGui 加载的
// 是 xinput1_4），各有各的 XInputGetState。每找到一个就占一个槽，钩子按槽调回对应的原函数。
constexpr int kSlots = 12;
GetStateFn g_orig[kSlots] = {};
int g_slots = 0;
std::atomic<bool> g_block{false};
std::atomic<long long> g_calls{0};   // 游戏调了几次 XInputGetState（诊断用）
bool g_installed = false;

// 面板关着的时候：什么都不改，游戏拿到的就是真实手柄状态（这就是「策略」的一半）。
// 面板开着的时候：上面那段 muted 会把整个手柄状态清零（另一半）。
// 以前还在这里做「组合键扣留」——为了不让游戏看到面板快捷键的那几个键，先把按键按住
// 最多 100 ms 看会不会凑成组合键，没凑成就补发。这既拖慢了手感（原版也卡），
// 又把快捷键和游戏按键绑死；现在改成：面板快捷键只从「过滤前的原始状态」里认，
// 游戏照常收到按键，所以面板快捷键要选游戏自己不用的组合（默认 View+Menu）。
struct Pad {
    bool muted = false;           // 面板开过：等全部松开才恢复
};
Pad g_pad[XUSER_MAX_COUNT];

// 给快捷键用的「游戏刚读到的原始状态」（过滤前：组合键扣住的键也照记，快捷键要认它们）
struct Published {
    std::atomic<unsigned int> buttons{0};
    std::atomic<unsigned int> triggers{0};   // 低 8 位 LT，高 8 位 RT
    std::atomic<unsigned long long> at{0};
} g_pub[XUSER_MAX_COUNT];

void Publish(DWORD i, const XINPUT_STATE& s)
{
    if (i >= XUSER_MAX_COUNT) return;
    g_pub[i].buttons.store(s.Gamepad.wButtons);
    g_pub[i].triggers.store((unsigned)s.Gamepad.bLeftTrigger | ((unsigned)s.Gamepad.bRightTrigger << 8));
    g_pub[i].at.store(::GetTickCount64());
}


bool AnyDown(const XINPUT_GAMEPAD& g)
{
    return g.wButtons != 0 || g.bLeftTrigger > XINPUT_GAMEPAD_TRIGGER_THRESHOLD ||
           g.bRightTrigger > XINPUT_GAMEPAD_TRIGGER_THRESHOLD;
}

void Filter(DWORD i, XINPUT_STATE* s)
{
    if (i >= XUSER_MAX_COUNT || !s) return;
    Pad& p = g_pad[i];
    XINPUT_GAMEPAD& g = s->Gamepad;

    if (g_block.load()) p.muted = true;
    if (p.muted) {
        if (!g_block.load() && !AnyDown(g)) {
            p.muted = false;
        } else {
            std::memset(&g, 0, sizeof(g));
            return;
        }
    }
}

template <int K> DWORD WINAPI HkGetState(DWORD i, XINPUT_STATE* s)
{
    ++g_calls;
    const DWORD r = g_orig[K](i, s);
    if (r == ERROR_SUCCESS) {
        Publish(i, *s);
        Filter(i, s);
    }
    return r;
}

typedef DWORD(WINAPI* HookFn)(DWORD, XINPUT_STATE*);
const HookFn kHooks[kSlots] = {HkGetState<0>, HkGetState<1>, HkGetState<2>, HkGetState<3>,
                               HkGetState<4>, HkGetState<5>, HkGetState<6>, HkGetState<7>,
                               HkGetState<8>, HkGetState<9>, HkGetState<10>, HkGetState<11>};

} // namespace

bool Install(std::string& info)
{
    if (g_installed) return true;
    // MHW 导入的是 XINPUT9_1_0.dll。每个加载了的 XInput 库都试，XInputGetState 和序号 100
    //（XInputGetStateEx，带 Guide 键）都试；同一个地址只占一个槽。
    static const wchar_t* const kDlls[] = {L"xinput9_1_0.dll", L"xinput1_4.dll", L"xinput1_3.dll",
                                           L"xinput1_2.dll", L"xinput1_1.dll"};
    int n = 0;
    bool anyModule = false;
    for (const wchar_t* d : kDlls) {
        HMODULE m = ::GetModuleHandleW(d);
        if (!m) continue;
        anyModule = true;
        const FARPROC fns[2] = {::GetProcAddress(m, "XInputGetState"), ::GetProcAddress(m, reinterpret_cast<LPCSTR>(100))};
        for (int k = 0; k < 2; ++k) {
            GetStateFn fn = reinterpret_cast<GetStateFn>(fns[k]);
            if (!fn || g_slots >= kSlots) continue;
            bool dup = false;
            for (int s = 0; s < g_slots; ++s) dup |= (g_orig[s] == fn);
            if (dup) continue;
            const int slot = g_slots;
            g_orig[slot] = fn;   // 先填好原函数再换指针：换完的下一瞬间游戏就可能调进来
            char what[64];
            std::snprintf(what, sizeof(what), "%ls!%s", d, k == 0 ? "XInputGetState" : "#100");
            // 存原函数的表本身不能换，否则钩子调「原函数」就是调自己。游戏里不会碰到
            //（这张表在我们的 DLL 里，扫的是游戏 exe），过滤器被编进被扫的模块时（测试）会。
            std::string where;
            const int hit = hook::ReplaceModulePointers(reinterpret_cast<std::uintptr_t>(::GetModuleHandleW(nullptr)),
                                                        reinterpret_cast<void*>(fn), reinterpret_cast<void*>(kHooks[slot]),
                                                        g_orig, g_orig + kSlots, &where);
            if (hit) { Log("padfilter: 换掉 %s处的 %s", where.c_str(), what); ++g_slots; n += hit; }
            else g_orig[slot] = nullptr;
        }
    }
    if (!anyModule) { info = i18n::Tr("游戏还没加载 XInput", "the game has not loaded XInput yet", "ゲームがまだ XInput を読み込んでいません"); return false; }
    if (n == 0) { info = i18n::Tr("游戏模块里没找到任何 XInputGetState 的指针",
                                  "no XInputGetState pointer found in game modules",
                                  "ゲームのモジュールに XInputGetState のポインタが見つかりません"); return false; }
    g_installed = true;
    info = i18n::Fmt(i18n::Tr("替换了 %d 处", "patched %d slot(s)", "%d 箇所を差し替え"), n);
    return true;
}

bool Installed() { return g_installed; }

bool Latest(unsigned long pad, unsigned short* buttons, unsigned char* lt, unsigned char* rt)
{
    if (pad >= XUSER_MAX_COUNT) return false;
    const unsigned long long at = g_pub[pad].at.load();
    if (!at || ::GetTickCount64() - at > 1000) return false;
    if (buttons) *buttons = (unsigned short)g_pub[pad].buttons.load();
    const unsigned t = g_pub[pad].triggers.load();
    if (lt) *lt = (unsigned char)(t & 0xFF);
    if (rt) *rt = (unsigned char)((t >> 8) & 0xFF);
    return true;
}

std::string Profile()
{
    char b[96];
    std::snprintf(b, sizeof(b), "XInputGetState %lld 次", g_calls.load());
    return b;
}

void TestFilter(unsigned long pad, void* s) { Filter(pad, static_cast<XINPUT_STATE*>(s)); }
void SetBlock(bool block) { g_block = block; }

} // namespace padfilter
