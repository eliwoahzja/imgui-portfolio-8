#include "settings_popup.h"

#include "core/blur_renderer.h"
#include "helpers/fonts.h"
#include "helpers/lucide.h"
#include "helpers/widgets.h"
#include "theme/anim.h"
#include "theme/colors.h"
#include "theme/layout.h"
#include "theme/ui_scale.h"

#include "imgui_internal.h"

namespace settings_popup
{
    void toggle(state& s)
    {
        s.open = !s.open;
        if (s.open)
            s.open_frame = ImGui::GetFrameCount();
    }

    void draw(state& s, bool& blocks_drag)
    {
        s.alpha = anim::step(s.alpha, s.open ? 1.f : 0.f, 28.f);

        if (s.alpha < 0.01f && !s.open)
            return;

        // Figma: 360x239, rgba(0,0,0,0.7), 1px rgba(0,0,0,0.3), blur 12.5, r14
        const float w     = ui_scale::s(layout::settings_popup_w);
        const float h     = ui_scale::s(layout::settings_popup_h);
        const float round = ui_scale::s(layout::settings_popup_round);
        const float pad   = ui_scale::s(layout::settings_popup_pad);

        const ImVec2 display = ImGui::GetIO().DisplaySize;
        // Slight slide-in from above while fading.
        const float slide = (1.f - s.alpha) * ui_scale::s(8.f);
        ImVec2 min = {
            s.anchor_right ? s.anchor.x - w : s.anchor.x,
            s.anchor.y - slide };

        // Keep it on screen.
        min.x = ImClamp(min.x, ui_scale::s(8.f), ImMax(ui_scale::s(8.f), display.x - w - ui_scale::s(8.f)));
        min.y = ImClamp(min.y, ui_scale::s(8.f), ImMax(ui_scale::s(8.f), display.y - h - ui_scale::s(8.f)));

        const ImRect bb(min, { min.x + w, min.y + h });

        ImGui::PushStyleVar(ImGuiStyleVar_Alpha, ImGui::GetStyle().Alpha * s.alpha);
        ImDrawList* dl = ImGui::GetWindowDrawList();

        blur_renderer::draw_live_blur(dl, bb.Min, bb.Max, round);
        dl->AddRectFilled(bb.Min, bb.Max, ImGui::GetColorU32(colors::dropdown_bg), round);
        dl->AddRect(bb.Min, bb.Max, ImGui::GetColorU32(ImVec4(0.f, 0.f, 0.f, 0.3f)),
            round, 0, ui_scale::s(1.f));

        // Six rows share the interior height evenly.
        const int   row_count = 6;
        const float row_h = (h - pad * 2.f) / (float)row_count;
        float y = bb.Min.y + pad;

        const auto next_row = [&]() -> ImRect
        {
            const ImRect r({ bb.Min.x, y }, { bb.Max.x, y + row_h });
            y += row_h;
            return r;
        };

        widgets::checkbox_row("##ms_autosave", "Auto Save", &s.auto_save, next_row(), blocks_drag);

        // DPI Scale: preset multipliers applied on top of the resolution auto-fit.
        {
            static const char* const dpi_items[] = { "75%", "100%", "125%", "150%", "175%", "200%" };
            static const float dpi_values[] = { 75.f, 100.f, 125.f, 150.f, 175.f, 200.f };

            int index = 2;
            for (int i = 0; i < IM_ARRAYSIZE(dpi_values); ++i)
                if (dpi_values[i] == s.dpi_scale)
                    index = i;

            if (widgets::combo_row("##ms_dpi", "DPI Scale", &index, dpi_items,
                    IM_ARRAYSIZE(dpi_items), next_row(), blocks_drag))
                s.dpi_scale = dpi_values[ImClamp(index, 0, IM_ARRAYSIZE(dpi_values) - 1)];
        }

        {
            static const char* const theme_items[] = { "Night", "Light" };
            int index = ImClamp(s.theme_index, 0, IM_ARRAYSIZE(theme_items) - 1);
            if (widgets::combo_row("##ms_theme", "Theme", &index, theme_items,
                    IM_ARRAYSIZE(theme_items), next_row(), blocks_drag))
            {
                s.theme_index = index;
                colors::apply_theme(index == 1 ? colors::theme::light : colors::theme::night);
            }
        }

        // Animation speed: 0..200%, off below the 50% threshold.
        widgets::slider_row("##ms_anim", "Animation speed", &anim::speed_percent,
            anim::slider_min, anim::slider_max, next_row(), blocks_drag);

        widgets::color_row("##ms_accent", "Accent color", &colors::accent, next_row(), blocks_drag);

        widgets::keybind_row("##ms_menubind", "Menu bind", &s.menu_key, next_row(), blocks_drag);

        ImGui::PopStyleVar();

        // Click outside closes — but never on the frame the popup was opened, and
        // never when the click lands on something this panel spawned. The colour
        // picker and the combo dropdowns are separate ImGui windows drawn on top of
        // the panel, so geometrically they sit "outside" bb: without this guard the
        // first click inside the picker tore the whole panel down.
        const ImGuiWindow* hovered = GImGui->HoveredWindow;
        const bool on_child_window = hovered && hovered != ImGui::GetCurrentWindow();

        if (s.open && ImGui::GetFrameCount() != s.open_frame &&
            ImGui::IsMouseClicked(ImGuiMouseButton_Left) &&
            !bb.Contains(ImGui::GetIO().MousePos) && !on_child_window)
        {
            s.open = false;
        }
    }
}
