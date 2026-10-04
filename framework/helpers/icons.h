#pragma once

#include "imgui.h"
#include "imgui_internal.h"

// Single entry point for every icon in the UI. Names are Lucide names
// ("crosshair", "sliders-horizontal", "ellipsis-vertical", ...).
//
// Lookup order:
//   1. framework/data/custom_icons.h  - hand-pasted SVG, overrides everything
//   2. the embedded Lucide icon font
namespace icons
{
    // Draws the icon centred inside bb. Returns false when the name is unknown.
    bool draw(ImDrawList* dl, const char* name, const ImRect& bb, ImU32 col);

    // Same, but bb is derived from a centre point and a side length.
    bool draw(ImDrawList* dl, const char* name, const ImVec2& centre, float side, ImU32 col);

    // True when the name resolves to either source.
    bool exists(const char* name);
}
