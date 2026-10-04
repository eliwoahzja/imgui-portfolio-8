#pragma once

#include "imgui.h"

namespace settings_popup
{
    struct state
    {
        bool  open  = false;
        float alpha = 0.f;

        // Frame the popup was opened on, so the very click that opened it is not
        // also treated as a click-outside that closes it again.
        int   open_frame = -1;

        // Where the popup hangs from — set by the caller from the icon that
        // opened it. `anchor_right` aligns the popup's right edge to anchor.x.
        ImVec2 anchor       = { 0.f, 0.f };
        bool   anchor_right = true;

        // Settings values (Figma design)
        bool  auto_save       = false;
        float dpi_scale       = 100.f;  // manual % on top of the resolution auto-fit
        int   theme_index     = 0;      // 0 = Night (default), 1 = Light
        int   menu_key        = 0x2D;   // VK_INSERT
    };

    void draw(state& s, bool& blocks_drag);
    void toggle(state& s);
}
