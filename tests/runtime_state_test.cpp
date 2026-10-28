#include <cstdio>

#include "../src/runtime_state.h"
#include "../src/startup_gate.h"

static int g_fail = 0;
#define CHECK(cond, ...) do { if (cond) std::printf("  ok   " __VA_ARGS__); \
    else { std::printf("  FAIL " __VA_ARGS__); ++g_fail; } std::printf("\n"); } while (0)

int main()
{
    using runtime::Mode;

    const runtime::TransitionPlan off = runtime::PlanTransition(Mode::Active, Mode::Dormant);
    CHECK(off.cancelInqPending, "Active -> Dormant 会取消尚未提交的 IN-Q 动作");
    CHECK(off.closePanel && off.unblockInputs, "Active -> Dormant 会关面板并放开输入");
    CHECK(!off.enableInq, "Active -> Dormant 会关闭 IN-Q 执行总闸");

    const runtime::TransitionPlan on = runtime::PlanTransition(Mode::Dormant, Mode::Active);
    CHECK(on.enableInq, "Dormant -> Active 只重新打开执行总闸");
    CHECK(!on.cancelInqPending && !on.closePanel && !on.unblockInputs,
          "Dormant -> Active 不重复安装、不制造额外副作用");

    const runtime::TransitionPlan same = runtime::PlanTransition(Mode::Active, Mode::Active);
    CHECK(!same.cancelInqPending && !same.closePanel && !same.unblockInputs && !same.enableInq,
          "状态没变时不执行过渡动作");

    const runtime::HotkeyActions dormant = runtime::GateHotkeys(Mode::Dormant, true, true, true);
    CHECK(dormant.togglePanel, "Dormant 仍允许面板快捷键（用于恢复装载态）");
    CHECK(!dormant.restart && !dormant.resetOnly, "Dormant 屏蔽重开和只重置快捷键");

    const runtime::HotkeyActions active = runtime::GateHotkeys(Mode::Active, true, true, true);
    CHECK(active.togglePanel && active.restart && active.resetOnly, "Active 放行全部快捷键动作");

    using startup::Gate;
    using PA = Gate::PanelAction;
    Gate gate;
    gate.Sample(false, 0);
    CHECK(!gate.AllowsActions() && gate.Panel(true, false) == PA::None,
          "startup/title edges cannot install or queue a panel");
    gate.Sample(true, 10);
    gate.Sample(true, 509);
    CHECK(!gate.AllowsActions() && gate.Panel(true, false) == PA::None,
          "scene must remain stable for the full interval");
    gate.Sample(true, 510);
    CHECK(gate.SceneReady() && !gate.AllowsActions() && gate.Panel(true, false) == PA::None,
          "edge on the ready boundary is discarded too");
    gate.Sample(true, 526);
    CHECK(gate.AllowsActions() && gate.Panel(false, false) == PA::None,
          "an early held key has no fresh edge after readiness");
    CHECK(gate.Panel(true, true) == PA::Unavailable,
          "disabled overlay never attempts installation");
    CHECK(gate.Panel(true, false) == PA::Install, "first fresh ready edge attempts installation");
    gate.CompleteInstall(false);
    CHECK(gate.Panel(true, false) == PA::Unavailable,
          "partial/failed installation is never retried");
    gate.Sample(false, 600);
    CHECK(!gate.SceneReady() && !gate.AllowsActions() && gate.Panel(true, false) == PA::None,
          "losing the scene closes every shortcut gate immediately");
    gate.Sample(true, 700);
    gate.Sample(true, 1200);
    CHECK(!gate.AllowsActions(), "restored scene also suppresses its ready boundary");
    gate.Sample(true, 1216);
    CHECK(gate.Panel(true, false) == PA::Unavailable, "scene recovery does not retry failed installation");

    Gate installed;
    installed.Sample(true, 0);
    installed.Sample(true, Gate::kStableMs);
    installed.Sample(true, Gate::kStableMs + 16);
    CHECK(installed.Panel(true, false) == PA::Install, "ready first press installs once");
    installed.CompleteInstall(true);
    CHECK(installed.Panel(true, false) == PA::Toggle, "installed panel toggles without reinstalling");
    installed.Sample(false, 600);
    CHECK(installed.Panel(true, false) == PA::None, "installed panel cannot reopen on title/loading");
    installed.Sample(true, 700);
    installed.Sample(true, 1200);
    installed.Sample(true, 1216);
    CHECK(installed.Panel(true, false) == PA::Toggle, "return to scene reuses installed hooks");
    const auto gated = runtime::GateHotkeys(Mode::Active, false, gate.AllowsActions(), gate.AllowsActions());
    CHECK(gated.restart && gated.resetOnly, "stable recovered scene permits business edges");
    gate.Sample(false, 1300);
    const auto blocked = runtime::GateHotkeys(Mode::Active, false, gate.AllowsActions(), gate.AllowsActions());
    CHECK(!blocked.restart && !blocked.resetOnly, "title/loading blocks restart and reset edges");

    std::printf(g_fail ? "\n%d 项失败\n" : "\n全部通过（0 项失败）\n", g_fail);
    return g_fail ? 1 : 0;
}
