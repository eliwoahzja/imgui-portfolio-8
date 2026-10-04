#pragma once

#include "imgui.h"
#include "imgui_internal.h"

// Renders the SVG snippets registered in framework/data/custom_icons.h with
// ImDrawList. Parsed geometry is cached per name, so drawing is just a
// transform plus a stroke each frame.
namespace svg_icons
{
    // True when the name exists in the custom icon table.
    bool has(const char* name);

    // Draws the icon fitted into bb (aspect preserved, centred).
    // scale_hint multiplies the stroke width, e.g. for extra-thin variants.
    bool draw(ImDrawList* dl, const char* name, const ImRect& bb, ImU32 col,
        float stroke_scale = 1.f);
}
