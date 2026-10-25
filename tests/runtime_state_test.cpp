#include <cstdio>

#include "../src/runtime_state.h"

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

    std::printf(g_fail ? "\n%d 项失败\n" : "\n全部通过（0 项失败）\n", g_fail);
    return g_fail ? 1 : 0;
}
