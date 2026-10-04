#pragma once

#include "imgui.h"

namespace fonts
{
    void init();
    void shutdown();

    void  sync_ui_scale(float factor);
    float design_to_pixel(float design_size);
    float size(ImFont* font);

    // MuseoSansEx 500 - the UI face. Sizes are in design px.
    ImFont* inter(float design_size, bool bold = false);
    // Lucide icon face. Only requested codepoints are baked, see request_icon.
    ImFont* icon(float design_size);
    // Zen Dots
    ImFont* logo(float design_size);

    // Bakes one Lucide glyph into the atlas. Unknown codepoints schedule a
    // rebuild, so the glyph shows up on the next frame.
    void request_icon(unsigned short codepoint);
}
