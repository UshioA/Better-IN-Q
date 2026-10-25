#pragma once

// DLL 常驻的逻辑装载态。Dormant 只让所有游戏侧行为旁路；钩子和虚表不摘，
// 因为当前安装器没有足够的 patch ledger，也不能安全 FreeLibrary。
namespace runtime {

enum class Mode { Dormant, Active };

struct TransitionPlan {
    bool cancelInqPending = false;
    bool closePanel = false;
    bool unblockInputs = false;
    bool enableInq = false;
};

inline TransitionPlan PlanTransition(Mode from, Mode to)
{
    TransitionPlan p;
    if (from == to) return p;
    p.enableInq = to == Mode::Active;
    if (to == Mode::Dormant) {
        p.cancelInqPending = true;
        p.closePanel = true;
        p.unblockInputs = true;
    }
    return p;
}

struct HotkeyActions {
    bool togglePanel = false;
    bool restart = false;
    bool resetOnly = false;
};

inline HotkeyActions GateHotkeys(Mode mode, bool panel, bool restart, bool resetOnly)
{
    HotkeyActions a;
    // 面板是从卸载态恢复功能的入口，所以 Dormant 也必须能打开。
    a.togglePanel = panel;
    if (mode == Mode::Active) {
        a.restart = restart;
        a.resetOnly = resetOnly;
    }
    return a;
}

} // namespace runtime
