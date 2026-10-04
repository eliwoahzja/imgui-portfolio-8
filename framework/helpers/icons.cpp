#include "icons.h"

#include "helpers/lucide.h"
#include "helpers/svg_icons.h"

namespace icons
{
    bool draw(ImDrawList* dl, const char* name, const ImRect& bb, ImU32 col)
    {
        if (svg_icons::draw(dl, name, bb, col))
            return true;
        return lucide::draw(dl, name, bb, col);
    }

    bool draw(ImDrawList* dl, const char* name, const ImVec2& centre, float side, ImU32 col)
    {
        const float half = side * 0.5f;
        const ImRect bb = { { centre.x - half, centre.y - half },
                            { centre.x + half, centre.y + half } };
        return draw(dl, name, bb, col);
    }

    bool exists(const char* name)
    {
        return svg_icons::has(name) || lucide::codepoint(name) != 0;
    }
}
