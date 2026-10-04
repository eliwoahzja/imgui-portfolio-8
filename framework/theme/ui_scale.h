#pragma once

#include "imgui.h"
#include "imgui_internal.h"   // ImMin/ImMax/ImClamp

namespace ui_scale
{
    // Reference display. 1920x1080 — and every other 16:9 mode of that size
    // class — is the 100% case: the menu there has exactly the footprint it was
    // designed with. Larger and smaller displays scale from here.
    inline constexpr float ref_display_w = 1920.f;
    inline constexpr float ref_display_h = 1080.f;

    // Design px (theme/layout.h) -> screen px on the reference display at 100%.
    // The Figma board is 2560 wide, so a naive 1920/2560 = 0.75 of the design
    // scale (0.9375 here) read a size too large on an actual Full HD screen;
    // 100% is what used to sit behind the 75% preset.
    inline constexpr float ref_factor = 0.9375f * 0.75f;

    // Final multiplier applied by s(): auto-fit for the current resolution,
    // times the user's manual percentage.
    inline float factor = ref_factor;

    // The two inputs to `factor`, kept apart so either can change alone.
    inline float auto_factor   = ref_factor;
    inline float manual_factor = 1.f;

    inline void recompute() { factor = auto_factor * manual_factor; }

    // Derives the auto factor from the render target size, preserving the layout,
    // spacing and proportions at every resolution. Returns ref_factor on
    // 1920x1080, and the same on 1920x1200: fitting against the *smaller* of the
    // two ratios makes a 16:10 panel read as its 16:9 equivalent instead of
    // stretching the menu to fill the extra height.
    inline void set_display_size(const ImVec2& display)
    {
        if (display.x <= 0.f || display.y <= 0.f)
            return;

        const float fit = ImMin(display.x / ref_display_w, display.y / ref_display_h);
        auto_factor = ImClamp(fit, 0.5f, 3.f) * ref_factor;
        recompute();
    }

    inline void set_percent(float percent)
    {
        manual_factor = ImMax(0.1f, percent * 0.01f);
        recompute();
    }

    inline float s(float design_px) { return design_px * factor; }
    inline float px(float design_px) { return design_px * factor; }
}
