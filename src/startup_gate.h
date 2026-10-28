#pragma once

#include <cstdint>

namespace startup {

// Pure worker-side policy. A scene-pointer sample is a heuristic, not proof that
// another overlay has finished installing its hooks. Edges must be polled even
// while this gate is closed; never queue a title-screen press for later.
class Gate {
public:
    static constexpr std::uint64_t kStableMs = 500;
    enum class PanelAction { None, Install, Toggle, Unavailable };

    void Sample(bool inScene, std::uint64_t now)
    {
        const bool wasReady = ready_;
        if (!inScene) {
            tracking_ = false;
            ready_ = false;
        } else {
            if (!tracking_) { tracking_ = true; since_ = now; }
            ready_ = now - since_ >= kStableMs;
        }
        // Suppress the boundary sample too: an early held key is not a new press.
        actions_ = ready_ && wasReady;
    }
    bool AllowsActions() const { return actions_; }
    bool SceneReady() const { return ready_; }

    PanelAction Panel(bool fired, bool disabled)
    {
        if (!actions_ || !fired) return PanelAction::None;
        if (disabled) return PanelAction::Unavailable;
        if (installed_) return PanelAction::Toggle;
        if (attempted_) return PanelAction::Unavailable;
        // Install can partially patch a shared vtable before returning failure.
        // Latch before calling it: a second attempt could save our own callback.
        attempted_ = true;
        return PanelAction::Install;
    }
    void CompleteInstall(bool success) { installed_ = attempted_ && success; }

private:
    bool tracking_ = false, ready_ = false, actions_ = false;
    bool attempted_ = false, installed_ = false;
    std::uint64_t since_ = 0;
};

} // namespace startup
