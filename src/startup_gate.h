#pragma once

#include <cstdint>

namespace startup {

// Startup readiness is a one-way latch, not a recurring loading-screen gate.
// Title readiness is a conservative sample, not proof that other hooks settled.
class Gate {
public:
    static constexpr std::uint64_t kStableMs = 500;
    enum class PanelAction { None, Install, Toggle, Unavailable };

    void Sample(bool titleStarted, bool inScene, std::uint64_t now)
    {
        const bool wasReady = ready_;
        if (!ready_) {
            if (!(titleStarted || inScene)) tracking_ = false;
            else {
                if (!tracking_) { tracking_ = true; since_ = now; }
                ready_ = now - since_ >= kStableMs;
            }
        }
        // Consume early shortcut edges, including the initial readiness boundary.
        actions_ = ready_ && wasReady;
        // Game-writing shortcuts still require a stable loaded character scene.
        if (!inScene) { sceneTracking_ = false; sceneReady_ = false; }
        else {
            if (!sceneTracking_) { sceneTracking_ = true; sceneSince_ = now; }
            const bool wasSceneReady = sceneReady_;
            sceneReady_ = now - sceneSince_ >= kStableMs;
            sceneActions_ = sceneReady_ && wasSceneReady;
        }
        if (!inScene) sceneActions_ = false;
    }
    bool AllowsActions() const { return actions_ && sceneActions_; }
    bool StartupReady() const { return ready_; }

    bool BeginInstall(bool disabled)
    {
        if (!ready_ || disabled || attempted_) return false;
        // Latch before the call: a failed install may already have patched slots.
        attempted_ = true;
        return true;
    }
    PanelAction Panel(bool fired, bool disabled)
    {
        if (!actions_ || !fired) return PanelAction::None;
        if (disabled) return PanelAction::Unavailable;
        if (installed_) return PanelAction::Toggle;
        if (BeginInstall(false)) return PanelAction::Install;
        return PanelAction::Unavailable;
    }
    void CompleteInstall(bool success) { installed_ = attempted_ && success; }

private:
    bool tracking_ = false, ready_ = false, actions_ = false;
    bool sceneTracking_ = false, sceneReady_ = false, sceneActions_ = false;
    bool attempted_ = false, installed_ = false;
    std::uint64_t since_ = 0, sceneSince_ = 0;
};

} // namespace startup
