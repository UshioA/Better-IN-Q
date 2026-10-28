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
    gate.Sample(false, false, 0);
    CHECK(!gate.BeginInstall(false) && gate.Panel(true, false) == PA::None,
          "early startup cannot install or queue a panel");
    gate.Sample(true, false, 10);
    gate.Sample(true, false, 509);
    CHECK(!gate.StartupReady(), "title must be stable for full interval");
    gate.Sample(true, false, 510);
    CHECK(gate.StartupReady() && gate.Panel(true, false) == PA::None,
          "readiness boundary consumes the early edge");
    CHECK(!gate.BeginInstall(true), "disabled overlay never installs");
    CHECK(gate.BeginInstall(false), "title readiness installs without a key or save");
    gate.CompleteInstall(true);
    gate.Sample(true, false, 526);
    CHECK(gate.Panel(false, false) == PA::None, "early held shortcut is not replayed");
    CHECK(gate.Panel(true, false) == PA::Toggle, "fresh title shortcut opens panel");
    CHECK(!gate.AllowsActions(), "title panel cannot invoke business shortcuts");
    gate.Sample(false, false, 600);
    CHECK(gate.StartupReady() && gate.Panel(true, false) == PA::Toggle,
          "transition blackout preserves panel availability");
    CHECK(!gate.BeginInstall(false), "transition never reinstalls graphics hooks");
    gate.Sample(false, true, 700);
    gate.Sample(false, true, 1200);
    CHECK(!gate.AllowsActions(), "business scene readiness boundary remains suppressed");
    gate.Sample(false, true, 1216);
    CHECK(gate.AllowsActions(), "stable character scene permits business shortcuts");
    gate.Sample(false, false, 1300);
    CHECK(!gate.AllowsActions() && gate.Panel(true, false) == PA::Toggle,
          "loading blocks game writes but not the panel");
    gate.Sample(true, false, 1400);
    CHECK(gate.Panel(true, false) == PA::Toggle && !gate.BeginInstall(false),
          "return to title preserves panel and never reinstalls");
    Gate failed;
    failed.Sample(true, false, 0);
    failed.Sample(true, false, 500);
    CHECK(failed.BeginInstall(false), "failure path attempts once");
    failed.CompleteInstall(false);
    failed.Sample(false, false, 516);
    CHECK(!failed.BeginInstall(false) && failed.Panel(true, false) == PA::Unavailable,
          "partial failed installation is never retried even during loading");
    Gate fallback;
    fallback.Sample(false, true, 0);
    fallback.Sample(false, true, 500);
    CHECK(fallback.BeginInstall(false), "loaded scene fallback handles skipped title");
    fallback.CompleteInstall(true);
    fallback.Sample(false, false, 516);
    CHECK(fallback.Panel(true, false) == PA::Toggle, "fallback also latches across loading");
    Gate unstable;
    unstable.Sample(true, false, 0);
    unstable.Sample(false, false, 499);
    unstable.Sample(true, false, 500);
    unstable.Sample(true, false, 999);
    CHECK(!unstable.BeginInstall(false), "interrupted initial title sample resets timer");

    std::printf(g_fail ? "\n%d 项失败\n" : "\n全部通过（0 项失败）\n", g_fail);
    return g_fail ? 1 : 0;
}
