#pragma once

#include "imgui.h"
#include "imgui_internal.h"

namespace overlay {

// Render-thread only. Consumes only ImGui's copy of controller events; XInput
// and the game's input stream remain untouched. Must run AFTER backend polling
// and BEFORE ImGui::NewFrame(), including when InputTrickleEventQueue is enabled.
class PanelGamepadReleaseGate {
public:
    void Arm() { Reset(); armed_ = true; }
    void Reset()
    {
        armed_ = false;
        lastEvent_ = 0;
        for (bool& blocked : blocked_) blocked = false;
    }

    void Filter(ImGuiContext& context)
    {
        if (armed_) {
            for (int key = ImGuiKey_GamepadStart; key <= ImGuiKey_GamepadRStickDown; ++key) {
                ImGuiKeyData* data = ImGui::GetKeyData(&context, (ImGuiKey)key);
                blocked_[key - ImGuiKey_GamepadStart] = data->Down;
                data->Down = false;
                data->AnalogValue = 0.0f;
                data->DownDuration = data->DownDurationPrev = -1.0f;
            }
        }
        bool polledDown[ImGuiKey_GamepadRStickDown - ImGuiKey_GamepadStart + 1]{};
        ImU32 newest = lastEvent_;
        for (ImGuiInputEvent& event : context.InputEventsQueue) {
            if (event.EventId <= lastEvent_) continue;
            if (event.EventId > newest) newest = event.EventId;
            if (event.Type != ImGuiInputEventType_Key ||
                event.Key.Key < ImGuiKey_GamepadStart || event.Key.Key > ImGuiKey_GamepadRStickDown)
                continue;
            const int index = event.Key.Key - ImGuiKey_GamepadStart;
            polledDown[index] = event.Key.Down;
            bool& blocked = blocked_[index];
            if (armed_ && event.Key.Down) blocked = true;
            if (!event.Key.Down) {
                blocked = false;
            } else if (blocked) {
                event.Key.Down = false;
                event.Key.AnalogValue = 0.0f;
            }
        }
        lastEvent_ = newest;
        // Win32 backend polls every gamepad key each frame. While gated, ImGui's
        // latest state is false, so held=true always produces a new event. A
        // release may be deduplicated by AddKeyAnalogEvent(false): no fresh down
        // therefore also proves release (or disconnect), not a permanent block.
        if (!armed_) {
            for (int i = 0; i < IM_ARRAYSIZE(blocked_); ++i)
                if (!polledDown[i]) blocked_[i] = false;
        }
        // Keys may still be down in ImGui from the last time the panel was open,
        // even if backend polling deduplicated this frame's held-button event.
        for (int key = ImGuiKey_GamepadStart; key <= ImGuiKey_GamepadRStickDown; ++key) {
            if (!blocked_[key - ImGuiKey_GamepadStart]) continue;
            ImGuiKeyData* data = ImGui::GetKeyData(&context, (ImGuiKey)key);
            data->Down = false;
            data->AnalogValue = 0.0f;
            data->DownDuration = data->DownDurationPrev = -1.0f;
        }
        armed_ = false;
    }

private:
    bool armed_ = false;
    ImU32 lastEvent_ = 0;
    bool blocked_[ImGuiKey_GamepadRStickDown - ImGuiKey_GamepadStart + 1]{};
};

} // namespace overlay
