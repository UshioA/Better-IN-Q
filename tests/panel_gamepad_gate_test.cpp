#include "../src/panel_gamepad_gate.h"
#include <cstdio>

static int failures = 0;
#define CHECK(condition, label) do { if (!(condition)) { std::printf("FAIL: %s\n", label); ++failures; } } while (0)

// Apply the key portion of the event queue just as NewFrame would; intentionally
// retain selected events in the repeat test to exercise trickled queue safety.
static void Consume(ImGuiContext& ctx)
{
    for (const ImGuiInputEvent& e : ctx.InputEventsQueue) {
        if (e.Type != ImGuiInputEventType_Key) continue;
        ImGuiKeyData* data = ImGui::GetKeyData(&ctx, (ImGuiKey)e.Key.Key);
        data->Down = e.Key.Down;
        data->AnalogValue = e.Key.AnalogValue;
    }
    ctx.InputEventsQueue.clear();
}
int main()
{
    ImGuiContext* ctx = ImGui::CreateContext();
    ImGuiIO& io = ImGui::GetIO();
    overlay::PanelGamepadReleaseGate gate;
    gate.Arm();
    io.AddKeyEvent(ImGuiKey_GamepadFaceDown, true);
    io.AddKeyEvent(ImGuiKey_GamepadFaceRight, true);
    io.AddKeyEvent(ImGuiKey_Space, true);
    gate.Filter(*ctx);
    Consume(*ctx);
    CHECK(!ImGui::GetKeyData(ImGuiKey_GamepadFaceDown)->Down, "opening A suppressed");
    CHECK(!ImGui::GetKeyData(ImGuiKey_GamepadFaceRight)->Down, "opening B suppressed");
    CHECK(ImGui::GetKeyData(ImGuiKey_Space)->Down, "keyboard untouched");
    io.AddKeyEvent(ImGuiKey_GamepadFaceDown, true);
    io.AddKeyEvent(ImGuiKey_GamepadFaceRight, true);
    gate.Filter(*ctx);
    // Leave a rewritten event queued to simulate ImGui input trickling.
    io.AddKeyEvent(ImGuiKey_GamepadFaceDown, true);
    io.AddKeyEvent(ImGuiKey_GamepadFaceRight, true);
    gate.Filter(*ctx);
    Consume(*ctx);
    CHECK(!ImGui::GetKeyData(ImGuiKey_GamepadFaceDown)->Down, "held A remains suppressed across frames");
    io.AddKeyEvent(ImGuiKey_GamepadFaceDown, true);
    io.AddKeyEvent(ImGuiKey_GamepadFaceRight, true);
    gate.Filter(*ctx); Consume(*ctx);
    CHECK(!ImGui::GetKeyData(ImGuiKey_GamepadFaceDown)->Down, "trickled synthetic release did not unblock A");
    // Release A: AddKeyEvent(false) is deduplicated because the filtered state
    // already is false. B remains physically held and produces another event.
    io.AddKeyEvent(ImGuiKey_GamepadFaceDown, false);
    io.AddKeyEvent(ImGuiKey_GamepadFaceRight, true);
    gate.Filter(*ctx); Consume(*ctx);
    io.AddKeyEvent(ImGuiKey_GamepadFaceDown, true);
    io.AddKeyEvent(ImGuiKey_GamepadFaceRight, true);
    gate.Filter(*ctx); Consume(*ctx);
    CHECK(ImGui::GetKeyData(ImGuiKey_GamepadFaceDown)->Down, "fresh A works after deduplicated physical release");
    CHECK(!ImGui::GetKeyData(ImGuiKey_GamepadFaceRight)->Down, "B still blocked until its own release");
    gate.Arm();
    gate.Filter(*ctx); Consume(*ctx);
    CHECK(!ImGui::GetKeyData(ImGuiKey_GamepadFaceDown)->Down, "reopen clears stale down state without queued event");
    ImGui::DestroyContext(ctx);
    std::printf("%d failure(s)\n", failures);
    return failures ? 1 : 0;
}
