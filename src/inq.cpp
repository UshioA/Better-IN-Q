// ============================================================================
//  inq.cpp —— INQ.CT 的移植
//
//  两个上下文钩子（ApplyResetAndEquip / QuestLoad）跑在游戏线程里，逻辑和 CT
//  的汇编逐路径对应，连「改了寄存器再直接返回」的分支也照搬 —— 游戏后续代码
//  看到的寄存器值必须和用 CT 时一样。CT 里用 db 手写的相对跳转，按字节数核对过，
//  跳到的就是注释里写的 Z / Y / [Restart],0 那几处。
// ============================================================================
#include "inq.h"

#include "game.h"
#include "gamedata.h"
#include "hook.h"
#include "i18n.h"
#include "log.h"

#include <windows.h>
#include <atomic>
#include <cstdlib>
#include <cstring>

namespace inq {

State g;

namespace {

using namespace game;
using hook::Ctx;

bool g_ready = false;
// 运行时总闸：钩子永久驻留，Dormant 时所有游戏侧回调纯旁路。
std::atomic<bool> g_enabled{false};
std::atomic<unsigned> g_inflight{0};
// 汇编 call gate 不能直接操作 std::atomic；用 Interlocked 指令维护这两个 32 位闸门。
alignas(4) volatile std::int32_t g_gateEnabled = 0;
alignas(4) volatile std::int32_t g_gateInflight = 0;

class ActiveCall {
public:
    ActiveCall()
    {
        if (!g_enabled.load(std::memory_order_acquire)) return;
        g_inflight.fetch_add(1, std::memory_order_acq_rel);
        if (g_enabled.load(std::memory_order_acquire)) active_ = true;
        else g_inflight.fetch_sub(1, std::memory_order_release);
    }
    ~ActiveCall() { if (active_) g_inflight.fetch_sub(1, std::memory_order_release); }
    explicit operator bool() const { return active_; }
private:
    bool active_ = false;
};

// 「刚进去的那一关」。游戏自己记着最后一次进的任务（不管是我们重开的还是玩家在柜台接的），
// Tick 里发现变了就存进来，主循环用 TakeEnteredQuest() 取走、写进全局任务。
std::atomic<int> g_enteredQuest{0};
// 只用于面板主动选择的立即换装；不与重开的装备待办共用，避免改变重开状态机。
// 主循环发布，游戏线程消费；后一次选择覆盖前一次，0 取消尚未提交的请求。
std::atomic<int> g_pendingEquipApply{0};
int g_enteredSeen = 0;   // 只在游戏线程里读写

enum MsgBit : std::uint32_t { kMsgEquip = 1, kMsgQuest = 2, kMsgCant = 4, kMsgWindow = 8, kMsgNoQuest = 16 };

// ---------------------------------------------------------------------------
//  ApplyResetAndEquip：原指令 mov rcx,rbx ; call [rax+70]
//  只在 ax==71D0 && bx==02A0（某个 UI 对象的那一次调用）时介入：
//    · 有待套用的装备套装 → 借这次调用让游戏自己换装（换虚表、换对象）
//    · 否则如果请求了重开 → 判断当前能不能重开，能就借这次调用触发游戏的重置
// ---------------------------------------------------------------------------
void CantQ()
{
    g.message |= kMsgCant;
    g.restart = 0;
}

void OnApplyResetAndEquip(Ctx* c)
{
    ActiveCall call;
    if (!call) return;
    if ((c->rax & 0xFFFF) != 0x71D0 || (c->rbx & 0xFFFF) != 0x02A0) return;

    // 立即换装也只能借同一个游戏线程落点提交；读盘或 UI 指针链未就绪时保留请求。
    // 重开仍优先使用它原有的装备队列，不能在重置/接取流程中插入面板换装。
    int immediate = 0;
    if (g.equipLoadout == 0 && g.restart == 0) {
        immediate = g_pendingEquipApply.load(std::memory_order_acquire);
        if (immediate != 0) {
            const std::uintptr_t ui = Ui();
            const std::uintptr_t load = ui ? P(ui + 0x20) : 0;
            const std::uintptr_t slot = SaveSlot();
            const std::uintptr_t equipUi = ui ? P(ui + 0x310) : 0;
            const std::uintptr_t a60 = ui ? P(ui + 0x60) : 0;
            const std::uintptr_t pageUi = equipUi ? P(equipUi + 0x2960) : 0;
            const std::uintptr_t indexUi = equipUi ? P(equipUi + 0x2958) : 0;
            const std::uintptr_t equipObject = a60 ? P(a60 + 0x470) : 0;
            if (!load || Rd<std::uint32_t>(load + 0x1D04) != 0 || !slot ||
                !equipUi || a60 < 0x80 || !pageUi || !indexUi || equipObject < 0x5E0 ||
                !Readable(slot + 0x10CCF4 + std::uintptr_t(immediate - 1) * 0x2B0, 4) ||
                !Readable(pageUi + 0x240, 4) || !Readable(indexUi + 0x274, 4) ||
                !Readable(a60 - 0x80 + 0x2A28, 0x40)) return;
            if (!g_pendingEquipApply.compare_exchange_strong(immediate, 0, std::memory_order_acq_rel)) return;
        }
    }

    // ---------------- 装备套装 ----------------
    if (g.equipLoadout != 0 || immediate != 0) {
        const std::uint32_t n = immediate ? (std::uint32_t)immediate : (std::uint32_t)g.equipLoadout;
        if (!immediate) g.equipLoadout = 0;
        if (n > 224) { g.message |= kMsgEquip; return; }           // popp：寄存器原样
        const std::uintptr_t slot = SaveSlot();
        if (!slot) { g.message |= kMsgEquip; return; }
        const std::uint32_t i = n - 1;
        if (Rd<std::uint32_t>(slot + std::uintptr_t(i) * 0x2B0 + 0x10CCF4) == 0xFFFFFFFF) {
            g.message |= kMsgEquip;                                  // 这一格没放武器 = 空套装
            return;
        }
        const std::uint32_t page = i / 8, idx = i % 8;
        const std::uintptr_t ui = Ui();
        const std::uintptr_t rcx = ui ? P(ui + 0x310) : 0;
        const std::uintptr_t a60 = ui ? P(ui + 0x60) : 0;
        if (!rcx || !a60) { g.message |= kMsgEquip; return; }
        std::uintptr_t r10 = P(rcx + 0x2960);
        Wr<std::uint32_t>(r10 + 0x240, page);
        r10 = P(rcx + 0x2958);
        Wr<std::uint32_t>(r10 + 0x274, idx);
        const std::uintptr_t rdi = a60 - 0x80;
        const std::uintptr_t rbx = P(a60 + 0x470);
        const std::uintptr_t rsi = rbx - 0x5E0;
        Wr<std::uint64_t>(rdi + 0x2A60, rdi + 0x2A28);
        Wr<std::uint64_t>(rdi + 0x2A30, rcx);
        Wr<std::uint64_t>(rdi + 0x2A28, kEquipCmd);
        c->rax = kVtEquipApply;
        c->rbx = rbx;
        c->rcx = rcx;
        c->rdx = idx;          // div ebx 之后 edx = 余数（高 32 位清零）
        c->rdi = rdi;
        c->rsi = rsi;
        c->r10 = r10;
        return;
    }

    // ---------------- 重开 ----------------
    if (g.restart != 1) return;
    const std::uintptr_t ui = Ui();
    if (!ui) { CantQ(); return; }
    const std::uintptr_t load = P(ui + 0x20);
    if (Rd<std::uint32_t>(load + 0x1D04) != 0) { CantQ(); return; }   // 正在读盘
    const std::uintptr_t q = P(pQuest);
    if (!q) { CantQ(); return; }
    const std::uint32_t st = Rd<std::uint32_t>(q + 0x38);

    if (st == 4) {                                                     // skipR：结算画面
        if (Rd<std::uint32_t>(q + 0x58) != 4) { CantQ(); return; }
        Wr<float>(q + 0x131A0, 3601.0f);                               // 把结算计时拨到头
        return;                                                        // restart 保持 1，回据点后接着走
    }
    if (st == 5) {                                                     // skipR2
        if (Rd<std::uint32_t>(q + 0x58) != 4) { CantQ(); return; }
        Wr<std::uint32_t>(q + 0x38, 7);
        g.restart = 0;
        return;
    }
    // 在据点 / 调查：只重置没有可重置的任务，直接收工（不然会顺着去接任务）
    if (st == 1 || st == 0xD) {
        if (g.optResetOnly) {
            g.optResetOnly = 0;
            g.restart = 0;
            g.message |= kMsgNoQuest;
            return;
        }
        g.restart = 3;
        return;
    }
    g.restart = 3;
    if (st != 2 || Rd<std::uint32_t>(q + 0x54) != 2) { CantQ(); return; }   // 不在任务里

    // optioncheck（CT：Restart+6 = 这一次只重置，用过就清掉）
    const bool resetOnly = g.optResetOnly != 0;
    g.optResetOnly = 0;
    g.restart = resetOnly ? 0 : 3;   // 只重置：触发游戏的复位之后就收工，不接任务、不出发
    if (!resetOnly && !(g.optAcceptOnly == 1 || g.optReset == 1)) g.restart = 2;   // 直接重开：拦住回据点

    // ifquest：死了的话先补满血、把动作清零，免得重置卡在倒地状态
    const std::uintptr_t pl = P(P(pPlayerA) + 0x108);
    const std::uintptr_t hp = pl ? P(pl + 0x7670) : 0;
    if (hp && Rd<std::uint32_t>(hp + 0x64) == 0) {
        Wr<float>(hp + 0x64, 100.0f);
        Wr<std::uint32_t>(pl + 0x62B8, 0);
    }

    // resetmain：借这次调用触发游戏自己的重置函数
    const std::uintptr_t a = P(ui + 0xA8);
    if (!a) { CantQ(); return; }
    const std::uintptr_t rdi = a - 0xB0;
    const std::uintptr_t rsi = P(a + 0x260);
    if (!rsi) { CantQ(); return; }
    Wr<std::uint64_t>(rsi + 0x2478, kResetCmd);
    Wr<std::uint64_t>(rsi + 0x2480, rsi + 0x1850);
    Wr<std::uint64_t>(rsi + 0x24B0, rsi + 0x2478);
    Wr<std::uint64_t>(rdi + 0x2AD0, kResetCmd2);
    Wr<std::uint32_t>(rdi + 0x1DB4, 2);
    Wr<std::uint8_t>(rdi + 0x2348, 0xC9);
    Wr<float>(rsi + 0x2410, 10.0f);
    Wr<std::uint32_t>(rsi + 0x2888, 0xA);
    c->rax = kVtReset;
    c->rbx = rsi + 0x2270;
    c->rdi = rdi;
    c->rsi = rsi;
}

// ---------------------------------------------------------------------------
//  QuestLoad：原指令 test dword [rcx+2298],80000011（rcx = 任务柜台的 UI 对象）
//    restart==3 → 接任务（任务 ID 无效就用上一次的）
//    restart==4 → 出发
// ---------------------------------------------------------------------------
void Stop(Ctx* c, std::uintptr_t rax)
{
    g.message |= kMsgQuest;
    g.restart = 0;
    c->rax = rax;
}

void OnQuestLoad(Ctx* c)
{
    ActiveCall call;
    if (!call) return;
    const std::uintptr_t o = c->rcx;
    const std::uintptr_t ui = Ui();

    if (g.restart == 4) {                                              // goquest
        const std::uintptr_t r = ui ? P(ui + 0x3B8) : 0;
        if (!r) return;
        c->rax = r;
        if (Rd<std::uint8_t>(o + 0x2298) == 0xEA) { Stop(c, r); return; }
        if (Rd<std::uint32_t>(r + 0x28F0) == 0) return;
        Wr<std::uint32_t>(r + 0x28F0, 8);                              // 出发
        const std::uintptr_t q = P(pQuest);
        c->rbx = q;
        const std::uint32_t stage = Rd<std::uint32_t>(q + 0x291B0);
        // 普通地图（101~109）默认营地起始；勾了翼龙起始就不改
        if (stage > 100 && stage <= 109 && g.optWyvern != 1) Wr<std::uint32_t>(r + 0x28F8, 1);
        g.restart = 0;
        return;
    }
    if (g.restart != 3 || !ui) return;

    const std::uintptr_t load = P(ui + 0x20);
    c->rax = load;
    if (Rd<std::uint32_t>(load + 0x1D04) != 0) return;                // 还在读盘
    const std::uintptr_t pb = P(P(pPlayerB) + 0x108);
    const std::uintptr_t q = P(pQuest);
    c->rbx = pb;
    c->rax = q;
    if (g.optReset | g.optAcceptOnly) {                                // word [Restart+4] != 0
        const std::uint32_t st = Rd<std::uint32_t>(q + 0x38);
        if (st == 1) { if (Rd<std::uint32_t>(pb + 0x1138) != 3) return; }      // 据点：等角色站稳
        else if (st == 0xD) { if (Rd<std::uint32_t>(pb + 0x10F0) != 3) return; } // 调查：同上
        else return;
    }
    // checkaccept
    const bool acceptOnly = g.optAcceptOnly == 1;
    g.restart = acceptOnly ? 0 : 4;

    // accept
    const std::uintptr_t rbx = P(ui + 0x3C8);
    c->rbx = rbx;
    std::uint32_t qid = g.questId;
    if (!gamedata::ValidQuestId((int)qid)) {                           // Z：用上一次的任务
        const std::uintptr_t last = P(ui + 0x3B8);
        c->rax = last;
        if (Rd<std::uint32_t>(last + 0x2B94) == 0xFFFFFFFF) { Stop(c, last); return; }
        qid = Rd<std::uint32_t>(last + 0x2B94);
        g.questId = qid;
    }
    // Y
    Wr<std::uint32_t>(rbx + 0x292C, qid);
    Wr<std::uint64_t>(o + 0x2938, rbx);
    Wr<std::uint32_t>(o + 0x2940, 0xFFFFFFFF);
    Wr<std::uint64_t>(o + 0x2968, o + 0x2930);
    Wr<float>(o + 0x2AC8, 1.0f);
    Wr<std::uint64_t>(o + 0x2930, kQuestAcceptCmd);
    Wr<std::uint8_t>(o + 0x2298, 0xC9);
    Wr<std::uint32_t>(o + 0x1D04, 2);
    c->rax = kQuestAcceptCmd;
}

// ---------------------------------------------------------------------------
//  安装
// ---------------------------------------------------------------------------
bool BytesAre(std::uintptr_t a, const char* hex)
{
    std::uint8_t want[32];
    std::size_t n = 0;
    for (const char* p = hex; *p && n < sizeof(want);) {
        if (*p == ' ') { ++p; continue; }
        want[n++] = (std::uint8_t)std::strtoul(std::string(p, 2).c_str(), nullptr, 16);
        p += 2;
    }
    return Readable(a, n) && std::memcmp(reinterpret_cast<const void*>(a), want, n) == 0;
}

std::string Hex(std::uintptr_t a, std::size_t n)
{
    if (!Readable(a, n)) return i18n::Tr("(读不到)", "(unreadable)", "(読めません)");
    std::string s;
    char b[4];
    for (std::size_t i = 0; i < n; ++i) {
        std::snprintf(b, sizeof(b), "%02X ", reinterpret_cast<const std::uint8_t*>(a)[i]);
        s += b;
    }
    return s;
}

template <class T> bool PatchValue(std::uintptr_t a, T expect, T to, const char* what, std::string& rep)
{
    T cur{};
    std::memcpy(&cur, reinterpret_cast<const void*>(a), sizeof(T));
    if (std::memcmp(&cur, &expect, sizeof(T)) != 0) {
        Log("skip %s: 当前值不是原值（%s）", what, Hex(a, sizeof(T)).c_str());
        rep += std::string("·") + what + "没改（被改过或版本不同）\n";
        return false;
    }
    hook::WriteMem(a, &to, sizeof(T));
    Log("patched %s", what);
    return true;
}

} // namespace

bool Ready() { return g_ready; }
bool Enabled() { return g_enabled.load(std::memory_order_acquire); }

int TakeEnteredQuest() { return g_enteredQuest.exchange(0); }

bool Install(const Config& cfg, std::string& report)
{
    report.clear();
    std::uintptr_t tb = 0;
    std::size_t tl = 0;
    if (!hook::TextRange(EXE, tb, tl)) { report = "读不到游戏的代码段"; return false; }

    // Steam 的壳要等游戏自己把代码解开。等到固定点位的原始字节出现（最多 3 分钟）。
    struct Fixed { std::uintptr_t at; const char* bytes; const char* name; };
    const Fixed fixed[] = {
        {kBaseLoad,   "E8 2C 71 7A FF", "BaseLoad"},
        {kBaseLoad2,  "E8 3B A2 F5 FF", "BaseLoad2"},
        {kQuickLoad,  "E8 B6 DA FF FF", "QuickLoad"},
        {kMantleLoad, "40 53 48 83 EC 40 48 8B D9 E8 82 9D", "MantleLoad"},
    };
    const ULONGLONG t0 = ::GetTickCount64();
    for (;;) {
        int ok = 0;
        for (const auto& f : fixed) ok += BytesAre(f.at, f.bytes);
        if (ok > 0) break;
        if (::GetTickCount64() - t0 > 180000) break;
        ::Sleep(200);
    }
    ::Sleep(1500);   // 让同时启动的别的模组先打完它们的补丁，再看现场
    Log("代码段就绪（等了 %llu ms）", (unsigned long long)(::GetTickCount64() - t0));

    if (!hook::InitArena(tb, tb + tl)) { report = "在游戏模块附近要不到内存"; return false; }
    Log("arena = %p", (void*)hook::ArenaBase());

    bool core = true;
    auto need = [&](bool ok, const char* name, const std::string& why) {
        if (ok) { Log("hooked %s", name); return; }
        core = false;
        Log("FAILED %s: %s", name, why.c_str());
        report += std::string("·") + name + "：" + why + "\n";
    };

    // 特征码：必须恰好一处
    const auto h1 = hook::FindAll(tb, tl, kAobApplyResetAndEquip);
    const auto h2 = hook::FindAll(tb, tl, kAobQuestLoad);
    Log("AOB ApplyResetAndEquip: %zu 处%s", h1.size(), h1.size() == 1 ? "" : "（必须恰好 1 处）");
    Log("AOB QuestLoad: %zu 处", h2.size());

    // 固定点位
    bool fixedOk[4];
    for (int i = 0; i < 4; ++i) {
        fixedOk[i] = BytesAre(fixed[i].at, fixed[i].bytes);
        if (!fixedOk[i]) Log("%s 现场字节不对：%s", fixed[i].name, Hex(fixed[i].at, 12).c_str());
    }

    // 核心钩子要么全装，要么一个都不装 —— 装一半比不装更糟（会卡在重置中）
    if (h1.size() == 1 && h2.size() == 1 && fixedOk[0] && fixedOk[1] && fixedOk[2]) {
        static const std::uint8_t d1[] = {0x48, 0x8B, 0xCB, 0xFF, 0x50, 0x70};
        static const std::uint8_t d2[] = {0xF7, 0x81, 0x98, 0x22, 0x00, 0x00, 0x11, 0x00, 0x00, 0x80};
        need(hook::InstallCtxHook(h1[0], 6, OnApplyResetAndEquip, d1, sizeof(d1)), "ApplyResetAndEquip", "装钩子失败");
        need(hook::InstallCtxHook(h2[0], 10, OnQuestLoad, d2, sizeof(d2)), "QuestLoad", "装钩子失败");
        need(hook::InstallCallGate(kBaseLoad, &g.restart, 2, -1, &g_gateEnabled, &g_gateInflight),
             "BaseLoad", "装钩子失败");
        need(hook::InstallCallGate(kBaseLoad2, &g.restart, 2, 3, &g_gateEnabled, &g_gateInflight),
             "BaseLoad2", "装钩子失败");
        need(hook::InstallCallGate(kQuickLoad, &g.restart, 2, -1, &g_gateEnabled, &g_gateInflight),
             "QuickLoad", "装钩子失败");
    } else {
        core = false;
        report += "·重开用到的代码和预期对不上：游戏版本不同，或者被别的模组改过"
                  "（同时开着 IN-Q.exe 也会这样）\n";
        if (h1.size() != 1) report += "  ApplyResetAndEquip 特征码找到 " + std::to_string(h1.size()) + " 处\n";
        if (h2.size() != 1) report += "  QuestLoad 特征码找到 " + std::to_string(h2.size()) + " 处\n";
        for (int i = 0; i < 3; ++i)
            if (!fixedOk[i]) report += std::string("  ") + fixed[i].name + " 被改过\n";
    }

    // 黑龙开局脱衣装的修复：函数开头，rsi==0 时直接返回。这是修 bug，一直开着
    {
        if (fixedOk[3]) {
            hook::Emitter e;
            e.b({0x48, 0x85, 0xF6, 0x75, 0x01, 0xC3})       // test rsi,rsi ; jnz +1 ; ret
             .b({0x40, 0x53, 0x48, 0x83, 0xEC, 0x40})       // push rbx ; sub rsp,40
             .jmpAbs(kMantleLoad + 6);
            const std::uintptr_t s = hook::Commit(e);
            if (s && hook::PatchJump(kMantleLoad, 6, s)) Log("hooked MantleLoad");
            else report += "·衣装修复没装上\n";
        } else {
            report += "·衣装修复没装（那里被别的模组改过，比如 MantleRemover）\n";
        }
    }

    // 缩短转场黑屏 / 快速重置
    if (core) PatchValue<std::uint32_t>(kQuickReset, 4, 5, "快速重置", report);
    if (cfg.quickBlackout) {
        PatchValue<float>(kBlackOut1, 1.4f, 0.1f, "转场黑屏1", report);
        PatchValue<float>(kBlackOut2, 0.5f, 0.1f, "转场黑屏2", report);
    }

    ApplyOptions(cfg);
    g_ready = core;
    return core;
}

// 面板上「现在到底是哪份」按这套算：套装自己指定了就用套装的，否则用全局的。
// 界面和实际写入都用这个，避免出现「显示的是 A，吃进去的是 B」。
void ApplyOptions(const Config& cfg)
{
    // 禁用先关总闸并排空回调；启用则在所有选项发布完整之后最后开闸。
    if (!cfg.enabled) SetEnabled(false);
    g.optReset = cfg.reset ? 1 : 0;
    g.optAcceptOnly = cfg.acceptOnly ? 1 : 0;
    g.optWyvern = cfg.wyvern ? 1 : 0;
    g.questId = (std::uint32_t)cfg.questId;   // 0 = 用上一次的任务（QuestLoad 里会去读）
    g.food = cfg.food;
    // 当前套用的这套本身带了猫饭/任务的话，生效值就是它的那份（面板显示的也是这个）
    if (const LoadoutSet* s = cfg.FindSetById(cfg.activeSet)) {
        if (s->food > 0) g.food = s->food;
        if (gamedata::ValidQuestId(s->quest)) g.questId = (std::uint32_t)s->quest;
    }
    if (cfg.enabled) SetEnabled(true);
}

// ---------------------------------------------------------------------------
//  套装名字 → 编号
//
//  装备套装：每格 0x2B0，第 n 格的武器字段在 存档+10CCF4+(n-1)*2B0（CT 用它判空）。
//  名字字段的位置 CT 没用到，先按「256 字节名字紧挨在武器字段前面」找，
//  找不到再在整张表里搜一遍，把实际位置写进日志 —— 第一次实机跑完就能定下来。
// ---------------------------------------------------------------------------
namespace {
bool IsNumber(const std::string& s, int& v)
{
    if (s.empty()) return false;
    for (char ch : s) if (ch < '0' || ch > '9') return false;
    v = std::atoi(s.c_str());
    return true;
}

} // namespace

// 配置里存的就是编号：装备是「游戏里第几套」（1..224），道具是 "#存放格" 或显示编号。
// 不是编号的一律当没配 —— 旧格式（0.7.1 及以前按名字存）不再解析。
int ResolveEquip(const std::string& s)
{
    int v = 0;
    if (IsNumber(s, v)) return (v >= 1 && v <= 224) ? v : 0;
    if (!s.empty()) Log("equip '%s' 不是编号，忽略（旧配置不再按名字解析）", s.c_str());
    return 0;
}

// ---------------------------------------------------------------------------
//  道具套装有两套编号（实机确认）：
//    存放格 s（1..80）：数据在存档里的位置。名字在 存档 + s*768 - 28，
//                       CT 的「是否有效」字段在 存档 + s*768 + 278。
//    显示编号 d（1..80）：游戏列表里看到的顺序，玩家排过序就和存放格不一样。
//                       套用命令要的是显示编号。
//  两者由 存档+37FF8 的 80 字节表连起来：表[d-1] = s-1。
//  老 IN-Q 把按名字查到的存放格直接当显示编号用，排过序的人就会套错（用户实测：
//  选「米拉2」（存放 5 / 显示 20）结果套上了显示第 5 号「重弩」）。
// ---------------------------------------------------------------------------
std::uintptr_t ItemNameAt(std::uintptr_t slot, int s) { return slot + std::uintptr_t(s) * 0x768 - 0x28; }
constexpr std::uintptr_t kItemOrder = 0x37FF8;

// 读顺序表；不是 0..79 的排列就当没排过序（显示编号 = 存放格）
bool ItemOrder(std::uintptr_t slot, std::uint8_t order[80])
{
    if (!Readable(slot + kItemOrder, 80)) return false;
    std::memcpy(order, reinterpret_cast<const void*>(slot + kItemOrder), 80);
    bool seen[80] = {};
    for (int i = 0; i < 80; ++i) {
        if (order[i] >= 80 || seen[order[i]]) {
            for (int j = 0; j < 80; ++j) order[j] = (std::uint8_t)j;
            Log("道具套装顺序表不是排列，按未排序处理");
            return false;
        }
        seen[order[i]] = true;
    }
    return true;
}

int ItemStoreOfDisplay(std::uintptr_t slot, int d)
{
    std::uint8_t order[80];
    ItemOrder(slot, order);
    return (d >= 1 && d <= 80) ? order[d - 1] + 1 : 0;
}

// 返回显示编号（套用命令要的就是它）。"#存放格" 是按玩家排过序也不会错的写法；
// 数字按游戏里看到的编号理解；名字先找存放格再换算。
int ResolveItem(const std::string& s)
{
    int v = 0;
    if (!s.empty() && s[0] == '#') {
        const int st = std::atoi(s.c_str() + 1);
        const std::uintptr_t slot = SaveSlot();
        if (st < 1 || st > 80 || !slot) return 0;
        std::uint8_t order[80];
        ItemOrder(slot, order);
        for (int d = 1; d <= 80; ++d) {
            if (order[d - 1] + 1 == st) return d;
        }
        return 0;
    }
    if (IsNumber(s, v)) return (v >= 1 && v <= 80) ? v : 0;
    Log("item '%s' 不是编号，忽略（旧配置不再按名字解析）", s.c_str());
    return 0;
}

// ---------------------------------------------------------------------------
//  快捷键
// ---------------------------------------------------------------------------
namespace {
int g_pendingSetItem = 0;
bool g_pendingLink = false;
bool g_hasPendingLink = false;
int g_savedRadial = -1;                 // CT 的 wassetting：套道具前「联动快捷栏」原来的值
std::uintptr_t g_savedRadialSlot = 0;    // 必须恢复到当时改过的同一个存档槽

void RestoreRadial()
{
    if (g_savedRadial == -1) return;
    if (g_savedRadialSlot)
        Wr<std::uint8_t>(g_savedRadialSlot + 0x140415, (std::uint8_t)g_savedRadial);
    g_savedRadial = -1;
    g_savedRadialSlot = 0;
}
}

void CancelPending()
{
    // 先由 SetEnabled(false) 阻止新回调，再等待已进入的回调退出，最后统一清空。
    ::InterlockedExchange(reinterpret_cast<volatile LONG*>(&g.restart), 0);
    g.optResetOnly = 0;
    g.equipLoadout = 0;
    g_pendingEquipApply.store(0, std::memory_order_release);
    g_pendingSetItem = 0;
    g_pendingLink = false;
    g_hasPendingLink = false;
    g.message = 0;
    g_enteredQuest.exchange(0, std::memory_order_acq_rel);
}

void SetEnabled(bool enabled)
{
    if (enabled) {
        if (g_enabled.load(std::memory_order_acquire)) return;
        // 卸载期间发生的任务变化不应在重新装载时倒灌进配置；以当前值作为新基线。
        const std::uintptr_t ui = game::Ui();
        const std::uintptr_t last = ui ? game::P(ui + 0x3B8) : 0;
        const int entered = last ? (int)game::Rd<std::uint32_t>(last + 0x2B94) : 0;
        g_enteredSeen = gamedata::ValidQuestId(entered) ? entered : 0;
        g_enteredQuest.store(0, std::memory_order_release);
        ::InterlockedExchange(reinterpret_cast<volatile LONG*>(&g_gateEnabled), 1);
        g_enabled.store(true, std::memory_order_release);
        return;
    }
    ::InterlockedExchange(reinterpret_cast<volatile LONG*>(&g_gateEnabled), 0);
    if (!g_enabled.exchange(false, std::memory_order_acq_rel)) {
        while (::InterlockedCompareExchange(reinterpret_cast<volatile LONG*>(&g_gateInflight), 0, 0) != 0)
            ::Sleep(0);
        RestoreRadial();
        CancelPending();
        return;
    }
    // 先关闭两个入口闸门，再等待 C++ hook 与三个汇编 call gate 全部退出。
    // call gate 的 inflight 覆盖比较、restart 写回和跳过决定，所以排空后再清零不会被复活。
    ::InterlockedExchange(reinterpret_cast<volatile LONG*>(&g.restart), 0);
    while (g_inflight.load(std::memory_order_acquire) != 0 ||
           ::InterlockedCompareExchange(reinterpret_cast<volatile LONG*>(&g_gateInflight), 0, 0) != 0)
        ::Sleep(0);
    // 套道具时临时改过的快捷栏也在发布 Dormant 前恢复，并绑定原存档槽。
    RestoreRadial();
    CancelPending();
}

// 把一套全套里的装备/道具/猫饭/任务排进待办。装备由 ApplyResetAndEquip 钩子在游戏线程里换，
// 道具由下一次 Tick 发 UI 命令，猫饭在下次读盘时写。
void QueueSet(const LoadoutSet& s)
{
    const int e = ResolveEquip(s.equip);
    const int it = ResolveItem(s.item);
    if (!s.equip.empty() && !e) Chat(i18n::Fmt(i18n::Tr("Better-IN-Q：装备套装「%s」找不到（旧版按名字存的请在套装页重选）",
                                                       "Better-IN-Q: loadout \"%s\" not found (old name-based configs need re-picking)",
                                                       "Better-IN-Q：装備セット「%s」が見つかりません（旧形式の設定は選び直してください）"), s.equip.c_str()));
    if (!s.item.empty() && !it) Chat(i18n::Fmt(i18n::Tr("Better-IN-Q：道具套装「%s」找不到（旧版按名字存的请在套装页重选）",
                                                       "Better-IN-Q: item loadout \"%s\" not found (old name-based configs need re-picking)",
                                                       "Better-IN-Q：アイテムセット「%s」が見つかりません（旧形式の設定は選び直してください）"), s.item.c_str()));
    if (e) g.equipLoadout = e;
    if (it) { g_pendingSetItem = it; g_pendingLink = s.linkRadial; g_hasPendingLink = true; }
    if (s.food > 0) g.food = s.food;
    if (gamedata::ValidQuestId(s.quest)) g.questId = (std::uint32_t)s.quest;
}

bool RequestEquipApply(const Config& cfg, const std::string& equip)
{
    // 和 SetEnabled(false) 共用 inflight 闸门：停用会等发布者退出后再清队列，
    // 不会出现检查 Enabled 后恰好进入 Dormant、随后又发布旧请求的竞态。
    ActiveCall call;
    if (!call || !cfg.enabled || !g_ready) return false;
    const int n = equip.empty() ? 0 : ResolveEquip(equip);
    g_pendingEquipApply.store(n, std::memory_order_release);
    return equip.empty() || n != 0;
}

bool RequestActiveSetEquipApply(const Config& cfg)
{
    const LoadoutSet* s = cfg.activeSet > 0 ? cfg.FindSetById(cfg.activeSet) : nullptr;
    return RequestEquipApply(cfg, s ? s->equip : std::string());
}

void RequestRestart(const Config& cfg)
{
    if (!cfg.enabled || !Enabled()) return;
    if (!g_ready) {
        Chat(i18n::Tr("Better-IN-Q：重开功能没装上，详情见 BetterINQ.log",
                      "Better-IN-Q: the restart hook did not install, see BetterINQ.log",
                      "Better-IN-Q：再開機能を組み込めませんでした。BetterINQ.log を見てください"));
        return;
    }
    if (g.restart != 0) return;
    // 重开将按当前 Active 配置排队，不能在重开完后再补套旧的面板选择。
    g_pendingEquipApply.store(0, std::memory_order_release);
    if (cfg.activeSet > 0) {
        if (const LoadoutSet* s = cfg.FindSetById(cfg.activeSet)) QueueSet(*s);
        else Chat(i18n::Fmt(i18n::Tr("Better-IN-Q：配置里没有编号 %d 的全套",
                                     "Better-IN-Q: no loadout #%d in the config",
                                     "Better-IN-Q：設定にセット番号 %d がありません"), cfg.activeSet));
    }
    g.restart = 1;
}

// 只重置（原 IN-Q 的 Reset only，CT 里是 [Restart+6]）：把当前任务重置掉、回据点，
// 不接任务也不出发。不套用全套配置 —— 这一趟只是把任务清掉。
void RequestResetOnly()
{
    if (!Enabled()) return;
    if (!g_ready) {
        Chat(i18n::Tr("Better-IN-Q：重开功能没装上，详情见 BetterINQ.log",
                      "Better-IN-Q: the restart hook did not install, see BetterINQ.log",
                      "Better-IN-Q：再開機能を組み込めませんでした。BetterINQ.log を見てください"));
        return;
    }
    if (g.restart != 0) return;
    g.optResetOnly = 1;
    g.restart = 1;
}

std::vector<NamedLoadout> ListEquipLoadouts()
{
    std::vector<NamedLoadout> r;
    const std::uintptr_t slot = SaveSlot();
    if (!slot) return r;
    for (int n = 1; n <= 224; ++n) {
        const std::uintptr_t w = slot + 0x10CCF4 + std::uintptr_t(n - 1) * 0x2B0;
        if (!Readable(w - 0x100, 0x104) || Rd<std::uint32_t>(w) == 0xFFFFFFFF) continue;   // 空格子
        char name[0x101] = {};
        std::memcpy(name, reinterpret_cast<const void*>(w - 0x100), 0x100);
        r.push_back({n, name[0] ? std::string(name) : std::string(i18n::Tr("装备套装 ", "Equip loadout ", "装備セット ")) + std::to_string(n), n});
    }
    return r;
}

std::vector<NamedLoadout> ListItemLoadouts()
{
    std::vector<NamedLoadout> r;
    const std::uintptr_t slot = SaveSlot();
    if (!slot) return r;
    std::uint8_t order[80];
    ItemOrder(slot, order);
    for (int d = 1; d <= 80; ++d) {
        const int st = order[d - 1] + 1;
        if (Rd<std::uint32_t>(slot + std::uintptr_t(st) * 0x768 + 0x278) == 0) continue;   // 空套装
        const std::uintptr_t a = ItemNameAt(slot, st);
        char name[0x101] = {};
        if (Readable(a, 0x100)) std::memcpy(name, reinterpret_cast<const void*>(a), 0x100);
        r.push_back({d, name[0] ? std::string(name) : std::string(i18n::Tr("道具套装 ", "Item loadout ", "アイテムセット ")) + std::to_string(d), st});
    }
    return r;
}

// 原版 IN-Q 的 config.txt 里，装备/道具存的是名字（它自己也只会按名字找）；
// 我们存编号。导入之后用当前存档里的名单把名字换成编号，换不到的留空并报告，
// 绝不写一个看着像编号、其实指向别的套装的数字。
int NormalizeEquipItemNumbers(Config& cfg)
{
    int lost = 0;
    const int changed = ::MigrateLoadoutRefs(cfg, ListEquipLoadouts(), ListItemLoadouts(), &lost);
    if (changed || lost) Log("import: 装备/道具名字换编号，改了 %d 处，%d 处对不上已留空", changed, lost);
    return lost;
}

// 老配置一次性迁移：0.7.1 及以前面板把装备/道具套装按名字存进 ini，删掉读取兼容之后
// 这些值就没人认识了。进存档后（拿得到名单）换一次编号，调用方存回 profile 即可。
int MigrateLoadoutRefs(Config& cfg)
{
    if (!::NeedsLoadoutMigration(cfg)) return 0;
    const std::vector<NamedLoadout> eq = ListEquipLoadouts();
    const std::vector<NamedLoadout> it = ListItemLoadouts();
    if (eq.empty() && it.empty()) {
        Log("config: 还没读到存档里的套装列表，稍后再把老格式的套装引用换成编号");
        return 0;
    }
    int lost = 0;
    const int changed = ::MigrateLoadoutRefs(cfg, eq, it, &lost);
    if (changed)
        Log("config: 老格式的套装引用换成编号，改了 %d 处，%d 处对不上已留空（旧名字请在套装页重选）",
            changed, lost);
    return changed;
}

// ---------------------------------------------------------------------------
//  每 100ms（CT 的 Lua 定时器）
// ---------------------------------------------------------------------------
void Tick(const Config& cfg)
{
    const std::uintptr_t uib = P(pUiBase);
    if (!uib) return;
    const std::uintptr_t pui = P(uib + 0x278);
    if (!pui) return;
    const std::uintptr_t pload = P(pui + 0x20);
    const std::uintptr_t piui = P(pui + 0x170);
    const std::uintptr_t slot = SaveSlot();

    // 上一拍为套道具临时改的「联动快捷栏」，这一拍改回原存档槽。只动 1 个字节：
    // CT 按 4 字节读写这个奇数偏移，会把后面 3 个字节一起覆盖成 0。
    RestoreRadial();
    if (!cfg.enabled || !Enabled()) return;

    // 道具套装（通过 UI 命令让游戏自己套用）
    if (g_pendingSetItem > 0 && slot && piui) {
        const int n = g_pendingSetItem;                    // 显示编号（套用命令要它）
        g_pendingSetItem = 0;
        const int st = ItemStoreOfDisplay(slot, n);         // 数据真正在哪一格
        const std::uint32_t valid = st ? Rd<std::uint32_t>(slot + std::uintptr_t(st) * 0x768 + 0x278) : 0;
        if (valid != 0) {
            if (g_hasPendingLink) {
                g_savedRadial = Rd<std::uint8_t>(slot + 0x140415);
                g_savedRadialSlot = slot;
                Wr<std::uint8_t>(slot + 0x140415, g_pendingLink ? 1 : 2);
                g_hasPendingLink = false;
            }
            Wr<std::uint64_t>(uib + 0x4F60, kItemLoadoutCmd);
            Wr<std::uint64_t>(uib + 0x4F68, (std::uint64_t)(n - 1));
            Wr<std::uint64_t>(uib + 0x4F98, uib + 0x4F60);
            Wr<std::uint64_t>(uib + 0x4FA0, piui);
            Wr<std::uint64_t>(uib + 0x4FA8, uib + 0xF6B0);
            Wr<std::uint64_t>(uib + 0x4FB0, uib + 0x4FB8);
            Wr<std::uint64_t>(uib + 0x4FB8, piui);
            Wr<std::uint64_t>(uib + 0x4FC0, uib + 0x4FA0);
            Wr<std::uint64_t>(uib + 0x4FD0, piui);
            Wr<std::uint64_t>(uib + 0x4FD8, 1);
            Wr<std::uint64_t>(uib + 0x4FE0, kItemLoadoutCmd2);
            Wr<std::uint64_t>(uib + 0x5018, uib + 0x4FE0);
            Wr<std::uint64_t>(uib + 0x8F60, 1);
            Log("item loadout %d 已提交（联动快捷栏 %d）", n, g_savedRadial == -1 ? -1 : (g_pendingLink ? 1 : 2));
        } else {
            Chat(i18n::Fmt(i18n::Tr("Better-IN-Q：道具套装 %d 是空的",
                                    "Better-IN-Q: item loadout %d is empty",
                                    "Better-IN-Q：アイテムセット %d は空です"), n));
        }
    }

    // 猫饭：只在明确选中预设时于读盘期间写入（CT：pfood+19A4+i*4，i=1..8）。
    // 未选（0/-1）不覆盖；数值相同不能证明是我们写的，也可能是玩家手动吃的。
    if (pload && Rd<std::uint32_t>(pload + 0x1D04) != 0) {
        const std::uintptr_t pf = P(pFood);
        auto it = cfg.foods.find((int)g.food);
        if (pf && g.food > 0 && it != cfg.foods.end()) {
            const FoodPreset& f = it->second;
            const std::int32_t v[8] = {f.hp, f.stamina, f.attack, f.defense, f.elemRes,
                                       f.skill[0], f.skill[1], f.skill[2]};
            for (int i = 0; i < 8; ++i) Wr<std::int32_t>(pf + 0x19A8 + i * 4, v[i]);
        }
    }

    // 全局任务跟着「实际进去了的那一关」走。游戏把最后一次进的任务记在 pui+0x3B8 的 +0x2B94，
    // 我们重开的、玩家自己在柜台接的都会更新 —— 发现变了就交给主循环写进全局任务。
    // （不能只在「取消套用」那一刻改：那时玩家可能已经又打了别的关。）
    if (const std::uintptr_t last = P(pui + 0x3B8)) {
        const std::uint32_t entered = Rd<std::uint32_t>(last + 0x2B94);
        if (gamedata::ValidQuestId((int)entered) && (int)entered != g_enteredSeen) {
            g_enteredSeen = (int)entered;
            g_enteredQuest.store((int)entered);
        }
    }

    // 钩子里攒下的提示
    const std::uint32_t m = g.message;
    if (m) {
        g.message = 0;
        if (m & kMsgEquip) Chat(i18n::Tr("Better-IN-Q：这个装备套装是空的或无效",
                                         "Better-IN-Q: that loadout is empty or invalid",
                                         "Better-IN-Q：その装備セットは空か無効です"));
        if (m & kMsgQuest) Chat(i18n::Tr("Better-IN-Q：接任务失败（没选过任务？）",
                                         "Better-IN-Q: could not accept the quest (no quest picked yet?)",
                                         "Better-IN-Q：クエストを受けられませんでした（まだ選んでいませんか？）"));
        if (m & kMsgCant) Chat(i18n::Tr("Better-IN-Q：现在不能重开",
                                        "Better-IN-Q: cannot restart right now",
                                        "Better-IN-Q：今は再開できません"));
        if (m & kMsgWindow) Chat(i18n::Tr("Better-IN-Q：请先关闭所有窗口",
                                          "Better-IN-Q: close all windows first",
                                          "Better-IN-Q：先にすべてのウィンドウを閉じてください"));
        if (m & kMsgNoQuest) Chat(i18n::Tr("Better-IN-Q：现在不在任务里，没有可重置的",
                                           "Better-IN-Q: not in a quest, nothing to reset",
                                           "Better-IN-Q：クエスト中ではないのでリセットできません"));
    }
}

} // namespace inq
