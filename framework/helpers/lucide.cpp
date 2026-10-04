#include "lucide.h"

#include "data/lucide_codepoints.h"
#include "helpers/fonts.h"
#include "theme/ui_scale.h"

#include <cstring>

namespace lucide
{
    namespace
    {
        int encode_utf8(unsigned int cp, char* buf, int buf_size)
        {
            if (buf_size < 4)
                return 0;

            if (cp < 0x80)
            {
                buf[0] = (char)cp;
                return 1;
            }
            if (cp < 0x800)
            {
                buf[0] = (char)(0xC0 | (cp >> 6));
                buf[1] = (char)(0x80 | (cp & 0x3F));
                return 2;
            }
            buf[0] = (char)(0xE0 | (cp >> 12));
            buf[1] = (char)(0x80 | ((cp >> 6) & 0x3F));
            buf[2] = (char)(0x80 | (cp & 0x3F));
            return 3;
        }
    }

    unsigned short codepoint(const char* name)
    {
        if (!name || !*name)
            return 0;

        int lo = 0;
        int hi = lucide_codepoints::table_count - 1;
        while (lo <= hi)
        {
            const int mid = (lo + hi) / 2;
            const int cmp = std::strcmp(name, lucide_codepoints::table[mid].name);
            if (cmp == 0)
                return lucide_codepoints::table[mid].cp;
            if (cmp < 0)
                hi = mid - 1;
            else
                lo = mid + 1;
        }
        return 0;
    }

    const char* glyph(const char* name, char* buf, int buf_size)
    {
        if (!buf || buf_size < 5)
            return "";

        buf[0] = 0;
        const unsigned short cp = codepoint(name);
        if (cp == 0)
            return buf;

        const int len = encode_utf8(cp, buf, buf_size);
        buf[len] = 0;
        fonts::request_icon(cp);
        return buf;
    }

    bool draw(ImDrawList* dl, const char* name, const ImRect& bb, ImU32 col)
    {
        if (!dl)
            return false;

        const unsigned short cp = codepoint(name);
        if (cp == 0)
            return false;

        fonts::request_icon(cp);

        const float side = ImMin(bb.GetWidth(), bb.GetHeight());
        if (side < 1.f)
            return false;

        // Font sizes are kept in design px; the rect is already scaled.
        const float design = ImMax(1.f, ImFloor(side / ImMax(ui_scale::factor, 0.01f) + 0.5f));
        ImFont* font = fonts::icon(design);
        if (!font || !font->FindGlyphNoFallback((ImWchar)cp))
            return false;

        char buf[8];
        const int len = encode_utf8(cp, buf, IM_ARRAYSIZE(buf));
        buf[len] = 0;

        const float render = font->FontSize;
        const ImVec2 ts = font->CalcTextSizeA(render, FLT_MAX, 0.f, buf);
        const ImVec2 pos = {
            ImFloor(bb.Min.x + (bb.GetWidth() - ts.x) * 0.5f),
            ImFloor(bb.Min.y + (bb.GetHeight() - ts.y) * 0.5f) };
        dl->AddText(font, render, pos, col, buf);
        return true;
    }
}
