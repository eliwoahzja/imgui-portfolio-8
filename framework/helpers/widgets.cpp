#include "widgets.h"

#include "core/blur_renderer.h"
#include "helpers/fonts.h"
#include "helpers/glow.h"
#include "helpers/icons.h"
#include "helpers/search_index.h"
#include "theme/anim.h"
#include "theme/colors.h"
#include "theme/layout.h"
#include "theme/ui_scale.h"

#include "imgui_internal.h"

#include <Windows.h>
#include <cmath>
#include <map>
#include <string>
#include <vector>

namespace
{
    struct anim_state
    {
        float active = 0.f;
        float hover  = 0.f;
        float check  = 0.f;
        float fill   = 0.f;
        float pulse  = 0.f;
        float open   = 0.f;
    };

    std::map<ImGuiID, anim_state> g_anims;

    // Every widget animation funnels through anim::lerp_t so the "Animation
    // speed" setting drives the whole UI uniformly (tabs, popups, rows).
    void smooth(float& v, float target, float speed)
    {
        v = ImLerp(v, target, anim::lerp_t(speed));
    }

    void ease(float& v, float target, float speed)
    {
        smooth(v, target, speed);
    }

    void draw_row_hover(ImDrawList* dl, const ImRect& row, float hover_t)
    {
        if (hover_t < 0.01f)
            return;

        dl->AddRectFilled(row.Min, row.Max,
            ImGui::GetColorU32(colors::wash(0.025f * hover_t)), ui_scale::s(layout::control_round));
    }

    ImVec2 text_pos_in_row(ImFont* font, float size, const ImRect& row, const char* text, float x, float y_nudge = 0.f)
    {
        const ImVec2 ts = font->CalcTextSizeA(size, FLT_MAX, 0.f, text);
        return { x, row.Min.y + (row.GetHeight() - ts.y) * 0.5f + y_nudge };
    }

    ImVec2 label_pos_in_row(ImFont* font, float size, const ImRect& row, const char* text, float x)
    {
        return text_pos_in_row(font, size, row, text, x, ui_scale::s(layout::row_label_y_nudge));
    }

    ImVec4 label_color(bool active, bool hovered)
    {
        if (active || hovered)
            return colors::text;
        return colors::text_muted;
    }

    ImRect toggle_rect(const ImRect& row)
    {
        // Snapped to whole pixels: a half-pixel track edge is what made the
        // switches look lopsided at fractional DPI factors.
        const float w  = ImFloor(ui_scale::s(layout::toggle_w) + 0.5f);
        const float h  = ImFloor(ui_scale::s(layout::toggle_h) + 0.5f);
        const float pad = ui_scale::s(layout::box_pad_x);
        const float right = ImFloor(row.Max.x - pad);
        const float top = ImFloor(row.GetCenter().y - h * 0.5f);
        return { { right - w, top }, { right, top + h } };
    }

    struct combo_popup
    {
        bool  open      = false;
        float alpha     = 0.f;
        float slide     = 0.f;
        float opened_at = 0.f;
    };

    std::map<std::string, combo_popup> g_combos;
    std::map<ImGuiID, bool> g_keybind_capture;

    // --- per-function binds (right click on any row) ---------------------
    // A bind can drive a bool, a slider value, a combo index or a multicombo
    // mask. The row that owns the id registers its kind + payload every frame.
    enum bind_kind { bind_bool = 0, bind_float, bind_combo, bind_mask };
    enum bind_mode { mode_hold = 0, mode_toggle, mode_always };

    struct bind_entry
    {
        std::string label;
        int  key           = 0;
        int  mode          = mode_hold;
        bool show_in_list  = true;
        bool active        = false;  // live state, driven by the key
        bool prev_down     = false;  // edge detection, kept per entry so the
                                     // vector may reallocate freely

        int  kind          = bind_bool;

        // bool payload
        bool target_value  = true;

        // float payload (e.g. FOV 30 -> 95)
        float value_on  = 0.f;
        float value_off = 0.f;
        float fmin = 0.f, fmax = 1.f;

        // combo payload (explicit item index)
        int index_on  = 0;
        int index_off = 0;

        // multicombo payload (explicit mask)
        unsigned mask_on  = 0;
        unsigned mask_off = 0;

        // item names for combo/multicombo, copied from the owning row
        std::vector<std::string> items;
    };

    // A function can carry any number of binds; the vector order is the order
    // they were added in and doubles as the edit index.
    std::map<std::string, std::vector<bind_entry>> g_binds;

    struct bind_popup_state
    {
        std::string id;
        int    index     = 0;
        bool   open      = false;
        float  alpha     = 0.f;
        ImVec2 anchor    = {};
        bool   capturing = false;
        float  opened_at = 0.f;
        bool   fresh     = false;   // created by "Add Bind": Cancel removes it
        bind_entry backup;          // restored by Cancel
    };

    bind_popup_state g_bind_popup;

    // Right-click menu that sits in front of the edit popup.
    struct bind_menu_state
    {
        std::string id;
        std::string label;
        bool   open      = false;
        float  alpha     = 0.f;
        ImVec2 anchor    = {};
        float  opened_at = 0.f;
    };

    bind_menu_state g_bind_menu;

    void open_bind_popup(const std::string& id, int index, const ImVec2& anchor, bool fresh)
    {
        auto it = g_binds.find(id);
        if (it == g_binds.end() || index < 0 || index >= (int)it->second.size())
            return;

        g_bind_popup.id        = id;
        g_bind_popup.index     = index;
        g_bind_popup.open      = true;
        g_bind_popup.anchor    = anchor;
        g_bind_popup.capturing = fresh;
        g_bind_popup.opened_at = (float)ImGui::GetTime();
        g_bind_popup.fresh     = fresh;
        g_bind_popup.backup    = it->second[index];
    }

    // Ctrl+click on a slider opens an inline numeric field for precise input.
    ImGuiID g_slider_edit_id     = 0;
    int     g_slider_edit_frames = 0;
    char    g_slider_edit_buf[32];

    const char* key_name(int key)
    {
        static char buf[32];
        if (key <= 0)
            return "None";

        switch (key)
        {
        case VK_LBUTTON: return "M1";
        case VK_RBUTTON: return "M2";
        case VK_MBUTTON: return "M3";
        case VK_XBUTTON1: return "M4";
        case VK_XBUTTON2: return "M5";
        case VK_SHIFT: return "Shift";
        case VK_CONTROL: return "Ctrl";
        case VK_MENU: return "Alt";
        case VK_SPACE: return "Space";
        case VK_TAB: return "Tab";
        case VK_ESCAPE: return "Esc";
        default:
            if (key >= 'A' && key <= 'Z') { buf[0] = (char)key; buf[1] = 0; return buf; }
            if (key >= '0' && key <= '9') { buf[0] = (char)key; buf[1] = 0; return buf; }
            buf[0] = 0;
            if (GetKeyNameTextA(MapVirtualKeyA(key, MAPVK_VK_TO_VSC) << 16, buf, 32) > 0)
                return buf;
            return "Key";
        }
    }

    void close_other_combos(const char* keep)
    {
        for (auto& [id, state] : g_combos)
        {
            if (id != keep)
                state.open = false;
        }
    }

    // Advances a bind's engaged state from its key + mode. Returns true when
    // the engaged state changed this frame (edge), so callers only write the
    // target value on transitions and do not fight manual edits.
    struct bind_tick { bool edge; bool engaged; };

    bind_tick tick_bind(bind_entry& bind)
    {
        if (bind.mode == mode_always)
        {
            const bool edge = !bind.active;
            bind.active = true;
            return { edge, true };
        }

        if (bind.key <= 0)
        {
            const bool edge = bind.active;
            bind.active = false;
            return { edge, false };
        }

        const bool down = (GetAsyncKeyState(bind.key) & 0x8000) != 0;
        const bool pressed = down && !bind.prev_down;
        bind.prev_down = down;

        bool engaged = bind.active;
        if (bind.mode == mode_hold)
            engaged = down;
        else if (pressed) // toggle
            engaged = !engaged;

        const bool edge = engaged != bind.active;
        bind.active = engaged;
        return { edge, engaged };
    }

    // Drives a bool through its bind, if one is set. Returns true when the
    // function is currently held down by the bind.
    bool apply_bind_bool(const char* id, bool* value)
    {
        auto it = g_binds.find(id);
        if (it == g_binds.end())
            return false;

        bool held = false;
        for (bind_entry& bind : it->second)
        {
            bind.kind = bind_bool;
            if (bind.mode != mode_always && bind.key <= 0)
                continue;

            const bind_tick t = tick_bind(bind);
            if (t.edge)
                *value = t.engaged ? bind.target_value : !bind.target_value;
            held |= (bind.mode == mode_hold && t.engaged);
        }
        return held;
    }

    // Drives a slider: engaged -> value_on, released -> whatever the slider
    // held before the bind kicked in.
    void apply_bind_float(const char* id, float* value, float min, float max)
    {
        auto it = g_binds.find(id);
        if (it == g_binds.end())
            return;

        for (bind_entry& bind : it->second)
        {
            if (bind.kind != bind_float)
            {
                // First frame owning this bind: seed the payload from the row.
                bind.kind      = bind_float;
                bind.fmin      = min;
                bind.fmax      = max;
                bind.value_off = *value;
                bind.value_on  = *value;
            }
            bind.fmin = min;
            bind.fmax = max;
            if (bind.mode != mode_always && bind.key <= 0)
                continue;

            const bind_tick t = tick_bind(bind);
            if (!t.edge)
                continue;

            if (t.engaged)
            {
                // The slider's own value is the "off" value — capture it on the
                // way in instead of asking the user for it a second time.
                bind.value_off = *value;
                *value = ImClamp(bind.value_on, min, max);
            }
            else
            {
                *value = ImClamp(bind.value_off, min, max);
            }
        }
    }

    // Drives a combo: engaged -> index_on, released -> index_off.
    void apply_bind_combo(const char* id, int* index, const char* const items[], int count)
    {
        auto it = g_binds.find(id);
        if (it == g_binds.end())
            return;

        for (bind_entry& bind : it->second)
        {
            if (bind.kind != bind_combo)
            {
                bind.kind      = bind_combo;
                bind.index_off = *index;
                bind.index_on  = *index;
            }
            bind.items.assign(items, items + count);
            if (bind.mode != mode_always && bind.key <= 0)
                continue;

            const bind_tick t = tick_bind(bind);
            if (!t.edge)
                continue;

            if (t.engaged)
            {
                bind.index_off = *index;
                *index = ImClamp(bind.index_on, 0, count - 1);
            }
            else
            {
                *index = ImClamp(bind.index_off, 0, count - 1);
            }
        }
    }

    // Drives a multicombo: engaged -> mask_on, released -> mask_off.
    void apply_bind_mask(const char* id, unsigned* mask, const char* const items[], int count)
    {
        auto it = g_binds.find(id);
        if (it == g_binds.end())
            return;

        for (bind_entry& bind : it->second)
        {
            if (bind.kind != bind_mask)
            {
                bind.kind     = bind_mask;
                bind.mask_off = *mask;
                bind.mask_on  = *mask;
            }
            bind.items.assign(items, items + count);
            if (bind.mode != mode_always && bind.key <= 0)
                continue;

            const bind_tick t = tick_bind(bind);
            if (!t.edge)
                continue;

            if (t.engaged)
            {
                bind.mask_off = *mask;
                *mask = bind.mask_on;
            }
            else
            {
                *mask = bind.mask_off;
            }
        }
    }

    // Shared entry point for every row: search registration, the search jump
    // highlight, the right-click bind popup and the little bind badge.
    void row_begin(const char* id, const char* label, const ImRect& row, bool& blocks_drag)
    {
        search_index::note(id, label);

        ImDrawList* dl = ImGui::GetWindowDrawList();
        const float round = ui_scale::s(layout::control_round);

        const float hl = search_index::highlight_alpha(id);
        if (hl > 0.005f)
        {
            dl->AddRectFilled(row.Min, row.Max,
                ImGui::GetColorU32(ImVec4(colors::accent.x, colors::accent.y, colors::accent.z, 0.16f * hl)),
                round);
            dl->AddRect(row.Min, row.Max,
                ImGui::GetColorU32(ImVec4(colors::accent.x, colors::accent.y, colors::accent.z, 0.55f * hl)),
                round, 0, ui_scale::s(1.f));
        }

        // Right click opens the little menu: add another bind, edit one of the
        // existing ones, or wipe them all.
        if (ImGui::IsMouseHoveringRect(row.Min, row.Max)
            && ImGui::IsMouseClicked(ImGuiMouseButton_Right))
        {
            g_bind_menu.id        = id;
            g_bind_menu.label     = label && label[0] ? label : id;
            g_bind_menu.open      = true;
            g_bind_menu.anchor    = ImGui::GetIO().MousePos;
            g_bind_menu.opened_at = (float)ImGui::GetTime();
            g_bind_popup.open     = false;
            blocks_drag = true;
        }

        auto it = g_binds.find(id);
        if (it == g_binds.end() || it->second.empty())
            return;

        ImFont* font = fonts::inter(layout::row_label_font);
        ImFont* badge_font = fonts::inter(11.f);
        if (!font || !badge_font || !label)
            return;

        const float label_w = font->CalcTextSizeA(fonts::size(font), FLT_MAX, 0.f, label).x;
        const float bsize = fonts::size(badge_font);
        const float pad_x = ui_scale::s(5.f);
        const float h = ImFloor(bsize + ui_scale::s(4.f));

        // One badge per bind, laid out left to right after the label.
        float x = ImFloor(row.Min.x + ui_scale::s(layout::box_pad_x) + label_w + ui_scale::s(8.f));
        for (const bind_entry& bind : it->second)
        {
            if (bind.key <= 0 && bind.mode != mode_always)
                continue;

            const char* key_text = bind.mode == mode_always ? "Always" : key_name(bind.key);
            const float text_w = badge_font->CalcTextSizeA(bsize, FLT_MAX, 0.f, key_text).x;

            const ImVec2 min = { x, ImFloor(row.GetCenter().y - h * 0.5f) };
            const ImVec2 max = { min.x + text_w + pad_x * 2.f, min.y + h };
            if (max.x > row.Max.x - ui_scale::s(layout::box_pad_x))
                break;

            const float mix = bind.active ? 1.f : 0.f;
            dl->AddRectFilled(min, max,
                ImGui::GetColorU32(ImVec4(colors::accent.x, colors::accent.y, colors::accent.z, 0.24f + 0.30f * mix)),
                ui_scale::s(3.f));
            dl->AddText(badge_font, bsize, { min.x + pad_x, min.y + ui_scale::s(2.f) },
                ImGui::GetColorU32(ImLerp(colors::text_muted, colors::text, 0.55f + 0.45f * mix)),
                key_text);

            x = max.x + ui_scale::s(4.f);
        }
    }

    bool combo_dropdown(const char* id, const ImRect& anchor, int* index, const char* const items[], int count, bool& blocks_drag)
    {
        combo_popup& state = g_combos[id];
        smooth(state.alpha, state.open ? 1.f : 0.f, 18.f);
        smooth(state.slide, state.open ? 0.f : 8.f, 20.f);
        if (state.alpha < 0.01f && !state.open)
            return false;

        const float item_h = ui_scale::s(layout::dropdown_item_h);
        const float popup_h = (float)count * item_h + ui_scale::s(8.f);
        ImGui::SetNextWindowPos({ anchor.Min.x, anchor.Max.y + ui_scale::s(4.f) + state.slide }, ImGuiCond_Always);
        ImGui::SetNextWindowSize({ anchor.GetWidth(), popup_h }, ImGuiCond_Always);

        ImGui::PushStyleVar(ImGuiStyleVar_WindowRounding, ui_scale::s(layout::combo_round));
        ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, { ui_scale::s(4.f), ui_scale::s(4.f) });
        ImGui::PushStyleVar(ImGuiStyleVar_Alpha, state.alpha * state.alpha);
        ImGui::PushStyleColor(ImGuiCol_WindowBg, { 0.f, 0.f, 0.f, 0.f });

        char win_id[64];
        ImFormatString(win_id, IM_ARRAYSIZE(win_id), "##combo_%s", id);

        if (!ImGui::Begin(win_id, nullptr,
            ImGuiWindowFlags_NoTitleBar | ImGuiWindowFlags_NoResize | ImGuiWindowFlags_NoMove |
            ImGuiWindowFlags_NoScrollbar | ImGuiWindowFlags_NoSavedSettings | ImGuiWindowFlags_NoNav))
        {
            ImGui::End();
            ImGui::PopStyleColor();
            ImGui::PopStyleVar(3);
            return false;
        }

        blocks_drag = true;

        const ImRect panel = ImGui::GetCurrentWindow()->Rect();
        ImDrawList* wdl = ImGui::GetWindowDrawList();
        blur_renderer::draw_live_blur(wdl, panel.Min, panel.Max, ui_scale::s(layout::combo_round));
        wdl->AddRectFilled(panel.Min, panel.Max, ImGui::GetColorU32(colors::dropdown_bg), ui_scale::s(layout::combo_round));

        if (state.alpha > 0.5f && ImGui::IsMouseClicked(ImGuiMouseButton_Left)
            && ImGui::GetTime() - state.opened_at > 0.05f)
        {
            const ImVec2 mouse = ImGui::GetIO().MousePos;
            if (!panel.Contains(mouse) && !anchor.Contains(mouse))
                state.open = false;
        }

        // Dropdown items live in 28px rows, so they keep the smaller face.
        ImFont* font = fonts::inter(14.f);
        const float item_text = fonts::size(font);
        bool changed = false;

        for (int i = 0; i < count; ++i)
        {
            const ImVec2 pos = ImGui::GetCursorScreenPos();
            const ImRect item_bb(pos, pos + ImVec2(anchor.GetWidth() - ui_scale::s(8.f), item_h));
            ImGuiID item_id = ImGui::GetID((void*)(intptr_t)(i + 1));
            ImGui::ItemSize(item_bb);
            ImGui::ItemAdd(item_bb, item_id);

            bool hovered = false, held = false;
            const bool pressed = ImGui::ButtonBehavior(item_bb, item_id, &hovered, &held);
            if (hovered)
                blocks_drag = true;

            anim_state& item_anim = g_anims[item_id];
            smooth(item_anim.hover, hovered ? 1.f : 0.f, 20.f);

            const bool selected = (*index == i);
            if (hovered || selected || item_anim.hover > 0.01f)
            {
                const ImVec4 bg = selected
                    ? ImVec4(colors::accent.x, colors::accent.y, colors::accent.z, 0.14f)
                    : colors::wash(0.03f + item_anim.hover * 0.03f);
                wdl->AddRectFilled(item_bb.Min, item_bb.Max, ImGui::GetColorU32(bg), ui_scale::s(layout::combo_round));
            }

            if (font)
            {
                wdl->AddText(font, item_text,
                    text_pos_in_row(font, item_text, item_bb, items[i], item_bb.Min.x + ui_scale::s(10.f)),
                    ImGui::GetColorU32(label_color(selected, hovered)),
                    items[i]);
            }

            if (selected)
            {
                const ImRect icon_rect(
                    { item_bb.Max.x - ui_scale::s(24.f), item_bb.Min.y },
                    { item_bb.Max.x - ui_scale::s(6.f), item_bb.Max.y });
                icons::draw(wdl, "check", icon_rect,
                    ImGui::GetColorU32(hovered ? colors::text : colors::text_muted));
            }

            if (pressed)
            {
                *index = i;
                state.open = false;
                changed = true;
            }

            ImGui::SetCursorScreenPos({ pos.x, pos.y + item_h });
        }

        ImGui::End();
        ImGui::PopStyleColor();
        ImGui::PopStyleVar(3);
        return changed;
    }

    // Same dropdown, but every item is a checkbox bit and the popup stays
    // open so several items can be picked in one go.
    bool multicombo_dropdown(const char* id, const ImRect& anchor, unsigned* mask, const char* const items[], int count, bool& blocks_drag)
    {
        combo_popup& state = g_combos[id];
        smooth(state.alpha, state.open ? 1.f : 0.f, 18.f);
        smooth(state.slide, state.open ? 0.f : 8.f, 20.f);
        if (state.alpha < 0.01f && !state.open)
            return false;

        const float item_h = ui_scale::s(layout::dropdown_item_h);
        const float popup_h = (float)count * item_h + ui_scale::s(8.f);
        ImGui::SetNextWindowPos({ anchor.Min.x, anchor.Max.y + ui_scale::s(4.f) + state.slide }, ImGuiCond_Always);
        ImGui::SetNextWindowSize({ anchor.GetWidth(), popup_h }, ImGuiCond_Always);

        ImGui::PushStyleVar(ImGuiStyleVar_WindowRounding, ui_scale::s(layout::combo_round));
        ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, { ui_scale::s(4.f), ui_scale::s(4.f) });
        ImGui::PushStyleVar(ImGuiStyleVar_Alpha, state.alpha * state.alpha);
        ImGui::PushStyleColor(ImGuiCol_WindowBg, { 0.f, 0.f, 0.f, 0.f });

        char win_id[64];
        ImFormatString(win_id, IM_ARRAYSIZE(win_id), "##mcombo_%s", id);

        if (!ImGui::Begin(win_id, nullptr,
            ImGuiWindowFlags_NoTitleBar | ImGuiWindowFlags_NoResize | ImGuiWindowFlags_NoMove |
            ImGuiWindowFlags_NoScrollbar | ImGuiWindowFlags_NoSavedSettings | ImGuiWindowFlags_NoNav))
        {
            ImGui::End();
            ImGui::PopStyleColor();
            ImGui::PopStyleVar(3);
            return false;
        }

        blocks_drag = true;

        const ImRect panel = ImGui::GetCurrentWindow()->Rect();
        ImDrawList* wdl = ImGui::GetWindowDrawList();
        blur_renderer::draw_live_blur(wdl, panel.Min, panel.Max, ui_scale::s(layout::combo_round));
        wdl->AddRectFilled(panel.Min, panel.Max, ImGui::GetColorU32(colors::dropdown_bg), ui_scale::s(layout::combo_round));

        if (state.alpha > 0.5f && ImGui::IsMouseClicked(ImGuiMouseButton_Left)
            && ImGui::GetTime() - state.opened_at > 0.05f)
        {
            const ImVec2 mouse = ImGui::GetIO().MousePos;
            if (!panel.Contains(mouse) && !anchor.Contains(mouse))
                state.open = false;
        }

        ImFont* font = fonts::inter(14.f);
        const float item_text = fonts::size(font);
        bool changed = false;

        for (int i = 0; i < count; ++i)
        {
            const ImVec2 pos = ImGui::GetCursorScreenPos();
            const ImRect item_bb(pos, pos + ImVec2(anchor.GetWidth() - ui_scale::s(8.f), item_h));
            ImGuiID item_id = ImGui::GetID((void*)(intptr_t)(i + 1));
            ImGui::ItemSize(item_bb);
            ImGui::ItemAdd(item_bb, item_id);

            bool hovered = false, held = false;
            const bool pressed = ImGui::ButtonBehavior(item_bb, item_id, &hovered, &held);
            if (hovered)
                blocks_drag = true;

            anim_state& item_anim = g_anims[item_id];
            smooth(item_anim.hover, hovered ? 1.f : 0.f, 20.f);

            const bool selected = (*mask & (1u << i)) != 0;
            if (hovered || selected || item_anim.hover > 0.01f)
            {
                const ImVec4 bg = selected
                    ? ImVec4(colors::accent.x, colors::accent.y, colors::accent.z, 0.14f)
                    : colors::wash(0.03f + item_anim.hover * 0.03f);
                wdl->AddRectFilled(item_bb.Min, item_bb.Max, ImGui::GetColorU32(bg), ui_scale::s(layout::combo_round));
            }

            if (font)
            {
                wdl->AddText(font, item_text,
                    text_pos_in_row(font, item_text, item_bb, items[i], item_bb.Min.x + ui_scale::s(10.f)),
                    ImGui::GetColorU32(label_color(selected, hovered)),
                    items[i]);
            }

            if (selected)
            {
                const ImRect icon_rect(
                    { item_bb.Max.x - ui_scale::s(24.f), item_bb.Min.y },
                    { item_bb.Max.x - ui_scale::s(6.f), item_bb.Max.y });
                icons::draw(wdl, "check", icon_rect,
                    ImGui::GetColorU32(hovered ? colors::accent : colors::text_muted));
            }

            if (pressed)
            {
                *mask ^= (1u << i);   // toggle the bit, keep the popup open
                changed = true;
            }

            ImGui::SetCursorScreenPos({ pos.x, pos.y + item_h });
        }

        ImGui::End();
        ImGui::PopStyleColor();
        ImGui::PopStyleVar(3);
        return changed;
    }
}

namespace widgets
{
    void draw_section_header(ImDrawList* dl, const ImVec2& pos, const char* title)
    {
        ImFont* font = fonts::inter(layout::section_header_h, true);
        if (!font)
            font = ImGui::GetFont();

        if (font)
            dl->AddText(font, fonts::size(font), pos, ImGui::GetColorU32(colors::header_text), title);
    }

    ImRect draw_settings_box(ImDrawList* dl, const ImVec2& pos, float width, int rows)
    {
        const float height = ui_scale::s(layout::settings_box_height(rows));
        const ImRect box(pos, pos + ImVec2(width, height));
        dl->AddRectFilled(box.Min, box.Max, ImGui::GetColorU32(colors::box),
            ui_scale::s(layout::box_round));
        return box;
    }

    bool checkbox_row(const char* id, const char* label, bool* value, const ImRect& row, bool& blocks_drag)
    {
        row_begin(id, label, row, blocks_drag);
        apply_bind_bool(id, value);

        ImGuiID gid = ImGui::GetID(id);
        ImGui::ItemAdd(row, gid);

        bool hovered = false, held = false;
        const bool pressed = ImGui::ButtonBehavior(row, gid, &hovered, &held);
        if (pressed)
            *value = !*value;

        if (hovered || held)
            blocks_drag = true;

        anim_state& anim = g_anims[gid];
        ease(anim.active, *value ? 1.f : 0.f, 16.f);
        ease(anim.hover, hovered || held ? 1.f : 0.f, 24.f);

        ImDrawList* dl = ImGui::GetWindowDrawList();
        draw_row_hover(dl, row, anim.hover * 0.65f);

        ImFont* font = fonts::inter(layout::row_label_font);
        const float label_size = fonts::size(font);
        if (font)
        {
            dl->AddText(font, label_size,
                label_pos_in_row(font, label_size, row, label, row.Min.x + ui_scale::s(layout::box_pad_x)),
                ImGui::GetColorU32(label_color(*value, hovered)),
                label);
        }

        const ImRect toggle = toggle_rect(row);
        const float toggle_round = ui_scale::s(layout::toggle_round);

        // Classic Portfolio #8 switch: dark pill track, accent glow when on.
        const ImVec4 track_bg = *value
            ? colors::control
            : ImLerp(colors::control, colors::control_hover, anim.hover);
        dl->AddRectFilled(toggle.Min, toggle.Max, ImGui::GetColorU32(track_bg), toggle_round);

        if (anim.active > 0.01f)
        {
            dl->AddRectFilled(toggle.Min, toggle.Max,
                ImGui::GetColorU32(ImVec4(
                    colors::accent.x, colors::accent.y, colors::accent.z, 0.10f * anim.active)),
                toggle_round);
            glow::draw_accent_rect(dl, toggle, toggle_round, anim.active * 0.55f);
        }

        const ImVec4 knob_off = ImLerp(colors::text_muted, colors::circle_checkbox_hover, anim.hover);
        const ImVec4 knob_col = ImLerp(knob_off, colors::accent, anim.active);
        const float knob_x = toggle.Min.x + ui_scale::s(layout::toggle_knob_inset)
            + ui_scale::s(layout::toggle_knob_travel) * anim.active;
        dl->AddCircleFilled({ knob_x, toggle.GetCenter().y }, ui_scale::s(layout::toggle_knob_r),
            ImGui::GetColorU32(knob_col), 24);

        return pressed;
    }

    bool slider_row(const char* id, const char* label, float* value, float min, float max, const ImRect& row, bool& blocks_drag)
    {
        const float slider_w    = ui_scale::s(layout::slider_w);
        const float right       = row.Max.x - ui_scale::s(layout::box_pad_x);
        // Classic layout: the track hugs the right edge, the value sits to its left.
        const float track_right = right;
        const float track_left  = track_right - slider_w;

        const ImRect track_bb(
            { track_left, row.GetCenter().y - ui_scale::s(layout::slider_h) * 0.5f },
            { track_right, row.GetCenter().y + ui_scale::s(layout::slider_h) * 0.5f });
        // Grab zone is taller than the 6px visual so it is easy to hit.
        const ImRect grab_bb(
            { track_left, row.Min.y },
            { track_right, row.Max.y });

        row_begin(id, label, row, blocks_drag);
        apply_bind_float(id, value, min, max);

        ImGuiID gid = ImGui::GetID(id);
        ImGui::ItemAdd(row, gid);

        const bool row_hovered = ImGui::IsMouseHoveringRect(row.Min, row.Max);

        bool hovered = false, held = false;
        ImGui::ButtonBehavior(grab_bb, gid, &hovered, &held);
        if (hovered || held || row_hovered)
            blocks_drag = true;

        const bool editing = (g_slider_edit_id == gid);

        // Ctrl+click: switch to precise keyboard input instead of dragging.
        if (hovered && ImGui::IsMouseClicked(ImGuiMouseButton_Left) && ImGui::GetIO().KeyCtrl)
        {
            g_slider_edit_id     = gid;
            g_slider_edit_frames = 0;
            ImFormatString(g_slider_edit_buf, IM_ARRAYSIZE(g_slider_edit_buf), "%.0f", *value);
            ImGui::ClearActiveID();
        }

        // Absolute positioning: press or drag anywhere on the bar sets the
        // value from the cursor position directly.
        if (held && !editing)
        {
            const float t = ImClamp(
                (ImGui::GetIO().MousePos.x - track_left) / ImMax(track_bb.GetWidth(), 1.f),
                0.f, 1.f);
            *value = min + t * (max - min);
        }

        anim_state& anim = g_anims[gid];
        ease(anim.hover, hovered || held || row_hovered ? 1.f : 0.f, 18.f);
        const float target_t = ImClamp((*value - min) / (max - min), 0.f, 1.f);
        ease(anim.fill, target_t, held ? 28.f : 16.f);

        ImDrawList* dl = ImGui::GetWindowDrawList();
        draw_row_hover(dl, row, anim.hover * 0.55f);

        ImFont* font = fonts::inter(layout::row_label_font);
        const float label_size = fonts::size(font);
        const bool active = *value > min + 0.01f;
        if (font)
        {
            dl->AddText(font, label_size,
                label_pos_in_row(font, label_size, row, label, row.Min.x + ui_scale::s(layout::box_pad_x)),
                ImGui::GetColorU32(label_color(active, row_hovered || hovered || held)),
                label);
        }

        const float fill_w = track_bb.GetWidth() * anim.fill;
        const float cy = track_bb.GetCenter().y;
        const ImVec4 track_col = ImLerp(colors::control, colors::control_hover, anim.hover * 0.35f);
        const float track_h = ui_scale::s(layout::slider_track_h);
        const float slider_h = ui_scale::s(layout::slider_h);

        // Classic slider: hairline resting track, thicker accent fill with glow.
        dl->AddRectFilled(
            { track_bb.Min.x, cy - track_h * 0.5f },
            { track_bb.Max.x, cy + track_h * 0.5f },
            ImGui::GetColorU32(track_col), track_h * 0.5f);

        if (fill_w > 0.5f)
        {
            const ImRect fill_bb(
                { track_bb.Min.x, cy - slider_h * 0.5f },
                { track_bb.Min.x + fill_w, cy + slider_h * 0.5f });

            glow::draw_accent_rect(dl, fill_bb, slider_h * 0.5f, 0.45f + anim.fill * 0.40f);

            dl->AddRectFilled(fill_bb.Min, fill_bb.Max,
                ImGui::GetColorU32(colors::accent), slider_h * 0.5f);
        }

        bool changed = held;

        if (editing)
        {
            // Inline numeric field replaces the value text while active.
            const float field_w = ui_scale::s(48.f);
            const float field_h = ui_scale::s(20.f);
            const ImVec2 fmin = { track_left - field_w - ui_scale::s(8.f), cy - field_h * 0.5f };

            dl->AddRectFilled(fmin, fmin + ImVec2(field_w, field_h),
                ImGui::GetColorU32(colors::control_hover), ui_scale::s(3.f));
            dl->AddRect(fmin, fmin + ImVec2(field_w, field_h),
                ImGui::GetColorU32(ImVec4(colors::accent.x, colors::accent.y, colors::accent.z, 0.6f)),
                ui_scale::s(3.f), 0, ui_scale::s(1.f));

            ImGui::SetCursorScreenPos({ fmin.x + ui_scale::s(4.f), fmin.y + ui_scale::s(2.f) });
            ImGui::PushItemWidth(field_w - ui_scale::s(8.f));
            ImGui::PushStyleVar(ImGuiStyleVar_FramePadding, { 0.f, 0.f });
            ImGui::PushStyleColor(ImGuiCol_FrameBg, ImVec4(0.f, 0.f, 0.f, 0.f));
            ImGui::PushStyleColor(ImGuiCol_Text, colors::text);

            char input_id[64];
            ImFormatString(input_id, IM_ARRAYSIZE(input_id), "##sledit_%s", id);
            if (g_slider_edit_frames == 0)
                ImGui::SetKeyboardFocusHere();

            const bool entered = ImGui::InputText(input_id, g_slider_edit_buf,
                IM_ARRAYSIZE(g_slider_edit_buf),
                ImGuiInputTextFlags_CharsDecimal | ImGuiInputTextFlags_EnterReturnsTrue
                | ImGuiInputTextFlags_AutoSelectAll);
            const bool input_active = ImGui::IsItemActive();
            if (input_active)
                blocks_drag = true;

            ImGui::PopStyleColor(2);
            ImGui::PopStyleVar();
            ImGui::PopItemWidth();

            // Commit on Enter or when focus leaves; cancel on Esc.
            if (entered || (g_slider_edit_frames > 1 && !input_active))
            {
                *value = ImClamp((float)atof(g_slider_edit_buf), min, max);
                g_slider_edit_id = 0;
                changed = true;
            }
            if (ImGui::IsKeyPressed(ImGuiKey_Escape))
                g_slider_edit_id = 0;

            ++g_slider_edit_frames;
        }
        else
        {
            // Value text sits right next to the track.
            char buf[16];
            ImFormatString(buf, IM_ARRAYSIZE(buf), "%.0f", *value);
            if (font)
            {
                const ImVec2 ts = font->CalcTextSizeA(label_size, FLT_MAX, 0.f, buf);
                const float value_x = track_left - ts.x - ui_scale::s(10.f);
                dl->AddText(font, label_size,
                    text_pos_in_row(font, label_size, row, buf, value_x),
                    ImGui::GetColorU32(ImLerp(colors::text_muted, colors::text, anim.hover * 0.5f + (held ? 0.5f : 0.f))),
                    buf);
            }
        }

        return changed;
    }

    bool combo_row(const char* id, const char* label, int* index, const char* const items[], int count, const ImRect& row, bool& blocks_drag)
    {
        const float combo_w = ui_scale::s(layout::combo_w);
        const float combo_h = ui_scale::s(layout::combo_h);
        const float right = row.Max.x - ui_scale::s(layout::box_pad_x);
        const ImRect btn(
            { right - combo_w, row.GetCenter().y - combo_h * 0.5f },
            { right, row.GetCenter().y + combo_h * 0.5f });

        row_begin(id, label, row, blocks_drag);
        apply_bind_combo(id, index, items, count);

        ImGuiID gid = ImGui::GetID(id);
        ImGui::ItemAdd(row, gid);

        const bool row_hovered = ImGui::IsMouseHoveringRect(row.Min, row.Max);

        bool hovered = false, held = false;
        const bool pressed = ImGui::ButtonBehavior(btn, gid, &hovered, &held);
        if (hovered || held || row_hovered)
            blocks_drag = true;

        combo_popup& popup = g_combos[id];
        if (pressed)
        {
            close_other_combos(id);
            popup.open = !popup.open;
            popup.opened_at = (float)ImGui::GetTime();
        }

        anim_state& anim = g_anims[gid];
        ease(anim.hover, hovered || popup.open || row_hovered ? 1.f : 0.f, 18.f);
        ease(anim.open, popup.open ? 1.f : 0.f, 20.f);

        ImDrawList* dl = ImGui::GetWindowDrawList();
        draw_row_hover(dl, row, anim.hover * 0.5f);

        ImFont* font = fonts::inter(layout::row_label_font);
        const float label_size = fonts::size(font);
        if (font)
        {
            dl->AddText(font, label_size,
                label_pos_in_row(font, label_size, row, label, row.Min.x + ui_scale::s(layout::box_pad_x)),
                ImGui::GetColorU32(label_color(false, row_hovered || hovered || popup.open)),
                label);
        }

        const ImVec4 btn_col = ImLerp(colors::control, colors::control_hover, anim.hover);
        dl->AddRectFilled(btn.Min, btn.Max, ImGui::GetColorU32(btn_col), ui_scale::s(layout::combo_round));

        if (anim.open > 0.02f)
        {
            dl->AddRect(btn.Min, btn.Max,
                ImGui::GetColorU32(ImVec4(colors::accent.x, colors::accent.y, colors::accent.z, 0.35f * anim.open)),
                ui_scale::s(layout::combo_round), 0, 1.f);
        }

        const char* current = (*index >= 0 && *index < count) ? items[*index] : "";
        if (font && current)
        {
            dl->AddText(font, label_size,
                text_pos_in_row(font, label_size, btn, current, btn.Min.x + ui_scale::s(10.f)),
                ImGui::GetColorU32(label_color(true, hovered || popup.open)),
                current);
        }

        {
            const float icon_side = ui_scale::s(14.f);
            // The chevron drifts down a touch while the popup opens.
            const float nudge = anim.open * ui_scale::s(1.5f);
            const ImVec2 icon_min = {
                btn.Max.x - icon_side - ui_scale::s(8.f),
                btn.GetCenter().y - icon_side * 0.5f + nudge };
            icons::draw(dl, "chevron-down",
                ImRect(icon_min, { icon_min.x + icon_side, icon_min.y + icon_side }),
                ImGui::GetColorU32(ImLerp(colors::text_muted, colors::accent, anim.open * 0.6f)));
        }

        return combo_dropdown(id, btn, index, items, count, blocks_drag);
    }

    bool multicombo_row(const char* id, const char* label, unsigned* mask, const char* const items[], int count, const ImRect& row, bool& blocks_drag)
    {
        const float combo_w = ui_scale::s(layout::combo_w);
        const float combo_h = ui_scale::s(layout::combo_h);
        const float right = row.Max.x - ui_scale::s(layout::box_pad_x);
        const ImRect btn(
            { right - combo_w, row.GetCenter().y - combo_h * 0.5f },
            { right, row.GetCenter().y + combo_h * 0.5f });

        row_begin(id, label, row, blocks_drag);
        apply_bind_mask(id, mask, items, count);

        ImGuiID gid = ImGui::GetID(id);
        ImGui::ItemAdd(row, gid);

        const bool row_hovered = ImGui::IsMouseHoveringRect(row.Min, row.Max);

        bool hovered = false, held = false;
        const bool pressed = ImGui::ButtonBehavior(btn, gid, &hovered, &held);
        if (hovered || held || row_hovered)
            blocks_drag = true;

        combo_popup& popup = g_combos[id];
        if (pressed)
        {
            close_other_combos(id);
            popup.open = !popup.open;
            popup.opened_at = (float)ImGui::GetTime();
        }

        anim_state& anim = g_anims[gid];
        ease(anim.hover, hovered || popup.open || row_hovered ? 1.f : 0.f, 18.f);
        ease(anim.open, popup.open ? 1.f : 0.f, 20.f);

        ImDrawList* dl = ImGui::GetWindowDrawList();
        draw_row_hover(dl, row, anim.hover * 0.5f);

        ImFont* font = fonts::inter(layout::row_label_font);
        const float label_size = fonts::size(font);
        if (font)
        {
            dl->AddText(font, label_size,
                label_pos_in_row(font, label_size, row, label, row.Min.x + ui_scale::s(layout::box_pad_x)),
                ImGui::GetColorU32(label_color(false, row_hovered || hovered || popup.open)),
                label);
        }

        const ImVec4 btn_col = ImLerp(colors::control, colors::control_hover, anim.hover);
        dl->AddRectFilled(btn.Min, btn.Max, ImGui::GetColorU32(btn_col), ui_scale::s(layout::combo_round));

        if (anim.open > 0.02f)
        {
            dl->AddRect(btn.Min, btn.Max,
                ImGui::GetColorU32(ImVec4(colors::accent.x, colors::accent.y, colors::accent.z, 0.35f * anim.open)),
                ui_scale::s(layout::combo_round), 0, 1.f);
        }

        // Preview: "First, Second" then "+N" once the names stop fitting.
        char preview[64];
        {
            int selected = 0;
            preview[0] = 0;
            const float avail = btn.GetWidth() - ui_scale::s(44.f);
            for (int i = 0; i < count; ++i)
            {
                if (!(*mask & (1u << i)))
                    continue;
                ++selected;

                char attempt[64];
                if (preview[0])
                    ImFormatString(attempt, IM_ARRAYSIZE(attempt), "%s, %s", preview, items[i]);
                else
                    ImFormatString(attempt, IM_ARRAYSIZE(attempt), "%s", items[i]);

                if (font && font->CalcTextSizeA(label_size, FLT_MAX, 0.f, attempt).x <= avail)
                {
                    ImFormatString(preview, IM_ARRAYSIZE(preview), "%s", attempt);
                }
                else
                {
                    // Count everything that did not fit.
                    int shown = 0;
                    for (int j = 0; j < i; ++j)
                        if (*mask & (1u << j))
                            ++shown;
                    int rest = 0;
                    for (int j = i; j < count; ++j)
                        if (*mask & (1u << j))
                            ++rest;
                    if (shown > 0)
                        ImFormatString(attempt, IM_ARRAYSIZE(attempt), "%s +%d", preview, rest);
                    else
                        ImFormatString(attempt, IM_ARRAYSIZE(attempt), "%d selected", rest);
                    ImFormatString(preview, IM_ARRAYSIZE(preview), "%s", attempt);
                    break;
                }
            }
            if (!selected)
                ImFormatString(preview, IM_ARRAYSIZE(preview), "None");
        }

        if (font)
        {
            dl->AddText(font, label_size,
                text_pos_in_row(font, label_size, btn, preview, btn.Min.x + ui_scale::s(10.f)),
                ImGui::GetColorU32(label_color(*mask != 0, hovered || popup.open)),
                preview);
        }

        {
            const float icon_side = ui_scale::s(14.f);
            const float nudge = anim.open * ui_scale::s(1.5f);
            const ImVec2 icon_min = {
                btn.Max.x - icon_side - ui_scale::s(8.f),
                btn.GetCenter().y - icon_side * 0.5f + nudge };
            icons::draw(dl, "chevron-down",
                ImRect(icon_min, { icon_min.x + icon_side, icon_min.y + icon_side }),
                ImGui::GetColorU32(ImLerp(colors::text_muted, colors::accent, anim.open * 0.6f)));
        }

        return multicombo_dropdown(id, btn, mask, items, count, blocks_drag);
    }

    bool keybind_row(const char* id, const char* label, int* key, const ImRect& row, bool& blocks_drag)
    {
        const float keybind_w = ui_scale::s(layout::keybind_w);
        const float control_h = ui_scale::s(layout::control_h);
        const float right = row.Max.x - ui_scale::s(layout::box_pad_x);
        const ImRect btn(
            { right - keybind_w, row.GetCenter().y - control_h * 0.5f },
            { right, row.GetCenter().y + control_h * 0.5f });

        row_begin(id, label, row, blocks_drag);

        ImGuiID gid = ImGui::GetID(id);
        ImGui::ItemAdd(row, gid);

        const bool row_hovered = ImGui::IsMouseHoveringRect(row.Min, row.Max);

        bool hovered = false, held = false;
        const bool pressed = ImGui::ButtonBehavior(btn, gid, &hovered, &held);
        if (hovered || held || row_hovered)
            blocks_drag = true;

        static std::map<ImGuiID, float> capture_start;
        bool& capturing = g_keybind_capture[gid];
        if (pressed)
        {
            capturing = true;
            capture_start[gid] = (float)ImGui::GetTime();
            close_other_combos("none");
        }

        if (capturing && ImGui::GetTime() - capture_start[gid] > 0.12f)
        {
            for (int vk = 1; vk < 256; ++vk)
            {
                if (vk == VK_LBUTTON || vk == VK_RBUTTON || vk == VK_MBUTTON)
                    continue;
                if (GetAsyncKeyState(vk) & 0x8000)
                {
                    *key = vk;
                    capturing = false;
                    break;
                }
            }
            if (GetAsyncKeyState(VK_ESCAPE) & 0x8000)
            {
                *key = 0;
                capturing = false;
            }
        }

        anim_state& anim = g_anims[gid];
        ease(anim.hover, hovered || capturing || row_hovered ? 1.f : 0.f, 18.f);
        anim.pulse = capturing ? anim.pulse + ImGui::GetIO().DeltaTime * 6.f : 0.f;

        ImDrawList* dl = ImGui::GetWindowDrawList();
        draw_row_hover(dl, row, anim.hover * 0.5f);

        ImFont* font = fonts::inter(layout::row_label_font);
        const float label_size = fonts::size(font);
        if (font)
        {
            dl->AddText(font, label_size,
                label_pos_in_row(font, label_size, row, label, row.Min.x + ui_scale::s(layout::box_pad_x)),
                ImGui::GetColorU32(label_color(*key > 0, row_hovered || hovered || capturing)),
                label);
        }

        const ImVec4 btn_col = capturing
            ? ImVec4(colors::accent.x, colors::accent.y, colors::accent.z,
                0.22f + 0.08f * (0.5f + 0.5f * sinf(anim.pulse)))
            : ImLerp(colors::control, colors::control_hover, anim.hover);
        dl->AddRectFilled(btn.Min, btn.Max, ImGui::GetColorU32(btn_col), ui_scale::s(layout::control_round));

        if (capturing)
        {
            const float ring_a = 0.25f + 0.15f * (0.5f + 0.5f * sinf(anim.pulse * 1.4f));
            dl->AddRect(btn.Min, btn.Max,
                ImGui::GetColorU32(ImVec4(colors::accent.x, colors::accent.y, colors::accent.z, ring_a)),
                ui_scale::s(layout::control_round), 0, 1.2f);
        }

        {
            const ImRect icon_rect(
                { btn.Min.x + ui_scale::s(layout::keybind_icon_pad), btn.Min.y },
                { btn.Min.x + ui_scale::s(layout::keybind_icon_pad) + control_h, btn.Max.y });
            icons::draw(dl, "keyboard", icon_rect,
                ImGui::GetColorU32(hovered || capturing ? colors::text : colors::text_muted));
        }

        const char* key_text = capturing ? "Press key..." : key_name(*key);
        if (font)
        {
            const float text_x = btn.Min.x + ui_scale::s(layout::keybind_icon_pad) + control_h + ui_scale::s(4.f);
            const ImVec4 text_col = capturing
                ? ImLerp(colors::text_muted, colors::accent, 0.5f + 0.5f * sinf(anim.pulse))
                : ImLerp(colors::text_muted, colors::text, anim.hover);
            dl->AddText(font, label_size,
                text_pos_in_row(font, label_size, btn, key_text, text_x),
                ImGui::GetColorU32(text_col),
                key_text);
        }

        return pressed;
    }

    // --- colorpicker -----------------------------------------------------
    // Figma wants rounded corners on the SV square and on both bars, and
    // AddRectFilledMultiColor cannot round anything. So we fill a rounded rect
    // with a flat colour and repaint its vertices along a gradient axis. The
    // original per-vertex alpha is kept as a factor so the anti-aliased fringe
    // survives, which also lets the alpha bar fade to fully transparent.
    void shade_rounded(ImDrawList* dl, int vtx_start, const ImVec2& p0, const ImVec2& p1, ImU32 c0, ImU32 c1)
    {
        const ImVec2 extent   = p1 - p0;
        const float  inv_len2 = 1.f / ImMax(ImLengthSqr(extent), 1e-4f);

        const float r0 = (float)((c0 >> IM_COL32_R_SHIFT) & 0xFF), r1 = (float)((c1 >> IM_COL32_R_SHIFT) & 0xFF);
        const float g0 = (float)((c0 >> IM_COL32_G_SHIFT) & 0xFF), g1 = (float)((c1 >> IM_COL32_G_SHIFT) & 0xFF);
        const float b0 = (float)((c0 >> IM_COL32_B_SHIFT) & 0xFF), b1 = (float)((c1 >> IM_COL32_B_SHIFT) & 0xFF);
        const float a0 = (float)((c0 >> IM_COL32_A_SHIFT) & 0xFF), a1 = (float)((c1 >> IM_COL32_A_SHIFT) & 0xFF);

        for (int i = vtx_start; i < dl->VtxBuffer.Size; i++)
        {
            ImDrawVert& v = dl->VtxBuffer.Data[i];
            const float t = ImClamp(ImDot(v.pos - p0, extent) * inv_len2, 0.f, 1.f);
            const float keep = (float)((v.col >> IM_COL32_A_SHIFT) & 0xFF) / 255.f;
            v.col = IM_COL32(
                (int)ImLerp(r0, r1, t),
                (int)ImLerp(g0, g1, t),
                (int)ImLerp(b0, b1, t),
                (int)(ImLerp(a0, a1, t) * keep));
        }
    }

    struct picker_state
    {
        float  h = 0.f, s = 0.f, v = 0.f;
        ImVec4 cached = { -1.f, -1.f, -1.f, -1.f };
        int    fmt = 0;         // 0 = HEX, 1 = RGB

        // The format selector is a combo that unrolls upwards, inside the picker
        // window: a separate ImGui window would steal focus and tear the popup down.
        bool   fmt_open = false;
        float  fmt_anim = 0.f;

        // The two readout fields are live ImGui::InputText widgets. While one is
        // focused its buffer belongs to the user, otherwise it is re-seeded from
        // the colour every frame so dragging the SV square updates the text.
        bool   edit_val = false;
        bool   edit_pct = false;
        char   buf_val[24] = {};
        char   buf_pct[8]  = {};
    };
    std::map<ImGuiID, picker_state> g_pickers;

    int hex_digit(char c)
    {
        if (c >= '0' && c <= '9') return c - '0';
        if (c >= 'a' && c <= 'f') return c - 'a' + 10;
        if (c >= 'A' && c <= 'F') return c - 'A' + 10;
        return -1;
    }

    // "#RRGGBB", "RRGGBB", "#RGB" and "RGB" all parse; anything else is rejected
    // so a half-typed value never blanks the colour.
    bool parse_hex(const char* text, float out[3])
    {
        while (*text == ' ' || *text == '#')
            ++text;

        int digits[6];
        int n = 0;
        for (; text[n] && n < 6; ++n)
        {
            digits[n] = hex_digit(text[n]);
            if (digits[n] < 0)
                return false;
        }
        int tail = n;
        while (text[tail] == ' ')
            ++tail;
        if (text[tail] != '\0')
            return false;

        if (n == 3)
        {
            for (int i = 0; i < 3; ++i)
                out[i] = (float)(digits[i] * 16 + digits[i]) / 255.f;
            return true;
        }
        if (n == 6)
        {
            for (int i = 0; i < 3; ++i)
                out[i] = (float)(digits[i * 2] * 16 + digits[i * 2 + 1]) / 255.f;
            return true;
        }
        return false;
    }

    // "255,128,0" — commas or spaces separate, components clamp to 0..255.
    bool parse_rgb(const char* text, float out[3])
    {
        const char* p = text;
        for (int comp = 0; comp < 3; ++comp)
        {
            while (*p == ' ' || *p == ',')
                ++p;
            if (*p < '0' || *p > '9')
                return false;

            int v = 0;
            while (*p >= '0' && *p <= '9')
                v = v * 10 + (*p++ - '0');
            out[comp] = (float)ImClamp(v, 0, 255) / 255.f;
        }
        while (*p == ' ' || *p == ',')
            ++p;
        return *p == '\0';
    }

    bool parse_percent(const char* text, float& out)
    {
        const char* p = text;
        while (*p == ' ')
            ++p;
        if (*p < '0' || *p > '9')
            return false;

        int v = 0;
        while (*p >= '0' && *p <= '9')
            v = v * 10 + (*p++ - '0');
        while (*p == ' ' || *p == '%')
            ++p;
        if (*p != '\0')
            return false;

        out = (float)ImClamp(v, 0, 100) * 0.01f;
        return true;
    }

    bool color_row(const char* id, const char* label, ImVec4* value, const ImRect& row, bool& blocks_drag)
    {
        const float swatch_size = ui_scale::s(layout::color_swatch_size);
        const float right = row.Max.x - ui_scale::s(layout::box_pad_x);
        const ImRect swatch(
            { right - swatch_size, row.GetCenter().y - swatch_size * 0.5f },
            { right, row.GetCenter().y + swatch_size * 0.5f });

        row_begin(id, label, row, blocks_drag);

        ImGuiID gid = ImGui::GetID(id);
        ImGui::ItemAdd(row, gid);

        const bool row_hovered = ImGui::IsMouseHoveringRect(row.Min, row.Max);

        bool hovered = false, held = false;
        const bool pressed = ImGui::ButtonBehavior(swatch, gid, &hovered, &held);
        if (hovered || held || row_hovered)
            blocks_drag = true;

        anim_state& anim = g_anims[gid];
        ease(anim.hover, hovered || row_hovered ? 1.f : 0.f, 18.f);

        ImDrawList* dl = ImGui::GetWindowDrawList();
        draw_row_hover(dl, row, anim.hover * 0.45f);

        ImFont* font = fonts::inter(layout::row_label_font);
        const float label_size = fonts::size(font);
        if (font)
        {
            dl->AddText(font, label_size,
                label_pos_in_row(font, label_size, row, label, row.Min.x + ui_scale::s(layout::box_pad_x)),
                ImGui::GetColorU32(label_color(true, row_hovered || hovered)),
                label);
        }

        const ImVec4 frame_col = ImLerp(colors::control, colors::control_hover, anim.hover);
        const float control_round = ui_scale::s(layout::control_round);
        dl->AddRectFilled(swatch.Min, swatch.Max, ImGui::GetColorU32(frame_col), control_round);
        dl->AddRectFilled(
            { swatch.Min.x + ui_scale::s(3.f), swatch.Min.y + ui_scale::s(3.f) },
            { swatch.Max.x - ui_scale::s(3.f), swatch.Max.y - ui_scale::s(3.f) },
            ImGui::GetColorU32(*value), ImMax(0.f, control_round - 1.f));

        char popup_id[64];
        ImFormatString(popup_id, IM_ARRAYSIZE(popup_id), "##color_%s", id);
        if (pressed)
            ImGui::OpenPopup(popup_id);

        const float pw = ui_scale::s(layout::picker_w);
        const float ph = ui_scale::s(layout::picker_h);

        ImGui::SetNextWindowPos({ swatch.Max.x - pw, swatch.Max.y + ui_scale::s(6.f) }, ImGuiCond_Appearing);
        ImGui::SetNextWindowSize({ pw, ph });
        ImGui::PushStyleVar(ImGuiStyleVar_WindowRounding, ui_scale::s(layout::picker_round));
        ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, { 0.f, 0.f });
        ImGui::PushStyleVar(ImGuiStyleVar_WindowBorderSize, 1.f);
        ImGui::PushStyleColor(ImGuiCol_PopupBg, colors::dropdown_bg);
        ImGui::PushStyleColor(ImGuiCol_Border, ImVec4(0.f, 0.f, 0.f, 0.3f));

        bool changed = false;

        if (ImGui::BeginPopup(popup_id, ImGuiWindowFlags_NoScrollbar | ImGuiWindowFlags_NoScrollWithMouse))
        {
            blocks_drag = true;

            picker_state& ps = g_pickers[gid];
            // Re-derive HSV whenever the colour changed behind our back, but keep
            // hue/saturation when the RGB triplet cannot carry them (grey, black).
            if (ps.cached.x != value->x || ps.cached.y != value->y || ps.cached.z != value->z)
            {
                float h, s, v;
                ImGui::ColorConvertRGBtoHSV(value->x, value->y, value->z, h, s, v);
                if (s > 0.f) ps.h = h;
                if (v > 0.f) ps.s = s;
                ps.v = v;
                ps.cached = *value;
            }

            const auto write_back = [&]()
            {
                float r, g, b;
                ImGui::ColorConvertHSVtoRGB(ps.h, ps.s, ps.v, r, g, b);
                value->x = r; value->y = g; value->z = b;
                ps.cached = *value;
                changed = true;
            };

            ImDrawList* pdl = ImGui::GetWindowDrawList();
            const ImVec2 org = ImGui::GetWindowPos();
            const ImGuiIO& io = ImGui::GetIO();

            const float pad  = ui_scale::s((layout::picker_w - layout::picker_sv_size) * 0.5f);
            const float side = ui_scale::s(layout::picker_sv_size);

            const ImRect sv_bb(org + ImVec2(pad, pad), org + ImVec2(pad + side, pad + side));
            // Vertical stack in design px: 7.5 pad, 190 square, bars, readout row,
            // 4 px left at the bottom of the 247 px card.
            const ImRect hue_bb(
                org + ImVec2(pad, ui_scale::s(204.f)),
                org + ImVec2(pad + side, ui_scale::s(204.f + layout::picker_hue_h)));
            const ImRect alpha_bb(
                org + ImVec2(pad, ui_scale::s(214.f)),
                org + ImVec2(pad + side, ui_scale::s(214.f + layout::picker_alpha_h)));

            // --- readout row geometry ----------------------------------------
            // Declared before the bars because the format menu unrolls upwards
            // over them and has to swallow their input while it is open.
            const float row_y = ui_scale::s(223.f);
            const float row_h = ui_scale::s(20.f);
            const float gap   = ui_scale::s(3.5f);
            const float field_round = ui_scale::s(6.f);
            const ImU32 field_bg = ImGui::GetColorU32(ImVec4(0.f, 0.f, 0.f, 0.25f));

            // Widths add up to the 190 px the SV square spans. The value field
            // gets the lion's share: "255,200,255" is the widest string any of
            // the three can hold, and a clipped value is not an editable one.
            const ImRect fmt_bb(org + ImVec2(pad, row_y),
                                org + ImVec2(pad + ui_scale::s(47.f), row_y + row_h));
            const ImRect val_bb(ImVec2(fmt_bb.Max.x + gap, fmt_bb.Min.y),
                                ImVec2(fmt_bb.Max.x + gap + ui_scale::s(86.f), fmt_bb.Max.y));
            const ImRect pct_bb(ImVec2(val_bb.Max.x + gap, fmt_bb.Min.y),
                                ImVec2(val_bb.Max.x + gap + ui_scale::s(50.f), fmt_bb.Max.y));

            static const char* const fmt_items[] = { "HEX", "RGB" };
            const float fmt_item_h = ui_scale::s(20.f);
            const float fmt_menu_h = (float)IM_ARRAYSIZE(fmt_items) * fmt_item_h + ui_scale::s(8.f);
            const ImRect fmt_menu(
                ImVec2(fmt_bb.Min.x, fmt_bb.Min.y - ui_scale::s(4.f) - fmt_menu_h),
                ImVec2(fmt_bb.Max.x, fmt_bb.Min.y - ui_scale::s(4.f)));

            smooth(ps.fmt_anim, ps.fmt_open ? 1.f : 0.f, 20.f);
            const bool menu_capture = ps.fmt_open && fmt_menu.Contains(io.MousePos);

            // --- SV square ---------------------------------------------------
            // BeginDisabled only wraps the hit-tests: it scales style alpha, and
            // every fill below goes through GetColorU32, which would dim too.
            ImGui::BeginDisabled(menu_capture);
            ImGui::SetCursorScreenPos(sv_bb.Min);
            ImGui::InvisibleButton("##sv", sv_bb.GetSize());
            const bool sv_active = ImGui::IsItemActive();
            ImGui::EndDisabled();
            if (sv_active)
            {
                ps.s = ImClamp((io.MousePos.x - sv_bb.Min.x) / ImMax(sv_bb.GetWidth(), 1.f), 0.f, 1.f);
                ps.v = 1.f - ImClamp((io.MousePos.y - sv_bb.Min.y) / ImMax(sv_bb.GetHeight(), 1.f), 0.f, 1.f);
                write_back();
            }

            const float sv_round = ui_scale::s(8.f);
            float hr, hg, hb;
            ImGui::ColorConvertHSVtoRGB(ps.h, 1.f, 1.f, hr, hg, hb);
            const ImU32 hue_full = IM_COL32((int)(hr * 255.f), (int)(hg * 255.f), (int)(hb * 255.f), 255);

            int vtx = pdl->VtxBuffer.Size;
            pdl->AddRectFilled(sv_bb.Min, sv_bb.Max, IM_COL32_WHITE, sv_round);
            shade_rounded(pdl, vtx, sv_bb.Min, { sv_bb.Max.x, sv_bb.Min.y }, IM_COL32_WHITE, hue_full);

            vtx = pdl->VtxBuffer.Size;
            pdl->AddRectFilled(sv_bb.Min, sv_bb.Max, IM_COL32_BLACK, sv_round);
            shade_rounded(pdl, vtx, sv_bb.Min, { sv_bb.Min.x, sv_bb.Max.y },
                IM_COL32(0, 0, 0, 0), IM_COL32(0, 0, 0, 255));

            const ImVec2 sv_marker(
                sv_bb.Min.x + sv_bb.GetWidth() * ps.s,
                sv_bb.Min.y + sv_bb.GetHeight() * (1.f - ps.v));
            const float marker_r = ui_scale::s(6.f);
            pdl->AddCircleFilled(sv_marker, marker_r - ui_scale::s(1.5f),
                ImGui::GetColorU32(ImVec4(value->x, value->y, value->z, 1.f)), 24);
            pdl->AddCircle(sv_marker, marker_r, IM_COL32(255, 255, 255, 235), 24, ui_scale::s(1.5f));
            pdl->AddCircle(sv_marker, marker_r + ui_scale::s(1.f), IM_COL32(0, 0, 0, 70), 24, ui_scale::s(1.f));

            // --- hue bar -----------------------------------------------------
            ImGui::BeginDisabled(menu_capture);
            ImGui::SetCursorScreenPos({ hue_bb.Min.x, hue_bb.GetCenter().y - ui_scale::s(7.f) });
            ImGui::InvisibleButton("##hue", { hue_bb.GetWidth(), ui_scale::s(14.f) });
            const bool hue_active = ImGui::IsItemActive();
            ImGui::EndDisabled();
            if (hue_active)
            {
                ps.h = ImClamp((io.MousePos.x - hue_bb.Min.x) / ImMax(hue_bb.GetWidth(), 1.f), 0.f, 1.f);
                write_back();
            }

            static const ImU32 rainbow[7] = {
                IM_COL32(255,   0,   0, 255), IM_COL32(255, 255,   0, 255),
                IM_COL32(  0, 255,   0, 255), IM_COL32(  0, 255, 255, 255),
                IM_COL32(  0,   0, 255, 255), IM_COL32(255,   0, 255, 255),
                IM_COL32(255,   0,   0, 255) };

            // Six segments, each its own rect so the vertex shading stays inside
            // its band — one full-width rect would only carry the end colours.
            const float hue_round = hue_bb.GetHeight() * 0.5f;
            for (int i = 0; i < 6; i++)
            {
                const float x0 = hue_bb.Min.x + hue_bb.GetWidth() * ((float)i / 6.f);
                const float x1 = hue_bb.Min.x + hue_bb.GetWidth() * ((float)(i + 1) / 6.f);
                const ImDrawFlags flags = i == 0 ? ImDrawFlags_RoundCornersLeft
                                       : i == 5 ? ImDrawFlags_RoundCornersRight
                                                : ImDrawFlags_RoundCornersNone;
                // Half-pixel overlap hides the seams between segments.
                vtx = pdl->VtxBuffer.Size;
                pdl->AddRectFilled({ x0, hue_bb.Min.y }, { x1 + (i < 5 ? 0.5f : 0.f), hue_bb.Max.y },
                    IM_COL32_WHITE, hue_round, flags);
                shade_rounded(pdl, vtx, { x0, hue_bb.Min.y }, { x1, hue_bb.Min.y }, rainbow[i], rainbow[i + 1]);
            }

            const float hue_x = hue_bb.Min.x + hue_bb.GetWidth() * ps.h;
            const float hue_kw = ui_scale::s(3.f);
            const ImVec2 hk_min(hue_x - hue_kw * 0.5f, hue_bb.Min.y - ui_scale::s(2.f));
            const ImVec2 hk_max(hue_x + hue_kw * 0.5f, hue_bb.Max.y + ui_scale::s(2.f));
            pdl->AddRectFilled(hk_min - ImVec2(1.f, 1.f), hk_max + ImVec2(1.f, 1.f),
                IM_COL32(0, 0, 0, 90), hue_kw);
            pdl->AddRectFilled(hk_min, hk_max, IM_COL32_WHITE, hue_kw * 0.5f);

            // --- alpha bar ---------------------------------------------------
            ImGui::BeginDisabled(menu_capture);
            ImGui::SetCursorScreenPos({ alpha_bb.Min.x, alpha_bb.GetCenter().y - ui_scale::s(7.f) });
            ImGui::InvisibleButton("##alpha", { alpha_bb.GetWidth(), ui_scale::s(14.f) });
            const bool alpha_active = ImGui::IsItemActive();
            ImGui::EndDisabled();
            if (alpha_active)
            {
                value->w = ImClamp((io.MousePos.x - alpha_bb.Min.x) / ImMax(alpha_bb.GetWidth(), 1.f), 0.f, 1.f);
                ps.cached = *value;
                changed = true;
            }

            const float alpha_round = alpha_bb.GetHeight() * 0.5f;
            pdl->AddRectFilled(alpha_bb.Min, alpha_bb.Max,
                ImGui::GetColorU32(ImVec4(0.f, 0.f, 0.f, 0.25f)), alpha_round);
            vtx = pdl->VtxBuffer.Size;
            pdl->AddRectFilled(alpha_bb.Min, alpha_bb.Max, IM_COL32_WHITE, alpha_round);
            shade_rounded(pdl, vtx, alpha_bb.Min, { alpha_bb.Max.x, alpha_bb.Min.y },
                IM_COL32((int)(value->x * 255.f), (int)(value->y * 255.f), (int)(value->z * 255.f), 0),
                IM_COL32((int)(value->x * 255.f), (int)(value->y * 255.f), (int)(value->z * 255.f), 255));

            const ImVec2 a_marker(alpha_bb.Min.x + alpha_bb.GetWidth() * value->w, alpha_bb.GetCenter().y);
            const float a_marker_r = alpha_bb.GetHeight() * 0.5f + ui_scale::s(2.5f);
            pdl->AddCircleFilled(a_marker, a_marker_r + ui_scale::s(1.f), IM_COL32(0, 0, 0, 90), 20);
            pdl->AddCircleFilled(a_marker, a_marker_r, IM_COL32_WHITE, 20);

            // --- readout row -------------------------------------------------
            pdl->AddRectFilled(fmt_bb.Min, fmt_bb.Max, field_bg, field_round);
            pdl->AddRectFilled(val_bb.Min, val_bb.Max, field_bg, field_round);
            pdl->AddRectFilled(pct_bb.Min, pct_bb.Max, field_bg, field_round);

            // Format selector: a real combo, drawn inside this window so it never
            // steals focus from the picker popup.
            ImGui::BeginDisabled(menu_capture);
            ImGui::SetCursorScreenPos(fmt_bb.Min);
            ImGui::InvisibleButton("##fmt", fmt_bb.GetSize());
            const bool fmt_pressed = ImGui::IsItemActivated();
            ImGui::EndDisabled();
            if (fmt_pressed)
                ps.fmt_open = !ps.fmt_open;

            ImFont* fmt_font = fonts::inter(14.f);
            const float fmt_size = fonts::size(fmt_font);
            if (fmt_font)
            {
                const char* fmt_text = fmt_items[ImClamp(ps.fmt, 0, 1)];
                pdl->AddText(fmt_font, fmt_size,
                    text_pos_in_row(fmt_font, fmt_size, fmt_bb, fmt_text, fmt_bb.Min.x + ui_scale::s(7.f)),
                    ImGui::GetColorU32(colors::text_muted), fmt_text);
            }
            icons::draw(pdl, ps.fmt_open ? "chevron-up" : "chevron-down",
                { fmt_bb.Max.x - ui_scale::s(8.5f), fmt_bb.GetCenter().y }, ui_scale::s(10.f),
                ImGui::GetColorU32(colors::text_muted));

            // No colour chip in the value field: the swatch in the row, the SV
            // marker and the alpha bar already carry it, and the ~16 px it cost
            // is exactly what the longest RGB triplet was missing.

            // --- editable colour + alpha fields ------------------------------
            const int ri = (int)(value->x * 255.f + 0.5f);
            const int gi = (int)(value->y * 255.f + 0.5f);
            const int bi = (int)(value->z * 255.f + 0.5f);

            if (!ps.edit_val)
            {
                if (ps.fmt == 0)
                    ImFormatString(ps.buf_val, IM_ARRAYSIZE(ps.buf_val), "#%02X%02X%02X", ri, gi, bi);
                else
                    ImFormatString(ps.buf_val, IM_ARRAYSIZE(ps.buf_val), "%d,%d,%d", ri, gi, bi);
            }
            if (!ps.edit_pct)
                ImFormatString(ps.buf_pct, IM_ARRAYSIZE(ps.buf_pct), "%d%%", (int)(value->w * 100.f + 0.5f));

            ImGui::PushStyleVar(ImGuiStyleVar_FramePadding, { 0.f, 0.f });
            ImGui::PushStyleVar(ImGuiStyleVar_FrameBorderSize, 0.f);
            ImGui::PushStyleColor(ImGuiCol_FrameBg, ImVec4(0.f, 0.f, 0.f, 0.f));
            ImGui::PushStyleColor(ImGuiCol_FrameBgHovered, ImVec4(0.f, 0.f, 0.f, 0.f));
            ImGui::PushStyleColor(ImGuiCol_FrameBgActive, ImVec4(0.f, 0.f, 0.f, 0.f));
            ImGui::PushStyleColor(ImGuiCol_Text, colors::text);
            if (ImFont* val_font = fonts::inter(13.f))
                ImGui::PushFont(val_font);

            ImFont* val_font = ImGui::GetFont();
            const float val_size = ImGui::GetFontSize();
            const float text_h = val_font->CalcTextSizeA(val_size, FLT_MAX, 0.f, "0").y;

            ImGui::BeginDisabled(menu_capture);
            {
                const float x0 = val_bb.Min.x + ui_scale::s(8.f);
                ImGui::SetCursorScreenPos({ x0, val_bb.GetCenter().y - text_h * 0.5f });
                ImGui::PushItemWidth(val_bb.Max.x - ui_scale::s(6.f) - x0);
                // CharsHexadecimal would eat the commas the RGB form needs, so the
                // filtering is left to the parser.
                const ImGuiInputTextFlags flags = ImGuiInputTextFlags_AutoSelectAll
                    | (ps.fmt == 0 ? ImGuiInputTextFlags_CharsUppercase : 0);
                ImGui::InputText("##val", ps.buf_val, IM_ARRAYSIZE(ps.buf_val), flags);
                ps.edit_val = ImGui::IsItemActive();
                if (ImGui::IsItemHovered() || ps.edit_val)
                    blocks_drag = true;

                float rgb[3];
                const bool ok = ps.fmt == 0 ? parse_hex(ps.buf_val, rgb) : parse_rgb(ps.buf_val, rgb);
                if (ps.edit_val && ok &&
                    (rgb[0] != value->x || rgb[1] != value->y || rgb[2] != value->z))
                {
                    value->x = rgb[0]; value->y = rgb[1]; value->z = rgb[2];
                    // Feed HSV from the typed colour, keeping hue/sat where the
                    // triplet cannot express them, exactly like the cache path.
                    float h, s, v;
                    ImGui::ColorConvertRGBtoHSV(rgb[0], rgb[1], rgb[2], h, s, v);
                    if (s > 0.f) ps.h = h;
                    if (v > 0.f) ps.s = s;
                    ps.v = v;
                    ps.cached = *value;
                    changed = true;
                }
                ImGui::PopItemWidth();
            }

            {
                ImGui::SetCursorScreenPos({ pct_bb.Min.x + ui_scale::s(6.f),
                                            pct_bb.GetCenter().y - text_h * 0.5f });
                ImGui::PushItemWidth(pct_bb.GetWidth() - ui_scale::s(12.f));
                ImGui::PushStyleColor(ImGuiCol_Text, colors::text_muted);
                ImGui::InputText("##pct", ps.buf_pct, IM_ARRAYSIZE(ps.buf_pct),
                    ImGuiInputTextFlags_AutoSelectAll);
                ImGui::PopStyleColor();
                ps.edit_pct = ImGui::IsItemActive();
                if (ImGui::IsItemHovered() || ps.edit_pct)
                    blocks_drag = true;

                float a = value->w;
                if (ps.edit_pct && parse_percent(ps.buf_pct, a) && a != value->w)
                {
                    value->w = a;
                    ps.cached = *value;
                    changed = true;
                }
                ImGui::PopItemWidth();
            }
            ImGui::EndDisabled();

            if (fonts::inter(13.f))
                ImGui::PopFont();
            ImGui::PopStyleColor(4);
            ImGui::PopStyleVar(2);

            // --- format menu, drawn last so it sits on top -------------------
            if (ps.fmt_anim > 0.01f)
            {
                const float a = ps.fmt_anim * ps.fmt_anim;
                const float slide = (1.f - ps.fmt_anim) * ui_scale::s(6.f);
                const ImRect menu(fmt_menu.Min + ImVec2(0.f, slide), fmt_menu.Max + ImVec2(0.f, slide));

                pdl->AddRectFilled(menu.Min, menu.Max,
                    ImGui::GetColorU32(ImVec4(colors::dropdown_bg.x, colors::dropdown_bg.y,
                        colors::dropdown_bg.z, ImMin(0.96f, colors::dropdown_bg.w + 0.22f) * a)),
                    ui_scale::s(layout::combo_round));
                pdl->AddRect(menu.Min, menu.Max, ImGui::GetColorU32(colors::wash(0.08f * a)),
                    ui_scale::s(layout::combo_round), 0, 1.f);

                for (int i = 0; i < IM_ARRAYSIZE(fmt_items); ++i)
                {
                    const ImRect item(
                        { menu.Min.x + ui_scale::s(4.f), menu.Min.y + ui_scale::s(4.f) + (float)i * fmt_item_h },
                        { menu.Max.x - ui_scale::s(4.f), menu.Min.y + ui_scale::s(4.f) + (float)(i + 1) * fmt_item_h });

                    const bool hovered = ps.fmt_open && item.Contains(io.MousePos);
                    const bool selected = ps.fmt == i;
                    if (hovered || selected)
                    {
                        const ImVec4 bg = selected
                            ? ImVec4(colors::accent.x, colors::accent.y, colors::accent.z, 0.16f * a)
                            : colors::wash(0.05f * a);
                        pdl->AddRectFilled(item.Min, item.Max, ImGui::GetColorU32(bg),
                            ui_scale::s(layout::combo_round));
                    }

                    if (fmt_font)
                    {
                        const ImVec4 col = label_color(selected, hovered);
                        pdl->AddText(fmt_font, fmt_size,
                            text_pos_in_row(fmt_font, fmt_size, item, fmt_items[i], item.Min.x + ui_scale::s(6.f)),
                            ImGui::GetColorU32(ImVec4(col.x, col.y, col.z, col.w * a)), fmt_items[i]);
                    }

                    if (hovered && ImGui::IsMouseClicked(ImGuiMouseButton_Left))
                    {
                        ps.fmt = i;
                        ps.fmt_open = false;
                        ps.edit_val = false;   // reformat the buffer for the new base
                    }
                }

                // Click anywhere else inside the picker folds the menu back up.
                if (ps.fmt_open && ImGui::IsMouseClicked(ImGuiMouseButton_Left)
                    && !menu.Contains(io.MousePos) && !fmt_bb.Contains(io.MousePos))
                {
                    ps.fmt_open = false;
                }
            }

            ImGui::EndPopup();
        }
        else
        {
            // Folded away, so the menu must not linger open for the next opening.
            g_pickers[gid].fmt_open = false;
        }

        ImGui::PopStyleColor(2);
        ImGui::PopStyleVar(3);
        return pressed || changed;
    }

    bool button_row(const char* id, const char* label, const ImRect& row, bool& blocks_drag, ImVec4 color)
    {
        const float btn_w = ui_scale::s(96.f);
        const float btn_h = ui_scale::s(layout::button_h);
        const float right = row.Max.x - ui_scale::s(layout::box_pad_x);
        const ImRect btn(
            { right - btn_w, row.GetCenter().y - btn_h * 0.5f },
            { right, row.GetCenter().y + btn_h * 0.5f });

        ImGuiID gid = ImGui::GetID(id);
        ImGui::ItemAdd(btn, gid);

        bool hovered = false, held = false;
        const bool pressed = ImGui::ButtonBehavior(btn, gid, &hovered, &held);
        if (hovered || held)
            blocks_drag = true;

        anim_state& anim = g_anims[gid];
        ease(anim.hover, hovered || held ? 1.f : 0.f, 18.f);

        ImDrawList* dl = ImGui::GetWindowDrawList();
        ImFont* font = fonts::inter(layout::row_label_font);

        const bool custom = color.w > 0.f;
        const ImVec4 base = custom ? color : colors::control;
        const ImVec4 hover_col = custom
            ? ImVec4(color.x * 1.08f, color.y * 1.08f, color.z * 1.08f, color.w)
            : colors::control_hover;
        const ImVec4 btn_col = ImLerp(base, hover_col, anim.hover);
        dl->AddRectFilled(btn.Min, btn.Max, ImGui::GetColorU32(btn_col), ui_scale::s(layout::control_round));

        if (font)
        {
            const float label_size = fonts::size(font);
            const ImVec2 ts = font->CalcTextSizeA(label_size, FLT_MAX, 0.f, label);
            dl->AddText(font, label_size,
                { btn.Min.x + (btn.GetWidth() - ts.x) * 0.5f, btn.GetCenter().y - ts.y * 0.5f },
                ImGui::GetColorU32(custom ? colors::text : label_color(true, hovered)),
                label);
        }

        return pressed;
    }

    bool text_input_row(const char* id, const char* label, char* buf, int buf_size, const ImRect& row, bool& blocks_drag)
    {
        const float field_w = ui_scale::s(168.f);
        const float control_h = ui_scale::s(layout::control_h);
        const float right   = row.Max.x - ui_scale::s(layout::box_pad_x);
        const ImRect field(
            { right - field_w, row.GetCenter().y - control_h * 0.5f },
            { right, row.GetCenter().y + control_h * 0.5f });

        ImGuiID gid = ImGui::GetID(id);

        const bool row_hovered = ImGui::IsMouseHoveringRect(row.Min, row.Max);
        static std::map<ImGuiID, float> input_hover;
        float& input_h = input_hover[gid];
        const bool field_hovered = ImGui::IsMouseHoveringRect(field.Min, field.Max);
        smooth(input_h, row_hovered || field_hovered ? 1.f : 0.f, 18.f);

        if (row_hovered || field_hovered)
            blocks_drag = true;

        ImDrawList* dl = ImGui::GetWindowDrawList();
        draw_row_hover(dl, row, input_h * 0.45f);

        ImFont* font = fonts::inter(layout::row_label_font);
        const float label_size = fonts::size(font);
        if (font)
        {
            dl->AddText(font, label_size,
                label_pos_in_row(font, label_size, row, label, row.Min.x + ui_scale::s(layout::box_pad_x)),
                ImGui::GetColorU32(label_color(false, row_hovered)),
                label);
        }

        const ImVec4 field_col = ImLerp(colors::control, colors::control_hover, input_h);
        dl->AddRectFilled(field.Min, field.Max, ImGui::GetColorU32(field_col), ui_scale::s(layout::control_round));

        ImGui::SetCursorScreenPos({ field.Min.x + ui_scale::s(8.f), field.Min.y + ui_scale::s(4.f) });
        ImGui::PushItemWidth(field.GetWidth() - ui_scale::s(16.f));
        ImGui::PushStyleVar(ImGuiStyleVar_FramePadding, { 0.f, 0.f });
        ImGui::PushStyleVar(ImGuiStyleVar_FrameBorderSize, 0.f);
        ImGui::PushStyleColor(ImGuiCol_FrameBg, ImVec4(0.f, 0.f, 0.f, 0.f));
        ImGui::PushStyleColor(ImGuiCol_FrameBgHovered, ImVec4(0.f, 0.f, 0.f, 0.f));
        ImGui::PushStyleColor(ImGuiCol_FrameBgActive, ImVec4(0.f, 0.f, 0.f, 0.f));
        ImGui::PushStyleColor(ImGuiCol_Border, ImVec4(0.f, 0.f, 0.f, 0.f));
        ImGui::PushStyleColor(ImGuiCol_Text, colors::text);

        char input_id[64];
        ImFormatString(input_id, IM_ARRAYSIZE(input_id), "##input_%s", id);
        if (font)
            ImGui::PushFont(font);
        const bool changed = ImGui::InputText(input_id, buf, (size_t)buf_size);
        if (ImGui::IsItemActive() || ImGui::IsItemHovered())
            blocks_drag = true;
        if (font)
            ImGui::PopFont();

        ImGui::PopStyleColor(5);
        ImGui::PopStyleVar(2);
        ImGui::PopItemWidth();
        return changed;
    }

    // --- right-click bind popup ------------------------------------------
    // Card next to the row that was right-clicked. Key field, Hold/Toggle/
    // Always segment, "show in keybind list" switch and per-kind target
    // values (bool state, explicit slider values, combo item, mask items).
    // Right-click menu: Add Bind / the binds already on the row / Clear All.
    void draw_bind_menu(bool& blocks_drag)
    {
        bind_menu_state& m = g_bind_menu;
        smooth(m.alpha, m.open ? 1.f : 0.f, 24.f);
        if (m.alpha < 0.01f && !m.open)
            return;

        ImFont* font = fonts::inter(14.f);
        if (!font)
            return;
        const float fs = fonts::size(font);

        // Do not create the entry just by opening the menu — an empty vector
        // would linger for every row that was ever right-clicked.
        static const std::vector<bind_entry> no_binds;
        auto bind_it = g_binds.find(m.id);
        const std::vector<bind_entry>& binds = bind_it == g_binds.end() ? no_binds : bind_it->second;

        const float w      = ui_scale::s(164.f);
        const float item_h = ui_scale::s(26.f);
        const float pad    = ui_scale::s(6.f);
        const float sep_h  = ui_scale::s(7.f);
        const float sep_total = sep_h * 2.f;   // a separator eats half above, one and a half below
        const int   rows   = 1 + (int)binds.size() + (binds.empty() ? 0 : 1);
        const float h      = pad * 2.f + (float)rows * item_h
                           + (binds.empty() ? 0.f : sep_total * 2.f);

        const ImVec2 display = ImGui::GetIO().DisplaySize;
        ImVec2 pos = m.anchor + ImVec2(ui_scale::s(2.f), ui_scale::s(2.f));
        pos.x = ImClamp(pos.x, 0.f, ImMax(0.f, display.x - w));
        pos.y = ImClamp(pos.y, 0.f, ImMax(0.f, display.y - h));
        const ImRect card(pos, pos + ImVec2(w, h));

        const ImVec2 mouse = ImGui::GetIO().MousePos;
        if (m.open && card.Contains(mouse))
            blocks_drag = true;

        ImDrawList* fg = ImGui::GetForegroundDrawList();
        const float a = m.alpha;
        const float round = ui_scale::s(10.f);

        fg->AddRectFilled(card.Min, card.Max,
            ImGui::GetColorU32(ImVec4(colors::dropdown_bg.x, colors::dropdown_bg.y,
                colors::dropdown_bg.z, 0.94f * a)), round);
        fg->AddRect(card.Min, card.Max,
            ImGui::GetColorU32(colors::wash(0.06f * a)), round, 0, ui_scale::s(1.f));

        const bool clicked = ImGui::IsMouseClicked(ImGuiMouseButton_Left)
            && ImGui::GetTime() - m.opened_at > 0.05f;

        float y = card.Min.y + pad;
        const auto item = [&](const char* text, const ImVec4& col) -> bool
        {
            const ImRect line({ card.Min.x + pad, y }, { card.Max.x - pad, y + item_h });
            const bool hovered = m.open && line.Contains(mouse);
            if (hovered)
            {
                fg->AddRectFilled(line.Min, line.Max,
                    ImGui::GetColorU32(colors::wash(0.06f * a)), ui_scale::s(4.f));
            }
            const ImVec2 ts = font->CalcTextSizeA(fs, FLT_MAX, 0.f, text);
            fg->AddText(font, fs,
                { line.Min.x + ui_scale::s(8.f), line.GetCenter().y - ts.y * 0.5f },
                ImGui::GetColorU32(ImVec4(col.x, col.y, col.z, col.w * a * (hovered ? 1.f : 0.75f))),
                text);
            y += item_h;
            return hovered && clicked;
        };

        const auto separator = [&]()
        {
            y += sep_h * 0.5f;
            fg->AddRectFilled({ card.Min.x + pad, y }, { card.Max.x - pad, y + ui_scale::s(1.f) },
                ImGui::GetColorU32(colors::wash(0.08f * a)));
            y += sep_h * 1.5f;
        };

        int  edit_index = -1;
        bool do_add = false, do_clear = false;

        if (item("Add Bind", colors::text))
            do_add = true;

        if (!binds.empty())
        {
            separator();
            for (int i = 0; i < (int)binds.size(); ++i)
            {
                char text[64];
                static const char* mode_names[3] = { "Hold", "Toggle", "Always" };
                ImFormatString(text, IM_ARRAYSIZE(text), "%s  -  %s",
                    binds[i].mode == mode_always ? "Always" : key_name(binds[i].key),
                    mode_names[ImClamp(binds[i].mode, 0, 2)]);
                if (item(text, colors::text_muted))
                    edit_index = i;
            }
            separator();
            if (item("Clear All", colors::danger))
                do_clear = true;
        }

        if (do_add)
        {
            std::vector<bind_entry>& list = g_binds[m.id];
            bind_entry fresh;
            fresh.label = m.label;
            if (!list.empty())
                fresh.kind = list.front().kind;
            list.push_back(fresh);
            m.open = false;
            open_bind_popup(m.id, (int)list.size() - 1, m.anchor, true);
        }
        else if (edit_index >= 0)
        {
            m.open = false;
            open_bind_popup(m.id, edit_index, m.anchor, false);
        }
        else if (do_clear)
        {
            g_binds.erase(m.id);
            m.open = false;
            g_bind_popup.open = false;
        }
        else if (m.open && clicked && !card.Contains(mouse))
        {
            m.open = false;
        }

        if (m.open && ImGui::IsKeyPressed(ImGuiKey_Escape))
            m.open = false;
    }

    void draw_bind_popup(bool& blocks_drag)
    {
        bind_popup_state& p = g_bind_popup;
        smooth(p.alpha, p.open ? 1.f : 0.f, 22.f);
        if (p.alpha < 0.01f && !p.open)
        {
            draw_bind_menu(blocks_drag);
            return;
        }

        auto it = g_binds.find(p.id);
        if (it == g_binds.end() || p.index < 0 || p.index >= (int)it->second.size())
        {
            p.open = false;
            draw_bind_menu(blocks_drag);
            return;
        }
        bind_entry& bind = it->second[p.index];

        const float w     = ui_scale::s(layout::bind_popup_w);
        const float round = ui_scale::s(layout::bind_popup_round);
        const float pad   = ui_scale::s(15.f);

        ImFont* title_font = fonts::inter(15.f, true);
        ImFont* font       = fonts::inter(14.f);
        if (!font)
            return;
        const float fs  = fonts::size(font);
        const float tfs = title_font ? fonts::size(title_font) : fs;

        const float field_h = ui_scale::s(layout::bind_field_h);
        const float seg_h   = ui_scale::s(layout::bind_seg_h);
        const float line_h  = ui_scale::s(26.f);
        const float gap     = ui_scale::s(10.f);

        // Height depends on the payload rows the bind's kind needs.
        int value_rows = 0;
        switch (bind.kind)
        {
        case bind_bool:  value_rows = 1; break;                       // Set value to ON
        case bind_float: value_rows = 1; break;                       // value when active
        case bind_combo: value_rows = 1; break;                       // item when active
        case bind_mask:  value_rows = (int)bind.items.size(); break;  // item checkboxes
        }

        const float btn_h = ui_scale::s(28.f);
        const float h = pad + tfs + ui_scale::s(12.f)
            + field_h + gap
            + seg_h + gap
            + line_h + ui_scale::s(4.f)                                    // show in list
            + (float)value_rows * (line_h + ui_scale::s(4.f))
            + gap + btn_h
            + pad;

        // Keep the card inside the display.
        const ImVec2 display = ImGui::GetIO().DisplaySize;
        ImVec2 pos = p.anchor + ImVec2(ui_scale::s(6.f), ui_scale::s(6.f));
        pos.x = ImClamp(pos.x, 0.f, display.x - w);
        pos.y = ImClamp(pos.y, 0.f, display.y - h);
        const ImRect card(pos, pos + ImVec2(w, h));

        if (p.open && card.Contains(ImGui::GetIO().MousePos))
            blocks_drag = true;

        ImDrawList* fg = ImGui::GetForegroundDrawList();
        const float a = p.alpha;

        // Denser than a plain dropdown: the card floats over the settings rows
        // and has to stay readable on top of them.
        fg->AddRectFilled(card.Min, card.Max,
            ImGui::GetColorU32(ImVec4(colors::dropdown_bg.x, colors::dropdown_bg.y,
                colors::dropdown_bg.z, 0.94f * a)),
            round);
        fg->AddRect(card.Min, card.Max,
            ImGui::GetColorU32(colors::wash(0.06f * a)), round, 0, ui_scale::s(1.f));

        float y = card.Min.y + pad;

        // Title: the function this bind belongs to.
        if (title_font)
        {
            fg->AddText(title_font, tfs, { card.Min.x + pad, y },
                ImGui::GetColorU32(colors::fg(0.9f * a)), bind.label.c_str());
        }
        y += tfs + ui_scale::s(12.f);

        const auto text_col = [&](float alpha) {
            return ImGui::GetColorU32(colors::fg(alpha * a));
        };
        // While the card fades out it must not react to anything any more.
        const bool clicked = p.open && ImGui::IsMouseClicked(ImGuiMouseButton_Left);

        // --- key field ------------------------------------------------------
        {
            const ImRect field(
                { card.Min.x + pad, y },
                { card.Max.x - pad, y + field_h });

            const bool hovered = ImGui::IsMouseHoveringRect(field.Min, field.Max);
            if (hovered && clicked)
                p.capturing = true;

            const ImVec4 base = p.capturing
                ? ImVec4(colors::accent.x, colors::accent.y, colors::accent.z, 0.20f)
                : (hovered ? colors::control_hover : colors::control);
            fg->AddRectFilled(field.Min, field.Max,
                ImGui::GetColorU32(ImVec4(base.x, base.y, base.z, base.w * a)),
                ui_scale::s(layout::control_round));
            if (p.capturing)
            {
                fg->AddRect(field.Min, field.Max,
                    ImGui::GetColorU32(ImVec4(colors::accent.x, colors::accent.y, colors::accent.z, 0.5f * a)),
                    ui_scale::s(layout::control_round), 0, ui_scale::s(1.f));
            }

            const char* key_text = p.capturing ? "Press key..." : key_name(bind.key);
            const ImVec2 ts = font->CalcTextSizeA(fs, FLT_MAX, 0.f, key_text);
            fg->AddText(font, fs,
                { field.GetCenter().x - ts.x * 0.5f, field.GetCenter().y - ts.y * 0.5f },
                text_col(p.capturing ? 0.95f : 0.75f), key_text);

            y += field_h + gap;
        }

        // Capture: first pressed key becomes the bind, Esc clears it.
        if (p.capturing && ImGui::GetTime() - p.opened_at > 0.1f)
        {
            if (GetAsyncKeyState(VK_ESCAPE) & 0x8000)
            {
                bind.key = 0;
                p.capturing = false;
            }
            else
            {
                for (int vk = 1; vk < 256; ++vk)
                {
                    if (vk == VK_LBUTTON)
                        continue;
                    if (GetAsyncKeyState(vk) & 0x8000)
                    {
                        bind.key = vk;
                        p.capturing = false;
                        break;
                    }
                }
            }
        }

        // --- Hold / Toggle / Always segment -----------------------------------
        {
            const ImRect seg(
                { card.Min.x + pad, y },
                { card.Max.x - pad, y + seg_h });
            fg->AddRectFilled(seg.Min, seg.Max,
                ImGui::GetColorU32(ImVec4(colors::control.x, colors::control.y,
                    colors::control.z, colors::control.w * a)),
                ui_scale::s(layout::control_round));

            const char* names[3] = { "Hold", "Toggle", "Always" };
            const float part_w = seg.GetWidth() / 3.f;
            for (int i = 0; i < 3; ++i)
            {
                const ImRect part(
                    { seg.Min.x + part_w * (float)i, seg.Min.y },
                    { seg.Min.x + part_w * (float)(i + 1), seg.Max.y });
                const bool sel = bind.mode == i;
                const bool hovered = ImGui::IsMouseHoveringRect(part.Min, part.Max);
                if (hovered && clicked)
                    bind.mode = i;

                if (sel)
                {
                    fg->AddRectFilled(part.Min, part.Max,
                        ImGui::GetColorU32(ImVec4(colors::accent.x, colors::accent.y,
                            colors::accent.z, 0.30f * a)),
                        ui_scale::s(layout::control_round));
                }
                const ImVec2 ts = font->CalcTextSizeA(fs, FLT_MAX, 0.f, names[i]);
                fg->AddText(font, fs,
                    { part.GetCenter().x - ts.x * 0.5f, part.GetCenter().y - ts.y * 0.5f },
                    text_col(sel ? 0.95f : (hovered ? 0.75f : 0.5f)), names[i]);
            }
            y += seg_h + gap;
        }

        // --- rows: label on the left, control on the right ---------------------
        const auto mini_toggle_row = [&](const char* label, bool* v)
        {
            const ImRect line({ card.Min.x + pad, y }, { card.Max.x - pad, y + line_h });
            const bool hovered = ImGui::IsMouseHoveringRect(line.Min, line.Max);
            if (hovered && clicked)
                *v = !*v;

            fg->AddText(font, fs,
                { line.Min.x, line.GetCenter().y - fs * 0.5f },
                text_col(hovered ? 0.85f : 0.6f), label);

            // Same switch as the main UI, scaled to the popup's row height.
            anim_state& sw = g_anims[ImHashStr(label)];
            ease(sw.active, *v ? 1.f : 0.f, 16.f);
            ease(sw.hover, hovered ? 1.f : 0.f, 24.f);

            const float tw = ui_scale::s(34.f), th = ui_scale::s(18.f);
            const ImRect tog(
                { line.Max.x - tw, line.GetCenter().y - th * 0.5f },
                { line.Max.x, line.GetCenter().y + th * 0.5f });
            const float tr = th * 0.5f;

            // The main UI's track is translucent black over a lighter box; here
            // the card is already dark, so an opaque track keeps the contrast.
            const ImVec4 track_bg = ImLerp(colors::toggle_off, colors::slider_bg, sw.hover);
            fg->AddRectFilled(tog.Min, tog.Max,
                ImGui::GetColorU32(ImVec4(track_bg.x, track_bg.y, track_bg.z, track_bg.w * a)), tr);

            if (sw.active > 0.01f)
            {
                fg->AddRectFilled(tog.Min, tog.Max,
                    ImGui::GetColorU32(ImVec4(colors::accent.x, colors::accent.y,
                        colors::accent.z, 0.10f * sw.active * a)), tr);
                glow::draw_accent_rect(fg, tog, tr, sw.active * 0.55f * a);
            }

            const ImVec4 knob_off = ImLerp(colors::text_muted, colors::circle_checkbox_hover, sw.hover);
            const ImVec4 knob_col = ImLerp(knob_off, colors::accent, sw.active);
            // Same knob-to-track ratio as the main switch (8 of 11 design px).
            const float knob_r = tr * (layout::toggle_knob_r / (layout::toggle_h * 0.5f));
            const float knob_x = ImLerp(tog.Min.x + tr, tog.Max.x - tr, sw.active);
            fg->AddCircleFilled({ knob_x, tog.GetCenter().y }, knob_r,
                ImGui::GetColorU32(ImVec4(knob_col.x, knob_col.y, knob_col.z, knob_col.w * a)), 24);

            y += line_h + ui_scale::s(4.f);
        };

        // Compact drag-slider used for the explicit float payload values.
        const auto mini_value_row = [&](const char* label, float* v, float vmin, float vmax, int slot)
        {
            const ImRect line({ card.Min.x + pad, y }, { card.Max.x - pad, y + line_h });
            fg->AddText(font, fs,
                { line.Min.x, line.GetCenter().y - fs * 0.5f },
                text_col(0.6f), label);

            const float track_w = ui_scale::s(56.f);
            const ImRect track(
                { line.Max.x - track_w, line.GetCenter().y - ui_scale::s(3.f) },
                { line.Max.x, line.GetCenter().y + ui_scale::s(3.f) });
            const ImRect grab(
                { track.Min.x, line.Min.y },
                { track.Max.x, line.Max.y });

            static int  drag_slot = -1;
            const bool hovered = ImGui::IsMouseHoveringRect(grab.Min, grab.Max);
            if (hovered && clicked)
                drag_slot = slot;
            if (!ImGui::IsMouseDown(ImGuiMouseButton_Left))
                drag_slot = -1;
            if (drag_slot == slot)
            {
                const float t = ImClamp(
                    (ImGui::GetIO().MousePos.x - track.Min.x) / ImMax(track.GetWidth(), 1.f),
                    0.f, 1.f);
                *v = vmin + t * (vmax - vmin);
                blocks_drag = true;
            }

            const float t = ImClamp((*v - vmin) / ImMax(vmax - vmin, 0.0001f), 0.f, 1.f);
            // Opaque rail, like the switch track: translucent black would vanish
            // into the card.
            fg->AddRectFilled(track.Min, track.Max,
                ImGui::GetColorU32(ImVec4(colors::slider_bg.x, colors::slider_bg.y,
                    colors::slider_bg.z, colors::slider_bg.w * a)),
                track.GetHeight() * 0.5f);
            if (t > 0.01f)
            {
                fg->AddRectFilled(track.Min,
                    { track.Min.x + track.GetWidth() * t, track.Max.y },
                    ImGui::GetColorU32(ImVec4(colors::accent.x, colors::accent.y,
                        colors::accent.z, 0.9f * a)),
                    track.GetHeight() * 0.5f);
            }

            char buf[16];
            ImFormatString(buf, IM_ARRAYSIZE(buf), "%.0f", *v);
            const ImVec2 ts = font->CalcTextSizeA(fs, FLT_MAX, 0.f, buf);
            fg->AddText(font, fs,
                { track.Min.x - ts.x - ui_scale::s(8.f), line.GetCenter().y - ts.y * 0.5f },
                text_col(0.85f), buf);

            y += line_h + ui_scale::s(4.f);
        };

        // Combo payload: click cycles through the items.
        const auto mini_cycle_row = [&](const char* label, int* index)
        {
            const int count = (int)bind.items.size();
            const ImRect line({ card.Min.x + pad, y }, { card.Max.x - pad, y + line_h });
            fg->AddText(font, fs,
                { line.Min.x, line.GetCenter().y - fs * 0.5f },
                text_col(0.6f), label);

            const float bw = ui_scale::s(84.f);
            const ImRect btn(
                { line.Max.x - bw, line.Min.y + ui_scale::s(2.f) },
                { line.Max.x, line.Max.y - ui_scale::s(2.f) });
            const bool hovered = ImGui::IsMouseHoveringRect(btn.Min, btn.Max);
            if (hovered && clicked && count > 0)
                *index = (*index + 1) % count;

            const ImVec4 bg = hovered ? colors::control_hover : colors::control;
            fg->AddRectFilled(btn.Min, btn.Max,
                ImGui::GetColorU32(ImVec4(bg.x, bg.y, bg.z, bg.w * a)),
                ui_scale::s(3.f));

            const char* item = (count > 0 && *index >= 0 && *index < count)
                ? bind.items[*index].c_str() : "-";
            const ImVec2 ts = font->CalcTextSizeA(fs, FLT_MAX, 0.f, item);
            fg->AddText(font, fs,
                { btn.GetCenter().x - ts.x * 0.5f, btn.GetCenter().y - ts.y * 0.5f },
                text_col(hovered ? 0.9f : 0.7f), item);

            y += line_h + ui_scale::s(4.f);
        };

        mini_toggle_row("Show in keybind list", &bind.show_in_list);

        switch (bind.kind)
        {
        case bind_bool:
            mini_toggle_row("Set value to ON", &bind.target_value);
            break;
        case bind_float:
            // No "value when off" row: releasing the bind restores whatever the
            // slider was sitting at before it engaged.
            mini_value_row("Value when on", &bind.value_on, bind.fmin, bind.fmax, 0);
            break;
        case bind_combo:
            mini_cycle_row("Item when on", &bind.index_on);
            break;
        case bind_mask:
            // Which items the bind enables while active.
            for (int i = 0; i < (int)bind.items.size(); ++i)
            {
                bool on = (bind.mask_on & (1u << i)) != 0;
                mini_toggle_row(bind.items[i].c_str(), &on);
                if (on) bind.mask_on |= (1u << i);
                else    bind.mask_on &= ~(1u << i);
            }
            break;
        }

        // --- Remove / Cancel / Save -------------------------------------------
        bool do_remove = false, do_cancel = false, do_save = false;
        {
            y += gap - ui_scale::s(4.f);
            const float total = card.GetWidth() - pad * 2.f;
            const float bgap  = ui_scale::s(5.f);
            const float bw    = (total - bgap * 2.f) / 3.f;

            struct { const char* text; ImVec4 col; bool* out; } buttons[3] = {
                { "Remove", colors::danger,     &do_remove },
                { "Cancel", colors::text_muted, &do_cancel },
                { "Save",   colors::text,       &do_save   },
            };

            for (int i = 0; i < 3; ++i)
            {
                const ImRect btn(
                    { card.Min.x + pad + (bw + bgap) * (float)i, y },
                    { card.Min.x + pad + (bw + bgap) * (float)i + bw, y + btn_h });
                const bool hovered = ImGui::IsMouseHoveringRect(btn.Min, btn.Max);
                if (hovered && clicked)
                    *buttons[i].out = true;

                const ImVec4 bg = i == 2
                    ? colors::wash(hovered ? 0.14f : 0.09f)
                    : ImVec4(0.f, 0.f, 0.f, hovered ? 0.45f : 0.30f);
                fg->AddRectFilled(btn.Min, btn.Max,
                    ImGui::GetColorU32(ImVec4(bg.x, bg.y, bg.z, bg.w * a)),
                    ui_scale::s(layout::control_round));

                const ImVec4 c = buttons[i].col;
                const ImVec2 ts = font->CalcTextSizeA(fs, FLT_MAX, 0.f, buttons[i].text);
                fg->AddText(font, fs,
                    { btn.GetCenter().x - ts.x * 0.5f, btn.GetCenter().y - ts.y * 0.5f },
                    ImGui::GetColorU32(ImVec4(c.x, c.y, c.z, c.w * a * (hovered ? 1.f : 0.8f))),
                    buttons[i].text);
            }
            y += btn_h;
        }

        // Esc removes the bind once a key has been picked; while capturing it
        // only clears the key (handled above).
        const bool esc_remove = p.open && !p.capturing && ImGui::IsKeyPressed(ImGuiKey_Escape);

        if (do_remove || esc_remove)
        {
            it->second.erase(it->second.begin() + p.index);
            if (it->second.empty())
                g_binds.erase(it);
            p.open = false;
            return;
        }

        if (do_cancel)
        {
            if (p.fresh)
            {
                it->second.erase(it->second.begin() + p.index);
                if (it->second.empty())
                    g_binds.erase(it);
            }
            else
            {
                bind = p.backup;
            }
            p.open = false;
            return;
        }

        if (do_save)
        {
            p.fresh = false;
            p.open  = false;
            return;
        }

        // Close on any click outside (after the opening click has passed).
        if (p.open && clicked
            && ImGui::GetTime() - p.opened_at > 0.1f
            && !card.Contains(ImGui::GetIO().MousePos))
        {
            // A bind that never got a key is noise — drop it rather than leaving
            // a dead entry behind.
            if (p.fresh && bind.key <= 0 && bind.mode != mode_always)
            {
                it->second.erase(it->second.begin() + p.index);
                if (it->second.empty())
                    g_binds.erase(it);
            }
            p.fresh = false;
            p.open  = false;
        }

        draw_bind_menu(blocks_drag);
    }

    // --- on-screen keybind list -------------------------------------------
    void draw_keybind_list(bool menu_open)
    {
        // Collect binds that want to be listed. "Always" entries only show
        // while the menu itself is open.
        const auto listed = [&](const bind_entry& bind) {
            if (!bind.show_in_list)
                return false;
            if (bind.mode == mode_always)
                return menu_open;
            return bind.key > 0;
        };

        int count = 0;
        for (const auto& [id, binds] : g_binds)
            for (const bind_entry& bind : binds)
                if (listed(bind))
                    ++count;
        if (!count)
            return;

        ImFont* title_font = fonts::inter(14.f, true);
        ImFont* font       = fonts::inter(13.f);
        if (!font)
            return;
        const float fs  = fonts::size(font);
        const float tfs = title_font ? fonts::size(title_font) : fs;

        const float row_h   = fs + ui_scale::s(8.f);
        const float pad     = ui_scale::s(10.f);
        const float w       = ui_scale::s(190.f);
        const float title_h = tfs + ui_scale::s(14.f);
        const float h       = title_h + (float)count * row_h + pad;

        // The card is draggable by its header.
        static ImVec2 s_pos = { -1.f, -1.f };
        static bool   s_dragging = false;
        static ImVec2 s_drag_off = {};
        if (s_pos.x < 0.f)
            s_pos = { ui_scale::s(24.f), ui_scale::s(24.f) };

        const ImVec2 mouse = ImGui::GetIO().MousePos;
        const ImRect header(s_pos, s_pos + ImVec2(w, title_h));
        if (ImGui::IsMouseClicked(ImGuiMouseButton_Left) && header.Contains(mouse))
        {
            s_dragging = true;
            s_drag_off = mouse - s_pos;
        }
        if (!ImGui::IsMouseDown(ImGuiMouseButton_Left))
            s_dragging = false;
        if (s_dragging)
            s_pos = mouse - s_drag_off;

        const ImVec2 display = ImGui::GetIO().DisplaySize;
        s_pos.x = ImClamp(s_pos.x, 0.f, ImMax(0.f, display.x - w));
        s_pos.y = ImClamp(s_pos.y, 0.f, ImMax(0.f, display.y - h));

        const ImRect card(s_pos, s_pos + ImVec2(w, h));

        ImDrawList* fg = ImGui::GetForegroundDrawList();
        fg->AddRectFilled(card.Min, card.Max,
            ImGui::GetColorU32(colors::dropdown_bg), ui_scale::s(10.f));
        fg->AddRectFilled(card.Min, { card.Max.x, card.Min.y + ui_scale::s(2.f) },
            ImGui::GetColorU32(colors::accent), ui_scale::s(10.f));

        if (title_font)
        {
            fg->AddText(title_font, tfs, { card.Min.x + pad, card.Min.y + ui_scale::s(8.f) },
                ImGui::GetColorU32(colors::fg(0.9f)), "Keybinds");
        }

        float y = card.Min.y + title_h;
        for (const auto& [id, binds] : g_binds)
        {
            for (const bind_entry& bind : binds)
            {
                if (!listed(bind))
                    continue;

                const bool lit = bind.active || bind.mode == mode_always;
                fg->AddText(font, fs, { card.Min.x + pad, y },
                    ImGui::GetColorU32(colors::fg(lit ? 0.95f : 0.55f)),
                    bind.label.c_str());

                const char* key_text = bind.mode == mode_always ? "Always" : key_name(bind.key);
                const ImVec2 ts = font->CalcTextSizeA(fs, FLT_MAX, 0.f, key_text);
                fg->AddText(font, fs, { card.Max.x - pad - ts.x, y },
                    ImGui::GetColorU32(lit
                        ? colors::accent
                        : colors::fg(0.4f)),
                    key_text);

                y += row_h;
            }
        }
    }
}
