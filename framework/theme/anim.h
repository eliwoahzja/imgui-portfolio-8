#pragma once

#include "imgui.h"
#include "imgui_internal.h"

// Animation timing, driven by the "Animation speed" slider in the menu settings.
//
// The slider runs 0..200%. Below the 50% threshold animations are OFF and every
// transition snaps instantly. From 50% to 200% the value maps smoothly onto a
// 0..2x speed multiplier.
namespace anim
{
    inline constexpr float slider_min       = 0.f;
    inline constexpr float slider_max       = 200.f;
    inline constexpr float slider_threshold = 50.f;

    inline float speed_percent = 100.f;

    inline bool enabled() { return speed_percent >= slider_threshold; }

    // 0 when disabled, otherwise the percent maps directly onto the speed:
    // 50% -> 0.5x, 100% -> 1x, 200% -> 2x.
    inline float multiplier()
    {
        if (!enabled())
            return 0.f;
        return speed_percent / 100.f;
    }

    // Lerp factor for an exponential approach at the given base rate.
    // Returns 1 when animations are off, so callers snap to the target.
    inline float lerp_t(float rate)
    {
        const float m = multiplier();
        if (m <= 0.f)
            return 1.f;
        return ImMin(1.f, ImGui::GetIO().DeltaTime * rate * m);
    }

    // Convenience: advance `current` toward `target` at `rate`.
    inline float step(float current, float target, float rate)
    {
        return ImLerp(current, target, lerp_t(rate));
    }
}
