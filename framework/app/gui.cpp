#include "gui.h"

#include "profile_service.h"
#include "net/http.h"

#include "core/blur_renderer.h"
#include "core/config_store.h"
#include "core/glow_renderer.h"
#include "helpers/fonts.h"
#include "helpers/icons.h"
#include "helpers/screenshot.h"
#include "helpers/search_index.h"
#include "helpers/settings_popup.h"
#include "helpers/widgets.h"
#include "helpers/window_drag.h"
#include "pages/aim_page.h"
#include "pages/config_page.h"
#include "pages/settings_page.h"
#include "pages/visuals_page.h"
#include "theme/anim.h"
#include "theme/colors.h"
#include "theme/layout.h"
#include "theme/ui_scale.h"

#include "imgui.h"
#include "imgui_internal.h"

#include <cmath>
#include <cstdio>
#include <cstdint>
#include <string>

namespace
{
    // Figma sidebar: six tabs, no exit button. Settings moved to the
    // ellipsis-vertical popup in the top bar.
    enum class page
    {
        aimbot,
        players,
        world,
        misc,
        config,
        count_
    };

    struct tab_def
    {
        page        id;
        const char* icon;
        const char* label;
    };

    const tab_def g_tabs[] = {
        { page::aimbot,  "crosshair",          "Aimbot"  },
        { page::players, "users",              "Players" },
        { page::world,   "globe",              "World"   },
        { page::misc,    "sliders-horizontal", "Misc"    },
        { page::config,  "folder",             "Config"  },
    };

    page  g_selected_tab = page::aimbot;
    page  g_prev_tab     = page::aimbot;
    ImVec2 g_panel_origin     = {};
    bool  g_shot_full         = false;
    float g_tab_fade          = 1.f;
    ImGuiStyle g_base_style   = {};
    bool g_base_style_saved   = false;

    aim_page::state      g_aim;
    visuals_page::state  g_visuals;
    config_page::state   g_config;
    settings_page::state g_settings;
    settings_popup::state g_settings_popup;

    const char* active_config_name()
    {
        return g_config.active;
    }

    void save_active_config()
    {
        config_page::save_current_async(g_config);
    }

    constexpr std::uint64_t network_grace_ms = 5ull * 60ull * 1000ull;

    void draw_network_indicator()
    {
        if (!http::is_offline())
            return;

        const std::uint64_t elapsed = http::offline_ms();
        const std::uint64_t remaining = elapsed < network_grace_ms
            ? network_grace_ms - elapsed : 0;
        const std::uint64_t seconds = (remaining + 999) / 1000;

        char label[64];
        sprintf_s(label, "NO NETWORK  -  exit in %02llu:%02llu",
            (unsigned long long)(seconds / 60),
            (unsigned long long)(seconds % 60));

        ImFont* font = fonts::inter(14.f, true);
        if (!font)
            font = ImGui::GetFont();
        const float font_size = font ? fonts::size(font) : ImGui::GetFontSize();
        const ImVec2 text_size = font
            ? font->CalcTextSizeA(font_size, FLT_MAX, 0.f, label)
            : ImGui::CalcTextSize(label);

        const float pad_x = ui_scale::s(14.f);
        const float pad_y = ui_scale::s(10.f);
        const float dot_r = ui_scale::s(4.f);
        const float gap = ui_scale::s(10.f);
        const float margin = ui_scale::s(16.f);
        const ImVec2 display = ImGui::GetIO().DisplaySize;
        const ImVec2 size = {
            pad_x * 2.f + dot_r * 2.f + gap + text_size.x,
            pad_y * 2.f + ImMax(text_size.y, dot_r * 2.f)
        };
        const ImVec2 min = { display.x - margin - size.x, margin };
        const ImVec2 max = min + size;

        ImDrawList* fg = ImGui::GetForegroundDrawList();
        fg->AddRectFilled(min, max, IM_COL32(22, 18, 20, 235), ui_scale::s(9.f));
        fg->AddRect(min, max, IM_COL32(236, 79, 91, 150), ui_scale::s(9.f), 0, ui_scale::s(1.f));
        fg->AddCircleFilled({ min.x + pad_x + dot_r, (min.y + max.y) * 0.5f },
            dot_r, IM_COL32(244, 75, 87, 255));
        if (font)
            fg->AddText(font, font_size,
                { min.x + pad_x + dot_r * 2.f + gap, min.y + (size.y - text_size.y) * 0.5f },
                IM_COL32(255, 235, 237, 255), label);
    }

    // --- top bar search ---------------------------------------------------
    struct search_state
    {
        char  buf[64]    = {};
        bool  active     = false;   // input focused / dropdown visible
        float alpha      = 0.f;     // dropdown fade
        int   hover_idx  = -1;
    };

    search_state g_search;
    bool g_index_built = false;     // hidden pass over all pages done?

    void apply_style()
    {
        ImGuiStyle& s = ImGui::GetStyle();
        s.WindowRounding   = 0.f;
        s.WindowBorderSize = 0.f;
        s.WindowPadding    = { 0.f, 0.f };
        s.AntiAliasedLines = true;
        s.AntiAliasedFill  = true;
        s.ChildBorderSize  = 0.f;
        s.FrameBorderSize  = 0.f;
        s.ScrollbarSize    = 0.f;

        ImVec4* c = s.Colors;
        c[ImGuiCol_WindowBg]         = { 0.f, 0.f, 0.f, 0.f };
        c[ImGuiCol_ChildBg]          = { 0.f, 0.f, 0.f, 0.f };
        c[ImGuiCol_Text]             = colors::text;
        c[ImGuiCol_ScrollbarBg]      = { 0.f, 0.f, 0.f, 0.f };
        c[ImGuiCol_ScrollbarGrab]    = { 0.f, 0.f, 0.f, 0.f };
        c[ImGuiCol_ScrollbarGrabHovered] = { 0.f, 0.f, 0.f, 0.f };
        c[ImGuiCol_ScrollbarGrabActive]  = { 0.f, 0.f, 0.f, 0.f };

        if (!g_base_style_saved)
        {
            g_base_style = s;
            g_base_style_saved = true;
        }
    }

    void apply_scaled_style()
    {
        ImGui::GetStyle() = g_base_style;
        ImGui::GetStyle().ScaleAllSizes(ui_scale::factor);
        ImGui::GetIO().FontGlobalScale = 1.f;
    }

    void draw_text_centered(ImDrawList* dl, ImFont* font, const ImRect& rect,
        ImU32 col, const char* text)
    {
        if (!font || !text)
            return;

        const float render = fonts::size(font);
        const ImVec2 ts = font->CalcTextSizeA(render, FLT_MAX, 0.f, text);
        const ImVec2 pos = {
            rect.Min.x + (rect.GetWidth() - ts.x) * 0.5f,
            rect.Min.y + (rect.GetHeight() - ts.y) * 0.5f };
        dl->AddText(font, render, pos, col, text);
    }

    // Shortens `text` until it fits `avail`, appending an ellipsis. Trimming is
    // done on whole UTF-8 sequences so a multi-byte name never breaks mid-glyph.
    std::string truncate_text(ImFont* font, const std::string& text, float avail)
    {
        if (!font || text.empty() || avail <= 0.f)
            return text;

        const float render = fonts::size(font);
        if (font->CalcTextSizeA(render, FLT_MAX, 0.f, text.c_str()).x <= avail)
            return text;

        static const char ellipsis[] = "\xE2\x80\xA6";

        std::string cut = text;
        while (!cut.empty())
        {
            cut.pop_back();
            while (!cut.empty() && ((unsigned char)cut.back() & 0xC0) == 0x80)
                cut.pop_back();

            const std::string candidate = cut + ellipsis;
            if (font->CalcTextSizeA(render, FLT_MAX, 0.f, candidate.c_str()).x <= avail)
                return candidate;
        }
        return ellipsis;
    }

    ImRect sidebar_tab_rect(const ImRect& sidebar, float y)
    {
        // Figma: tabs are full width (241px), aligned to left edge
        const float tab_w = ui_scale::s(layout::sidebar_tab_w);
        const float tab_h = ui_scale::s(layout::sidebar_tab_h);
        return { { sidebar.Min.x, y }, { sidebar.Min.x + tab_w, y + tab_h } };
    }

    void draw_sidebar_tab(ImDrawList* dl, ImFont* text_font, const ImRect& tab_bb,
        const tab_def& tab, int tab_index, bool selected, bool& blocks_window_drag)
    {
        ImGuiID id = ImGui::GetID((void*)(intptr_t)tab.id);
        ImGui::ItemAdd(tab_bb, id);

        bool hovered = false;
        bool held    = false;
        const bool pressed = ImGui::ButtonBehavior(tab_bb, id, &hovered, &held);

        if (hovered || held)
            blocks_window_drag = true;

        if (pressed)
            g_selected_tab = tab.id;

        static float sel_anim[(int)page::count_]   = {};
        static float hover_anim[(int)page::count_] = {};
        const int idx = ImClamp(tab_index, 0, (int)page::count_ - 1);
        sel_anim[idx]   = anim::step(sel_anim[idx],   selected ? 1.f : 0.f, 22.f);
        hover_anim[idx] = anim::step(hover_anim[idx], hovered && !selected ? 1.f : 0.f, 24.f);

        // Selected tab: accent-tinted gradient (accent 28% -> transparent)
        if (sel_anim[idx] > 0.01f)
        {
            const ImVec4 acc = colors::accent;
            const ImU32 grad_start = ImGui::GetColorU32(ImVec4(acc.x, acc.y, acc.z, 0.28f * sel_anim[idx]));
            const ImU32 grad_end   = ImGui::GetColorU32(ImVec4(acc.x, acc.y, acc.z, 0.f));
            dl->AddRectFilledMultiColor(tab_bb.Min, tab_bb.Max,
                grad_start, grad_end, grad_end, grad_start);

            // Thin accent bar on the left edge marks the active tab.
            const float bar_w = ui_scale::s(3.f);
            dl->AddRectFilled(tab_bb.Min, { tab_bb.Min.x + bar_w, tab_bb.Max.y },
                ImGui::GetColorU32(ImVec4(acc.x, acc.y, acc.z, sel_anim[idx])));
        }

        // Figma: not selected tabs have subtle gradient on hover
        if (hover_anim[idx] > 0.01f && !selected)
        {
            const ImU32 hover_start = ImGui::GetColorU32(ImVec4(0x53 / 255.f, 0x53 / 255.f, 0x53 / 255.f, 0.068f * hover_anim[idx]));
            const ImU32 hover_end   = ImGui::GetColorU32(ImVec4(0xB9 / 255.f, 0xB9 / 255.f, 0xB9 / 255.f, 0.f));
            dl->AddRectFilledMultiColor(tab_bb.Min, tab_bb.Max,
                hover_start, hover_end, hover_end, hover_start);
        }

        // Icon: lucide 24x24 at the left, with the label after it
        const float icon_x    = tab_bb.Min.x + ui_scale::s(layout::sidebar_tab_icon_x);
        const float icon_size = ui_scale::s(layout::sidebar_tab_icon);
        const ImRect icon_rect(
            { icon_x, tab_bb.Min.y + (tab_bb.GetHeight() - icon_size) * 0.5f },
            { icon_x + icon_size, tab_bb.Min.y + (tab_bb.GetHeight() + icon_size) * 0.5f });

        const float icon_alpha = 0.55f + 0.45f * ImMax(sel_anim[idx], hover_anim[idx]);
        icons::draw(dl, tab.icon, icon_rect,
            ImGui::GetColorU32(colors::fg(icon_alpha)));

        // Text label: 24px MuseoSansEx, positioned after icon
        if (text_font && tab.label)
        {
            const float text_x = icon_x + icon_size + ui_scale::s(layout::sidebar_tab_text_gap);
            const float text_size = fonts::size(text_font);
            const ImVec2 text_pos = {
                text_x,
                tab_bb.Min.y + (tab_bb.GetHeight() - text_size) * 0.5f
            };
            const float text_alpha = 0.6f + 0.4f * ImMax(sel_anim[idx], hover_anim[idx]);
            dl->AddText(text_font, text_size, text_pos,
                ImGui::GetColorU32(colors::fg(text_alpha)), tab.label);
        }
    }

    void draw_sidebar(ImDrawList* dl, const ImRect& sidebar, bool& blocks_window_drag)
    {
        using namespace layout;

        dl->AddRectFilled(sidebar.Min, sidebar.Max, ImGui::GetColorU32(colors::sidebar),
            ui_scale::s(shell_round), ImDrawFlags_RoundCornersLeft);

        ImFont* logo_font = fonts::logo(sidebar_logo_font);
        if (logo_font)
        {
            const char* wordmark = "NAMEPLACE";
            float lsize = fonts::size(logo_font);

            // Zen Dots is a wide face, so shrink until the wordmark fits the sidebar.
            const float avail = sidebar.GetWidth() - ui_scale::s(sidebar_logo_x) * 2.f;
            ImVec2 ts = logo_font->CalcTextSizeA(lsize, FLT_MAX, 0.f, wordmark);
            if (ts.x > avail && ts.x > 0.f)
            {
                lsize *= avail / ts.x;
                ts = logo_font->CalcTextSizeA(lsize, FLT_MAX, 0.f, wordmark);
            }

            const float lx = ImFloor(sidebar.Min.x + (sidebar.GetWidth() - ts.x) * 0.5f);
            const float ly = ImFloor(sidebar.Min.y + ui_scale::s(sidebar_logo_y));

            // Figma: text-shadow 0 0 17.6px rgba(255,255,255,0.35) — ~8px once the
            // 2560px design is mapped onto this window, so sigma is 4 design px.
            // Copies are spread over concentric rings with gaussian weights: the
            // previous four-axis offsets only ever darkened up/down/left/right,
            // which read as hard vector edges instead of a halo.
            {
                constexpr int   rings  = 5;
                constexpr int   spokes = 12;
                constexpr float peak   = 0.35f;

                const float sigma = ui_scale::s(4.f);

                float weight[rings];
                float total = 0.f;
                for (int r = 0; r < rings; ++r)
                {
                    const float radius = sigma * (0.5f + (float)r * 0.5f);
                    weight[r] = expf(-(radius * radius) / (2.f * sigma * sigma));
                    total += weight[r] * (float)spokes;
                }

                for (int r = 0; r < rings; ++r)
                {
                    const float radius = sigma * (0.5f + (float)r * 0.5f);
                    const ImU32 glow = ImGui::GetColorU32(
                        colors::fg(peak * weight[r] / total));

                    // Half-step the phase on every other ring so the samples never
                    // line up into visible spokes.
                    const float phase = (float)r * (IM_PI / (float)spokes);
                    for (int k = 0; k < spokes; ++k)
                    {
                        const float ang = (2.f * IM_PI / (float)spokes) * (float)k + phase;
                        dl->AddText(logo_font, lsize,
                            { lx + cosf(ang) * radius, ly + sinf(ang) * radius }, glow, wordmark);
                    }
                }
            }
            dl->AddText(logo_font, lsize, { lx, ly },
                ImGui::GetColorU32(colors::text), wordmark);
        }

        ImFont* text_font = fonts::inter(24.f);  // Figma: 24px MuseoSansEx
        if (!text_font)
            text_font = ImGui::GetFont();

        // Tabs start level with the first content box, no vertical centring.
        const float tab_h   = ui_scale::s(sidebar_tab_h);
        const float tab_gap = ui_scale::s(sidebar_tabs_gap);
        float y = sidebar.Min.y + ui_scale::s(sidebar_tabs_y);

        for (int i = 0; i < (int)IM_ARRAYSIZE(g_tabs); ++i)
        {
            const ImRect tab_bb = sidebar_tab_rect(sidebar, y);
            draw_sidebar_tab(dl, text_font, tab_bb, g_tabs[i], i,
                g_selected_tab == g_tabs[i].id, blocks_window_drag);
            y += tab_h + tab_gap;
        }

        // User profile area at the bottom (Figma: 60x60 avatar at x=35, 36px up)
        const profile_svc::view& user = profile_svc::get();

        const float avatar_size = ui_scale::s(layout::user_avatar_size);
        const float user_y = sidebar.Max.y - ui_scale::s(layout::user_bottom_margin) - avatar_size;
        const ImVec2 avatar_pos = { sidebar.Min.x + ui_scale::s(layout::user_avatar_x), user_y };
        const ImVec2 avatar_center = { avatar_pos.x + avatar_size * 0.5f, avatar_pos.y + avatar_size * 0.5f };
        const ImVec2 avatar_end = { avatar_pos.x + avatar_size, avatar_pos.y + avatar_size };

        if (user.avatar)
        {
            dl->AddImageRounded(user.avatar, avatar_pos, avatar_end,
                { 0.f, 0.f }, { 1.f, 1.f }, IM_COL32_WHITE, avatar_size * 0.5f);
        }
        else
        {
            // Until the image arrives — or when the user has none — the circle
            // carries the first letter of the name, so the slot never empties.
            const bool known = user.st == profile_svc::state::ready;
            dl->AddCircleFilled(avatar_center, avatar_size * 0.5f,
                ImGui::GetColorU32(known ? colors::accent : colors::fg(0.08f)), 32);

            ImFont* avatar_font = fonts::inter(24.f, true);
            if (avatar_font)
            {
                draw_text_centered(dl, avatar_font, ImRect(avatar_pos, avatar_end),
                    ImGui::GetColorU32(known ? colors::text : colors::fg(0.35f)),
                    user.initial.c_str());
            }
        }

        ImFont* user_font = fonts::inter(layout::user_name_font);

        if (user_font)
        {
            const float text_x = avatar_pos.x + avatar_size + ui_scale::s(layout::user_text_gap);
            const float right  = sidebar.Max.x - ui_scale::s(layout::user_avatar_x);
            const float avail  = right - text_x;

            // Left-aligned: a centred name drifts as it grows and a long one
            // would run past the sidebar edge before the truncation shows.
            const std::string name = truncate_text(user_font, user.username, avail);

            const char* texts[2] = { name.c_str(), user.till.c_str() };

            const float line_gap = ui_scale::s(layout::user_line_gap);
            const float render   = fonts::size(user_font);

            float block_h = 0.f;
            ImVec2 sizes[2];
            for (int i = 0; i < 2; ++i)
            {
                sizes[i] = user_font->CalcTextSizeA(render, FLT_MAX, 0.f, texts[i]);
                block_h += sizes[i].y + (i ? line_gap : 0.f);
            }

            float y_line = avatar_center.y - block_h * 0.5f;
            for (int i = 0; i < 2; ++i)
            {
                dl->AddText(user_font, render, { text_x, y_line },
                    ImGui::GetColorU32(colors::fg(0.41f)), texts[i]);
                y_line += sizes[i].y + line_gap;
            }
        }
    }

    void draw_topbar(ImDrawList* dl, const ImRect& topbar, bool& blocks_window_drag)
    {
        using namespace layout;

        const float row_y = topbar.Min.y + ui_scale::s(topbar_row_y);
        const float row_h = ui_scale::s(topbar_row_h);
        const float round = ui_scale::s(shell_round);
        const float icon_size = ui_scale::s(topbar_icon_size);

        ImFont* text_font = fonts::inter(20.f);

        // Save button (Figma: 138x49, #000 @ 40%, r14, icon + label)
        {
            const float w = ui_scale::s(topbar_save_w);
            const ImRect bb({ topbar.Min.x + ui_scale::s(content_pad_x), row_y },
                            { topbar.Min.x + ui_scale::s(content_pad_x) + w, row_y + row_h });

            ImGuiID id = ImGui::GetID("##topbar_save");
            ImGui::ItemAdd(bb, id);
            bool hovered = false, held = false;
            const bool pressed = ImGui::ButtonBehavior(bb, id, &hovered, &held);
            if (hovered || held)
                blocks_window_drag = true;

            dl->AddRectFilled(bb.Min, bb.Max,
                ImGui::GetColorU32(hovered ? colors::control_hover : colors::box), round);

            const ImU32 col = ImGui::GetColorU32(hovered ? colors::text : colors::fg(0.8f));

            // Icon + label as a centred pair.
            const char* label = "Save";
            const float label_w = text_font
                ? text_font->CalcTextSizeA(fonts::size(text_font), FLT_MAX, 0.f, label).x : 0.f;
            const float gap = ui_scale::s(10.f);
            const float group_w = icon_size + gap + label_w;
            const float gx = bb.Min.x + (bb.GetWidth() - group_w) * 0.5f;

            const ImRect icon_bb({ gx, bb.GetCenter().y - icon_size * 0.5f },
                                 { gx + icon_size, bb.GetCenter().y + icon_size * 0.5f });
            icons::draw(dl, "save", icon_bb, col);

            if (text_font)
            {
                dl->AddText(text_font, fonts::size(text_font),
                    { gx + icon_size + gap, bb.GetCenter().y - fonts::size(text_font) * 0.5f },
                    col, label);
            }

            if (pressed)
                save_active_config();
        }

        // Gear on the far right: opens the menu settings popup.
        const float gear_x = topbar.Max.x - ui_scale::s(topbar_gear_margin) - icon_size;
        const ImRect gear_bb({ gear_x, row_y + (row_h - icon_size) * 0.5f },
                             { gear_x + icon_size, row_y + (row_h + icon_size) * 0.5f });
        {
            ImGuiID id = ImGui::GetID("##topbar_gear");
            ImGui::ItemAdd(gear_bb, id);
            bool hovered = false, held = false;
            const bool pressed = ImGui::ButtonBehavior(gear_bb, id, &hovered, &held);
            if (hovered || held)
                blocks_window_drag = true;

            icons::draw(dl, "settings", gear_bb,
                ImGui::GetColorU32(hovered ? colors::text : colors::fg(0.6f)));

            if (pressed)
            {
                settings_popup::toggle(g_settings_popup);
                // The card sits beside the shell, its top flush with the menu's top.
                g_settings_popup.anchor       = { topbar.Max.x + ui_scale::s(12.f), topbar.Min.y };
                g_settings_popup.anchor_right = false;
            }
        }

        // Search field (Figma: 434x49, right-aligned before the gear).
        // Live global search: typing filters every registered function row,
        // clicking a result jumps to its tab and pulses the row.
        {
            const float w = ui_scale::s(topbar_search_w);
            const float right = gear_bb.Min.x - ui_scale::s(26.f);
            const ImRect bb({ right - w, row_y }, { right, row_y + row_h });

            if (ImGui::IsMouseHoveringRect(bb.Min, bb.Max))
                blocks_window_drag = true;

            dl->AddRectFilled(bb.Min, bb.Max, ImGui::GetColorU32(colors::box), round);

            // Invisible InputText over the rounded box.
            ImGui::SetCursorScreenPos({ bb.Min.x + ui_scale::s(24.f), bb.GetCenter().y - fonts::size(text_font) * 0.55f });
            ImGui::PushItemWidth(bb.GetWidth() - ui_scale::s(24.f + 24.f + 8.f) - icon_size);
            ImGui::PushStyleVar(ImGuiStyleVar_FramePadding, { 0.f, 0.f });
            ImGui::PushStyleColor(ImGuiCol_FrameBg, { 0.f, 0.f, 0.f, 0.f });
            ImGui::PushStyleColor(ImGuiCol_FrameBgHovered, { 0.f, 0.f, 0.f, 0.f });
            ImGui::PushStyleColor(ImGuiCol_FrameBgActive, { 0.f, 0.f, 0.f, 0.f });
            ImGui::PushStyleColor(ImGuiCol_Text, colors::text);
            if (text_font)
                ImGui::PushFont(text_font);
            ImGui::InputText("##topbar_search", g_search.buf, IM_ARRAYSIZE(g_search.buf));
            const bool input_active  = ImGui::IsItemActive();
            const bool input_hovered = ImGui::IsItemHovered();
            if (text_font)
                ImGui::PopFont();
            ImGui::PopStyleColor(4);
            ImGui::PopStyleVar();
            ImGui::PopItemWidth();

            if (input_active || input_hovered)
                blocks_window_drag = true;

            // The placeholder counts every function the index knows about.
            if (text_font && !g_search.buf[0] && !input_active)
            {
                char hint[64];
                ImFormatString(hint, IM_ARRAYSIZE(hint), "explore %d functions...", search_index::total());
                dl->AddText(text_font, fonts::size(text_font),
                    { bb.Min.x + ui_scale::s(24.f), bb.GetCenter().y - fonts::size(text_font) * 0.5f },
                    ImGui::GetColorU32(colors::fg(0.25f)),
                    hint);
            }

            const ImRect search_icon({ bb.Max.x - ui_scale::s(24.f) - icon_size, bb.GetCenter().y - icon_size * 0.5f },
                                     { bb.Max.x - ui_scale::s(24.f), bb.GetCenter().y + icon_size * 0.5f });
            icons::draw(dl, "search", search_icon,
                ImGui::GetColorU32(colors::fg(0.6f)));

            // --- results dropdown ------------------------------------------
            const bool want_open = g_search.buf[0] && (input_active || g_search.active);
            g_search.alpha = anim::step(g_search.alpha, want_open ? 1.f : 0.f, 18.f);
            if (input_active)
                g_search.active = true;

            if (g_search.alpha > 0.02f && g_search.buf[0])
            {
                const search_index::entry* results[8] = {};
                const int count = search_index::find(g_search.buf, results, 8);

                const float item_h  = ui_scale::s(44.f);
                const float pad     = ui_scale::s(6.f);
                const float popup_h = count > 0
                    ? (float)count * item_h + pad * 2.f
                    : ui_scale::s(44.f);
                const ImRect popup(
                    { bb.Min.x, bb.Max.y + ui_scale::s(6.f) },
                    { bb.Max.x, bb.Max.y + ui_scale::s(6.f) + popup_h });

                ImDrawList* fg = ImGui::GetForegroundDrawList();
                const float a = g_search.alpha;
                fg->AddRectFilled(popup.Min, popup.Max,
                    ImGui::GetColorU32(ImVec4(colors::dropdown_bg.x, colors::dropdown_bg.y,
                        colors::dropdown_bg.z, colors::dropdown_bg.w * a)),
                    round);

                ImFont* label_font   = fonts::inter(18.f);
                ImFont* section_font = fonts::inter(13.f);
                g_search.hover_idx = -1;

                if (count == 0 && label_font)
                {
                    fg->AddText(label_font, fonts::size(label_font),
                        { popup.Min.x + ui_scale::s(24.f), popup.GetCenter().y - fonts::size(label_font) * 0.5f },
                        ImGui::GetColorU32(colors::fg(0.35f * a)),
                        "Nothing found");
                }

                for (int i = 0; i < count; ++i)
                {
                    const search_index::entry* e = results[i];
                    const ImRect item(
                        { popup.Min.x + pad, popup.Min.y + pad + (float)i * item_h },
                        { popup.Max.x - pad, popup.Min.y + pad + (float)(i + 1) * item_h });

                    const bool hovered = ImGui::IsMouseHoveringRect(item.Min, item.Max);
                    if (hovered)
                    {
                        g_search.hover_idx = i;
                        blocks_window_drag = true;
                        fg->AddRectFilled(item.Min, item.Max,
                            ImGui::GetColorU32(colors::wash(0.05f * a)),
                            ui_scale::s(8.f));
                    }

                    if (label_font)
                    {
                        fg->AddText(label_font, fonts::size(label_font),
                            { item.Min.x + ui_scale::s(18.f), item.Min.y + ui_scale::s(5.f) },
                            ImGui::GetColorU32(colors::fg((hovered ? 1.f : 0.85f) * a)),
                            e->label);
                    }
                    if (section_font)
                    {
                        char where[96];
                        ImFormatString(where, IM_ARRAYSIZE(where), "%s / %s",
                            e->page >= 0 && e->page < (int)IM_ARRAYSIZE(g_tabs) ? g_tabs[e->page].label : "?",
                            e->section);
                        fg->AddText(section_font, fonts::size(section_font),
                            { item.Min.x + ui_scale::s(18.f), item.Max.y - fonts::size(section_font) - ui_scale::s(5.f) },
                            ImGui::GetColorU32(colors::fg(0.4f * a)),
                            where);
                    }

                    if (hovered && ImGui::IsMouseClicked(ImGuiMouseButton_Left))
                    {
                        if (e->page >= 0 && e->page < (int)page::count_)
                            g_selected_tab = (page)e->page;
                        search_index::highlight(e->id);
                        g_search.buf[0]  = 0;
                        g_search.active  = false;
                        ImGui::ClearActiveID();
                    }
                }

                // Click anywhere else closes the dropdown.
                if (ImGui::IsMouseClicked(ImGuiMouseButton_Left)
                    && !popup.Contains(ImGui::GetIO().MousePos)
                    && !bb.Contains(ImGui::GetIO().MousePos))
                    g_search.active = false;
                if (ImGui::IsKeyPressed(ImGuiKey_Escape))
                {
                    g_search.active = false;
                    g_search.buf[0] = 0;
                }
            }
            else if (!input_active)
            {
                g_search.active = false;
            }
        }
    }

    void draw_placeholder_page(const char* title)
    {
        ImDrawList* dl = ImGui::GetWindowDrawList();
        ImFont* font = fonts::inter(20.f);
        if (!font)
            return;

        const ImVec2 avail = ImGui::GetContentRegionAvail();
        const ImVec2 origin = ImGui::GetCursorScreenPos();
        const float size = fonts::size(font);
        const ImVec2 ts = font->CalcTextSizeA(size, FLT_MAX, 0.f, title);
        dl->AddText(font, size,
            { origin.x + (avail.x - ts.x) * 0.5f, origin.y + (avail.y - ts.y) * 0.5f },
            ImGui::GetColorU32(colors::header_text), title);
    }

    void draw_active_page(bool& blocks_window_drag)
    {
        switch (g_selected_tab)
        {
        case page::aimbot:  aim_page::draw(g_aim, blocks_window_drag); break;
        case page::players: visuals_page::draw(g_visuals, blocks_window_drag); break;
        case page::world:   draw_placeholder_page("World — WIP"); break;
        case page::misc:    settings_page::draw(g_settings, blocks_window_drag); break;
        case page::config:  config_page::draw(g_config, blocks_window_drag); break;
        default: break;
        }
    }

    // One hidden pass over every page so the search registry knows about all
    // rows before the user has ever visited their tabs. Drawn far off-screen
    // at alpha 0; rows register through widgets' row_begin as a side effect.
    void run_index_pass()
    {
        if (g_index_built)
            return;

        ImGui::SetNextWindowPos({ -20000.f, -20000.f }, ImGuiCond_Always);
        ImGui::SetNextWindowSize(ImGui::GetIO().DisplaySize, ImGuiCond_Always);
        ImGui::PushStyleVar(ImGuiStyleVar_Alpha, 0.f);
        ImGui::Begin("##p8_index_pass", nullptr,
            ImGuiWindowFlags_NoTitleBar | ImGuiWindowFlags_NoResize |
            ImGuiWindowFlags_NoMove     | ImGuiWindowFlags_NoScrollbar |
            ImGuiWindowFlags_NoSavedSettings | ImGuiWindowFlags_NoBackground |
            ImGuiWindowFlags_NoInputs   | ImGuiWindowFlags_NoFocusOnAppearing);

        search_index::set_collecting(true);
        bool dummy = false;
        const page keep = g_selected_tab;
        for (int i = 0; i < (int)page::count_; ++i)
        {
            ImGui::PushID(i);
            search_index::set_page(i);
            search_index::set_section("General");
            g_selected_tab = (page)i;
            draw_active_page(dummy);
            ImGui::PopID();
        }
        g_selected_tab = keep;
        search_index::set_collecting(false);

        ImGui::End();
        ImGui::PopStyleVar();
        g_index_built = true;
    }
}

namespace gui
{
    void initialize(ID3D11Device* device, ID3D11DeviceContext* context)
    {
        http::start_monitor();
        g_settings.accent_color = colors::accent;
        glow_renderer::init(device);

        config_store::bind({ &g_aim, &g_visuals, &g_settings });
        config_store::sync(active_config_name());
        config_page::sync_from_cloud(g_config);

        // Stand-in for the game frame the overlay would normally sit on.
        // Next to the exe first, then the source tree, so a dev build picks it
        // up without a copy step.
        if (blur_renderer::init(device))
        {
            static const char* candidates[] = {
                "backdrop.jpg",
                "assets/backdrop.jpg",
                "../assets/backdrop.jpg",
            };
            for (const char* path : candidates)
            {
                if (blur_renderer::load_backdrop(context, path))
                    break;
            }
        }

        fonts::init();
        apply_style();

        profile_svc::init(device);
    }

    void shutdown()
    {
        config_page::shutdown();
        profile_svc::shutdown();
        http::stop_monitor();
        fonts::shutdown();
        blur_renderer::shutdown();
        glow_renderer::shutdown();
    }

    void select_tab(int index)
    {
        if (index >= 0 && index < (int)page::count_)
        {
            g_selected_tab = (page)index;
            g_prev_tab     = g_selected_tab;
        }
    }

    void panel_origin(float& x, float& y)
    {
        x = g_panel_origin.x;
        y = g_panel_origin.y;
    }

    void set_shot_full(bool on)
    {
        g_shot_full = on;
    }

    void prepare_frame()
    {
        // Auto-fit for the current resolution, with the user's manual % on top.
        ui_scale::set_display_size(ImGui::GetIO().DisplaySize);
        ui_scale::set_percent(g_settings_popup.dpi_scale);
        fonts::sync_ui_scale(ui_scale::factor);
        apply_scaled_style();

        profile_svc::poll();
        config_page::poll(g_config);

        if (http::is_offline() && http::offline_ms() >= network_grace_ms)
            ExitProcess(ERROR_NETWORK_UNREACHABLE);

        // Autosave: any changed value lands in the active config's file on its
        // own, debounced so a dragged slider writes once instead of per frame.
        if (g_settings.auto_save)
            config_store::autosave(active_config_name(), ImGui::GetIO().DeltaTime);
    }

    void render(HWND hwnd)
    {
        using namespace layout;

        if (ImGui::Shortcut(ImGuiMod_Ctrl | ImGuiKey_P, ImGuiInputFlags_RouteGlobal))
            screenshot::request();

        run_index_pass();
        search_index::tick(ImGui::GetIO().DeltaTime);

        const ImVec2 display = ImGui::GetIO().DisplaySize;

        static ImVec2 panel_pos = { -1.f, -1.f };
        static bool   panel_placed = false;
        static float  last_scale  = 1.f;

        const float panel_w = ui_scale::s(window_w);
        const float panel_h = ui_scale::s(window_h);
        const float sb_w    = ui_scale::s(sidebar_w);

        // Recenter only on first frame and on scale changes — a negative x is a
        // legitimate position once the panel has been dragged past the left edge.
        if (!panel_placed || last_scale != ui_scale::factor)
        {
            panel_pos.x = (display.x - panel_w) * 0.5f;
            panel_pos.y = (display.y - panel_h) * 0.5f;
            last_scale  = ui_scale::factor;
            panel_placed = true;
        }

        const ImVec2 panel_size = { panel_w, panel_h };
        const ImRect panel_bb(panel_pos, panel_pos + panel_size);
        g_panel_origin = panel_bb.Min;
        const float  panel_round = ui_scale::s(shell_round);

        if (screenshot::get().capture_frame)
        {
            const float outline = ui_scale::s(1.f);
            // Popups anchored beside the shell (menu settings, colour picker) fall
            // outside the panel crop, so --shotfull widens it to the whole display.
            if (g_shot_full)
                screenshot::set_bounds({ 0.f, 0.f }, display);
            else
                screenshot::set_bounds(
                    panel_bb.Min - ImVec2(outline, outline),
                    panel_bb.Max + ImVec2(outline, outline));
        }

        ImGui::SetNextWindowPos({ 0.f, 0.f }, ImGuiCond_Always);
        ImGui::SetNextWindowSize(display, ImGuiCond_Always);

        ImGui::Begin(main_window_id, nullptr,
            ImGuiWindowFlags_NoTitleBar | ImGuiWindowFlags_NoResize |
            ImGuiWindowFlags_NoMove     | ImGuiWindowFlags_NoScrollbar |
            ImGuiWindowFlags_NoCollapse | ImGuiWindowFlags_NoSavedSettings |
            ImGuiWindowFlags_NoBackground);

        ImDrawList* dl = ImGui::GetWindowDrawList();

        // The test backdrop always replaces the flat accent fill. The panel then
        // sits on a blurred copy of it, sampled from the same screen rectangle
        // so the frost lines up with whatever is behind the panel.
        const bool has_backdrop = blur_renderer::ready();
        ImVec2 bg_uv0 = { 0.f, 0.f };
        ImVec2 bg_uv1 = { 1.f, 1.f };
        if (has_backdrop)
        {
            // Cover fit: crop the longer axis instead of stretching the image.
            const ImVec2 src = blur_renderer::backdrop_size();
            const float  scale = ImMax(display.x / ImMax(src.x, 1.f), display.y / ImMax(src.y, 1.f));
            const ImVec2 covered = { src.x * scale, src.y * scale };
            const ImVec2 crop = { (covered.x - display.x) * 0.5f / ImMax(covered.x, 1.f),
                                  (covered.y - display.y) * 0.5f / ImMax(covered.y, 1.f) };
            bg_uv0 = crop;
            bg_uv1 = { 1.f - crop.x, 1.f - crop.y };
        }
        const auto screen_uv = [&](const ImVec2& p) {
            return ImVec2(
                ImLerp(bg_uv0.x, bg_uv1.x, ImClamp(p.x / ImMax(display.x, 1.f), 0.f, 1.f)),
                ImLerp(bg_uv0.y, bg_uv1.y, ImClamp(p.y / ImMax(display.y, 1.f), 0.f, 1.f)));
        };

        if (!screenshot::get().capture_frame)
        {
            if (has_backdrop)
                dl->AddImage(blur_renderer::backdrop(), { 0.f, 0.f }, display, bg_uv0, bg_uv1);
            else
                dl->AddRectFilled({ 0.f, 0.f }, display, ImGui::GetColorU32(colors::accent));
        }
        if (has_backdrop)
        {
            dl->AddImageRounded(blur_renderer::backdrop_blurred(), panel_bb.Min, panel_bb.Max,
                screen_uv(panel_bb.Min), screen_uv(panel_bb.Max), IM_COL32_WHITE, panel_round);
        }
        dl->AddRectFilled(panel_bb.Min, panel_bb.Max, ImGui::GetColorU32(colors::panel), panel_round);

        bool blocks_window_drag = false;

        const ImRect sidebar(panel_bb.Min, { panel_bb.Min.x + sb_w, panel_bb.Max.y });
        draw_sidebar(dl, sidebar, blocks_window_drag);

        const float topbar_h = ui_scale::s(layout::topbar_h);
        const ImRect topbar({ panel_bb.Min.x + sb_w, panel_bb.Min.y },
                           { panel_bb.Max.x, panel_bb.Min.y + topbar_h });
        draw_topbar(dl, topbar, blocks_window_drag);

        if (g_selected_tab != g_prev_tab)
        {
            g_tab_fade  = 0.f;
            g_prev_tab  = g_selected_tab;
        }

        // Tab transition: fade + a small upward slide, on the shared clock.
        g_tab_fade = anim::step(g_tab_fade, 1.f, 14.f);
        const float tab_slide = (1.f - g_tab_fade) * ui_scale::s(10.f);

        ImGui::SetCursorScreenPos({ panel_bb.Min.x + sb_w, panel_bb.Min.y + topbar_h + tab_slide });
        ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, { 0.f, 0.f });
        ImGui::BeginChild(layout::content_child_id,
            { panel_size.x - sb_w, panel_size.y - topbar_h },
            ImGuiChildFlags_None,
            ImGuiWindowFlags_NoBackground | ImGuiWindowFlags_NoScrollbar);

        ImGui::PushStyleVar(ImGuiStyleVar_Alpha, g_tab_fade);
        draw_active_page(blocks_window_drag);
        ImGui::PopStyleVar();

        // The share overlay dims the whole shell — sidebar and topbar included.
        // It must live inside the content child: the child's draw list renders
        // after the parent's, so drawing it from the parent would leave the
        // page content un-dimmed on top. The overlay widens its clip rect to
        // the panel bounds itself.
        if (config_page::share_open(g_config))
            config_page::draw_share_overlay(g_config, panel_bb.Min, panel_bb.Max, blocks_window_drag);

        ImGui::EndChild();
        ImGui::PopStyleVar();

        // Popups draw over the whole shell, anchored next to what opened them.
        // Auto Save appears both here and on the Misc page; the settings page
        // holds the value so autosave has one source of truth.
        g_settings_popup.auto_save = g_settings.auto_save;
        settings_popup::draw(g_settings_popup, blocks_window_drag);
        g_settings.auto_save = g_settings_popup.auto_save;
        widgets::draw_bind_popup(blocks_window_drag);
        widgets::draw_keybind_list(true);

        window_drag::handle_panel(panel_pos, panel_bb, blocks_window_drag);
        (void)hwnd;
        ImGui::End();
        draw_network_indicator();
    }
}
