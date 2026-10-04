#pragma once

#include "imgui.h"
#include "imgui_internal.h"

// Lucide icons, drawn from the embedded Lucide TTF. Icons are addressed by
// their lucide name ("crosshair", "sliders-horizontal", ...) exactly as listed
// in framework/data/lucide-font/codepoints.json.
namespace lucide
{
    // 0 when the name is not part of the icon font.
    unsigned short codepoint(const char* name);

    // Writes the UTF-8 encoding of a glyph into buf (8 bytes is enough) and
    // returns it. Empty string for unknown names.
    const char* glyph(const char* name, char* buf, int buf_size);

    // Draws the icon centred inside bb, fitted to the shorter axis.
    // Returns false when the name is unknown or the glyph is not baked yet.
    bool draw(ImDrawList* dl, const char* name, const ImRect& bb, ImU32 col);
}
