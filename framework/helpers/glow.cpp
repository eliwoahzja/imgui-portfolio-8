#include "glow.h"

#include "core/glow_renderer.h"
#include "theme/colors.h"

namespace glow
{
    void draw_accent_rect(ImDrawList* /*dl*/, const ImRect& rect, float round, float strength)
    {
        glow_renderer::queue_rect(rect, round, strength, colors::accent);
    }

    void clear_frame()
    {
        glow_renderer::clear_queue();
    }
}
