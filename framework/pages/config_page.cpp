#include "config_page.h"

#include "core/blur_renderer.h"
#include "core/config_store.h"
#include "core/menu_cloud.h"
#include "helpers/fonts.h"
#include "helpers/icons.h"
#include "helpers/settings_panel.h"
#include "helpers/widgets.h"
#include "theme/colors.h"
#include "theme/layout.h"
#include "theme/ui_scale.h"
#include "net/api.h"

#include "imgui_internal.h"

#include <algorithm>
#include <cctype>
#include <chrono>
#include <cstring>
#include <ctime>
#include <future>
#include <map>
#include <string>
#include <utility>
#include <vector>

namespace
{
    struct row_anim
    {
        float hover   = 0.f;
        float select  = 0.f;
        float visible = 1.f;
    };

    struct search_anim_state
    {
        float hover  = 0.f;
        float focus  = 0.f;
        float typed  = 0.f;
        float pulse  = 0.f;
    };

    static search_anim_state g_search_anim;

    void copy_str(char* dst, size_t dst_size, const char* src);
    void set_status(config_page::state& s, const char* msg);

    enum class cloud_job_kind
    {
        sync, save, load, create, duplicate, remove, rename, share,
        import_fetch, import_commit
    };

    struct cloud_job_result
    {
        cloud_job_kind kind = cloud_job_kind::save;
        bool ok = false;
        std::string name;
        std::string old_name;
        std::string content;
        std::string code;
        std::string error;
        std::vector<config_store::entry> entries;
    };

    std::future<cloud_job_result> g_cloud_job;
    bool g_cloud_busy = false;

    template <typename Fn>
    bool start_cloud_job(config_page::state& s, const char* status, Fn&& fn)
    {
        if (g_cloud_busy)
        {
            set_status(s, "Cloud request already in progress");
            return false;
        }
        g_cloud_busy = true;
        set_status(s, status);
        g_cloud_job = std::async(std::launch::async, std::forward<Fn>(fn));
        return true;
    }

    void add_config(config_page::state& s, const std::string& name)
    {
        if (s.count >= config_page::state::max_configs)
            return;
        copy_str(s.names[s.count], sizeof(s.names[s.count]), name.c_str());
        copy_str(s.dates[s.count], sizeof(s.dates[s.count]), "Just now");
        s.selected = s.count++;
        copy_str(s.active, sizeof(s.active), name.c_str());
        config_store::sync(s.active);
    }

    std::map<ImGuiID, row_anim> g_row_anims;

    // Index of the card whose name is being edited inline, -1 when none. The
    // buffer is separate from state::names so cancelling leaves the name intact.
    int  g_rename_index = -1;
    char g_rename_buf[config_page::state::name_len] = {};
    bool g_rename_focus = false;
    // Focus lands a frame or two after SetKeyboardFocusHere, so "lost focus"
    // only means abandon once the field has actually held it at least once.
    bool g_rename_was_active = false;

    void copy_str(char* dst, size_t dst_size, const char* src)
    {
        if (!dst || dst_size == 0)
            return;

        size_t i = 0;
        for (; src && src[i] && i + 1 < dst_size; ++i)
            dst[i] = src[i];
        dst[i] = '\0';
    }

    bool name_exists(config_page::state& s, const char* name, int skip = -1)
    {
        for (int i = 0; i < s.count; ++i)
        {
            if (i == skip)
                continue;
            if (std::strcmp(s.names[i], name) == 0)
                return true;
        }
        return false;
    }

    std::string import_name(config_page::state& s, const std::string& source)
    {
        std::string base;
        for (unsigned char c : source)
        {
            const bool allowed = std::isalnum(c) || c == ' ' || c == '-' || c == '_';
            if (allowed && base.size() + 1 < config_page::state::name_len)
                base.push_back((char)c);
        }
        while (!base.empty() && base.back() == ' ')
            base.pop_back();
        if (base.empty())
            base = "Imported config";
        if (!name_exists(s, base.c_str()))
            return base;

        for (int n = 2; n < 100; ++n)
        {
            char suffix[24];
            sprintf_s(suffix, " import %d", n);
            std::string candidate = base;
            const size_t max_base = config_page::state::name_len - 1 - std::strlen(suffix);
            if (candidate.size() > max_base)
                candidate.resize(max_base);
            candidate += suffix;
            if (!name_exists(s, candidate.c_str()))
                return candidate;
        }
        return {};
    }

    void fill_config_list(config_page::state& s,
        const std::vector<config_store::entry>& entries)
    {
        s.count = 0;
        for (const auto& entry : entries)
        {
            if (s.count >= config_page::state::max_configs)
                break;
            copy_str(s.names[s.count], sizeof(s.names[s.count]), entry.name.c_str());
            std::time_t raw = (std::time_t)entry.updated_ts;
            std::tm local{};
            char date[config_page::state::date_len] = "Cloud";
            if (entry.updated_ts > 0 && localtime_s(&local, &raw) == 0)
                std::strftime(date, sizeof(date), "%Y-%m-%d %H:%M", &local);
            copy_str(s.dates[s.count], sizeof(s.dates[s.count]), date);
            ++s.count;
        }
    }

    bool matches_search(const char* name, const char* search)
    {
        if (!search || search[0] == '\0')
            return true;

        for (int i = 0; name[i]; ++i)
        {
            int j = 0;
            for (; search[j]; ++j)
            {
                char a = name[i + j];
                char b = search[j];
                if (a >= 'A' && a <= 'Z') a = (char)(a - 'A' + 'a');
                if (b >= 'A' && b <= 'Z') b = (char)(b - 'A' + 'a');
                if (!a || a != b)
                    break;
            }
            if (search[j] == '\0')
                return true;
        }
        return false;
    }

    void set_status(config_page::state& s, const char* msg)
    {
        copy_str(s.status, sizeof(s.status), msg);
        s.status_alpha = 0.f;
    }

    // A config name becomes a filename on disk and a value sent to the cloud
    // endpoint, so the field only accepts what both sides accept: letters,
    // digits, space, dash, underscore. Rejecting the keystroke is better than
    // silently rewriting the name afterwards — the user sees the limit as they
    // type instead of finding a mangled name later.
    int name_filter(ImGuiInputTextCallbackData* data)
    {
        const ImWchar c = data->EventChar;
        const bool allowed = (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z')
            || (c >= '0' && c <= '9') || c == ' ' || c == '-' || c == '_';
        return allowed ? 0 : 1;
    }

    void set_share_status(config_page::state& s, const char* msg)
    {
        copy_str(s.share.status, sizeof(s.share.status), msg);
    }

    void draw_search_field(ImDrawList* dl, const ImRect& bar, config_page::state& s, bool& blocks_drag)
    {
        const float dt = ImGui::GetIO().DeltaTime;
        const bool mouse_on_bar = ImGui::IsMouseHoveringRect(bar.Min, bar.Max, false);
        g_search_anim.typed = ImLerp(g_search_anim.typed, s.search[0] ? 1.f : 0.f, ImMin(1.f, dt * 14.f));
        g_search_anim.pulse += dt * (g_search_anim.focus > 0.5f ? 4.f : 0.f);

        const ImVec4 bg_col = ImLerp(colors::control, colors::control_hover, g_search_anim.hover * 0.65f);
        const float control_round = ui_scale::s(layout::control_round);
        dl->AddRectFilled(bar.Min, bar.Max, ImGui::GetColorU32(bg_col), control_round);

        const float accent_mix = ImMax(g_search_anim.focus, g_search_anim.typed * 0.55f);
        if (accent_mix > 0.01f)
        {
            dl->AddRect(bar.Min, bar.Max,
                ImGui::GetColorU32(ImVec4(
                    colors::accent.x, colors::accent.y, colors::accent.z, 0.18f + accent_mix * 0.45f)),
                control_round, 0, 1.f);
        }

        const float underline_w = bar.GetWidth() * (0.25f + g_search_anim.focus * 0.75f);
        const float pulse = 0.5f + 0.5f * sinf(g_search_anim.pulse);
        const float underline_a = (0.08f + g_search_anim.focus * 0.55f) * pulse;
        dl->AddRectFilled(
            { bar.GetCenter().x - underline_w * 0.5f, bar.Max.y - ui_scale::s(2.f) },
            { bar.GetCenter().x + underline_w * 0.5f, bar.Max.y },
            ImGui::GetColorU32(ImVec4(colors::accent.x, colors::accent.y, colors::accent.z, underline_a)),
            1.f);

        {
            const ImVec4 icon_col = ImLerp(
                colors::text_muted,
                colors::accent,
                g_search_anim.focus * 0.65f + g_search_anim.typed * 0.35f);
            const float icon_side = ui_scale::s(16.f);
            const ImVec2 icon_min = { bar.Min.x + ui_scale::s(10.f), bar.GetCenter().y - icon_side * 0.5f };
            icons::draw(dl, "search",
                ImRect(icon_min, { icon_min.x + icon_side, icon_min.y + icon_side }),
                ImGui::GetColorU32(icon_col));
        }

        ImGui::SetCursorScreenPos({ bar.Min.x + ui_scale::s(32.f), bar.Min.y + ui_scale::s(6.f) });
        ImGui::PushItemWidth(bar.GetWidth() - ui_scale::s(42.f));
        ImGui::PushStyleVar(ImGuiStyleVar_FramePadding, { 0.f, 2.f });
        ImGui::PushStyleVar(ImGuiStyleVar_FrameBorderSize, 0.f);
        ImGui::PushStyleColor(ImGuiCol_FrameBg, ImVec4(0.f, 0.f, 0.f, 0.f));
        ImGui::PushStyleColor(ImGuiCol_FrameBgHovered, ImVec4(0.f, 0.f, 0.f, 0.f));
        ImGui::PushStyleColor(ImGuiCol_FrameBgActive, ImVec4(0.f, 0.f, 0.f, 0.f));
        ImGui::PushStyleColor(ImGuiCol_Border, ImVec4(0.f, 0.f, 0.f, 0.f));
        ImGui::PushStyleColor(ImGuiCol_Text, colors::text);

        if (ImFont* font = fonts::inter(18.f))
            ImGui::PushFont(font);

        // Hint is drawn by hand so it can sit centered — InputTextWithHint always left-aligns it.
        if (s.search[0] == '\0')
        {
            if (ImFont* hint_font = fonts::inter(18.f))
            {
                const float sz = fonts::size(hint_font);
                const char* hint = "Search configs...";
                const ImVec2 ts = hint_font->CalcTextSizeA(sz, FLT_MAX, 0.f, hint);
                dl->AddText(hint_font, sz,
                    { bar.GetCenter().x - ts.x * 0.5f, bar.GetCenter().y - ts.y * 0.5f },
                    ImGui::GetColorU32(colors::text_muted), hint);
            }
        }

        ImGui::InputText("##cfg_search_input", s.search, sizeof(s.search));

        const bool input_active = ImGui::IsItemActive();
        const bool input_hovered = ImGui::IsItemHovered();
        if (input_active || input_hovered || mouse_on_bar)
            blocks_drag = true;

        g_search_anim.hover = ImLerp(g_search_anim.hover,
            (mouse_on_bar || input_hovered || input_active) ? 1.f : 0.f, ImMin(1.f, dt * 20.f));
        g_search_anim.focus = ImLerp(g_search_anim.focus, input_active ? 1.f : 0.f, ImMin(1.f, dt * 22.f));

        if (ImFont* font = fonts::inter(18.f))
            ImGui::PopFont();

        ImGui::PopStyleColor(5);
        ImGui::PopStyleVar(2);
        ImGui::PopItemWidth();
    }

    void draw_config_card(ImDrawList* dl, const ImRect& item, config_page::state& s, int index,
        bool& blocks_drag)
    {
        ImGuiID id = ImGui::GetID((void*)(intptr_t)(index + 100));
        // AllowOverlap: the pencil sits on top of the card, and without this the
        // card claims the active id on mouse-down, which makes the pencil
        // unhoverable for the rest of the click.
        ImGui::SetNextItemAllowOverlap();
        ImGui::ItemAdd(item, id);

        bool hovered = false, held = false;
        const bool pressed = ImGui::ButtonBehavior(item, id, &hovered, &held,
            ImGuiButtonFlags_AllowOverlap);
        if (hovered || held)
            blocks_drag = true;

        // AllowOverlap costs the card its hover flag the moment the cursor
        // reaches the pencil on top of it, which would make the pencil fade out
        // from under the pointer. These two flags read the hover back through
        // the overlapping item and through the pencil holding the active id.
        const bool row_hovered = hovered || held || ImGui::IsItemHovered(
            ImGuiHoveredFlags_AllowWhenOverlappedByItem | ImGuiHoveredFlags_AllowWhenBlockedByActiveItem);

        row_anim& anim = g_row_anims[id];
        const float dt = ImGui::GetIO().DeltaTime;
        const bool matched = matches_search(s.names[index], s.search);
        anim.hover   = ImLerp(anim.hover,   row_hovered ? 1.f : 0.f, ImMin(1.f, dt * 18.f));
        anim.select  = ImLerp(anim.select,  index == s.selected ? 1.f : 0.f, ImMin(1.f, dt * 16.f));
        anim.visible = ImLerp(anim.visible, matched ? 1.f : 0.f, ImMin(1.f, dt * 16.f));

        if (anim.visible < 0.02f)
        {
            if (pressed && matched)
                s.selected = index;
            return;
        }

        const float slide = (1.f - anim.visible) * ui_scale::s(8.f);
        const ImRect draw_item(
            { item.Min.x, item.Min.y + slide },
            { item.Max.x, item.Max.y + slide });

        auto col_a = [&](ImVec4 c)
        {
            c.w *= anim.visible;
            return ImGui::GetColorU32(c);
        };

        const ImVec4 card_bg = ImLerp(
            colors::box,
            ImVec4(colors::accent.x, colors::accent.y, colors::accent.z, 0.08f),
            anim.select);
        dl->AddRectFilled(draw_item.Min, draw_item.Max, col_a(card_bg), ui_scale::s(layout::config_card_round));

        const ImVec4 border_col = ImLerp(
            ImVec4(colors::separator.x, colors::separator.y, colors::separator.z, 0.35f),
            ImVec4(colors::accent.x, colors::accent.y, colors::accent.z, 0.45f),
            anim.select + anim.hover * 0.25f);
        dl->AddRect(draw_item.Min, draw_item.Max, col_a(border_col), ui_scale::s(layout::config_card_round), 0, 1.f);

        // Same 20 px the rest of the menu labels use — the card is the widest
        // row on the page, so a smaller size read as an accident here.
        ImFont* font = fonts::inter(layout::row_label_font);
        ImFont* meta_font = fonts::inter(15.f);

        const float icon_side = ui_scale::s(17.f);
        const float icon_x    = draw_item.Min.x + ui_scale::s(12.f);
        {
            const ImVec2 icon_min = { icon_x, draw_item.GetCenter().y - icon_side * 0.5f };
            icons::draw(dl, "folder",
                ImRect(icon_min, { icon_min.x + icon_side, icon_min.y + icon_side }),
                col_a(ImLerp(colors::text_muted, colors::accent, anim.select * 0.85f)));
        }

        // The badge is only painted on the active card, but its footprint is always
        // reserved: the two text lines centre inside the gap left between the folder
        // icon and the badge, so selecting a card must not shuffle the text sideways.
        ImFont* badge_font = fonts::inter(12.f);
        if (!badge_font)
            badge_font = font;
        const char* badge = "Active";
        const float  badge_size = badge_font ? fonts::size(badge_font) : 0.f;
        const ImVec2 badge_ts   = badge_font
            ? badge_font->CalcTextSizeA(badge_size, FLT_MAX, 0.f, badge)
            : ImVec2(0.f, 0.f);
        const float badge_pad_x = ui_scale::s(8.f);
        const float badge_pad_y = ui_scale::s(4.f);
        const float badge_w = badge_ts.x + badge_pad_x * 2.f;
        const float badge_h = badge_ts.y + badge_pad_y * 2.f;
        const ImRect badge_bb(
            { draw_item.Max.x - badge_w - ui_scale::s(10.f), draw_item.GetCenter().y - badge_h * 0.5f },
            { draw_item.Max.x - ui_scale::s(10.f), draw_item.GetCenter().y + badge_h * 0.5f });

        // Rename affordance, tucked left of the badge footprint so the two never
        // overlap on the active card.
        const float pen_side = ui_scale::s(16.f);
        const ImRect pen_bb(
            { badge_bb.Min.x - ui_scale::s(10.f) - pen_side, draw_item.GetCenter().y - pen_side * 0.5f },
            { badge_bb.Min.x - ui_scale::s(10.f), draw_item.GetCenter().y + pen_side * 0.5f });

        const bool renaming = g_rename_index == index;

        const float text_cx = (icon_x + icon_side + pen_bb.Min.x) * 0.5f;

        if (renaming)
        {
            // The text block is replaced by a live field for the duration.
            const float field_h = ui_scale::s(layout::control_h);
            const ImRect field(
                { icon_x + icon_side + ui_scale::s(10.f), draw_item.GetCenter().y - field_h * 0.5f },
                { pen_bb.Min.x - ui_scale::s(8.f), draw_item.GetCenter().y + field_h * 0.5f });

            dl->AddRectFilled(field.Min, field.Max, col_a(colors::control), ui_scale::s(layout::control_round));

            ImGui::SetCursorScreenPos({ field.Min.x + ui_scale::s(8.f), field.GetCenter().y - ui_scale::s(9.f) });
            ImGui::PushItemWidth(field.GetWidth() - ui_scale::s(16.f));
            ImGui::PushStyleVar(ImGuiStyleVar_FramePadding, { 0.f, 0.f });
            ImGui::PushStyleColor(ImGuiCol_FrameBg, ImVec4(0.f, 0.f, 0.f, 0.f));
            ImGui::PushStyleColor(ImGuiCol_FrameBgHovered, ImVec4(0.f, 0.f, 0.f, 0.f));
            ImGui::PushStyleColor(ImGuiCol_FrameBgActive, ImVec4(0.f, 0.f, 0.f, 0.f));
            ImGui::PushStyleColor(ImGuiCol_Text, colors::text);
            if (font)
                ImGui::PushFont(font);

            if (g_rename_focus)
            {
                ImGui::SetKeyboardFocusHere();
                g_rename_focus = false;
            }

            const bool committed = ImGui::InputText("##cfg_rename", g_rename_buf, sizeof(g_rename_buf),
                ImGuiInputTextFlags_EnterReturnsTrue | ImGuiInputTextFlags_AutoSelectAll
                | ImGuiInputTextFlags_CallbackCharFilter, name_filter);
            const bool field_active = ImGui::IsItemActive();
            if (field_active)
                g_rename_was_active = true;
            blocks_drag = true;

            if (font)
                ImGui::PopFont();
            ImGui::PopStyleColor(4);
            ImGui::PopStyleVar();
            ImGui::PopItemWidth();

            // Enter commits, Escape or losing focus abandons the edit. The
            // focus check waits for g_rename_was_active: the field is not yet
            // active on the frame the focus is requested, and treating that as
            // a lost focus closed the editor before it ever drew.
            const bool cancelled = ImGui::IsKeyPressed(ImGuiKey_Escape) || (!field_active && g_rename_was_active);
            if (committed)
            {
                if (g_rename_buf[0] == '\0')
                    set_status(s, "Enter a config name");
                else if (name_exists(s, g_rename_buf, index))
                    set_status(s, "Config already exists");
                else
                {
                    const std::string old_name = s.names[index];
                    const std::string new_name = g_rename_buf;
                    start_cloud_job(s, "Renaming cloud config...", [old_name, new_name] {
                        cloud_job_result r;
                        r.kind = cloud_job_kind::rename;
                        r.old_name = old_name;
                        r.name = new_name;
                        long long updated = 0;
                        r.ok = api::rename_named_config(old_name, new_name, updated, r.error);
                        return r;
                    });
                }
                g_rename_index = -1;
            }
            else if (cancelled)
            {
                g_rename_index = -1;
            }
        }
        else
        {
            const float title_size = font ? fonts::size(font) : 0.f;
            const ImVec2 title_ts = font
                ? font->CalcTextSizeA(title_size, FLT_MAX, 0.f, s.names[index])
                : ImVec2(0.f, 0.f);
            const float meta_size = meta_font ? fonts::size(meta_font) : 0.f;
            const ImVec2 meta_ts = meta_font
                ? meta_font->CalcTextSizeA(meta_size, FLT_MAX, 0.f, s.dates[index])
                : ImVec2(0.f, 0.f);

            const float line_gap = ui_scale::s(4.f);
            const float block_h  = title_ts.y + line_gap + meta_ts.y;
            const float block_top = draw_item.GetCenter().y - block_h * 0.5f;

            if (font)
            {
                dl->AddText(font, title_size, { text_cx - title_ts.x * 0.5f, block_top },
                    col_a(ImLerp(colors::text_muted, colors::text, anim.select * 0.85f + anim.hover * 0.15f)),
                    s.names[index]);
            }
            if (meta_font)
            {
                dl->AddText(meta_font, meta_size,
                    { text_cx - meta_ts.x * 0.5f, block_top + title_ts.y + line_gap },
                    col_a(colors::text_muted), s.dates[index]);
            }
        }

        if (index == s.selected && badge_font)
        {
            dl->AddRectFilled(badge_bb.Min, badge_bb.Max,
                ImGui::GetColorU32(ImVec4(colors::accent.x, colors::accent.y, colors::accent.z, 0.22f)),
                badge_h * 0.5f);
            dl->AddText(badge_font, badge_size,
                { badge_bb.GetCenter().x - badge_ts.x * 0.5f, badge_bb.GetCenter().y - badge_ts.y * 0.5f },
                ImGui::GetColorU32(colors::accent), badge);
        }

        // Submitted after the card so it wins the click, and skipped while this
        // card is the one being edited. It only exists under the cursor: the
        // hit box follows the same fade as the glyph, so an invisible pencil is
        // never clickable.
        bool pen_pressed = false;
        if (!renaming && anim.hover > 0.15f)
        {
            ImGuiID pen_id = ImGui::GetID((void*)(intptr_t)(index + 400));
            ImGui::ItemAdd(pen_bb, pen_id);
            bool pen_hovered = false, pen_held = false;
            pen_pressed = ImGui::ButtonBehavior(pen_bb, pen_id, &pen_hovered, &pen_held);
            if (pen_hovered || pen_held)
                blocks_drag = true;

            ImVec4 pen_col = pen_hovered ? colors::text : colors::text_muted;
            pen_col.w *= anim.hover;
            icons::draw(dl, "pencil", pen_bb, col_a(pen_col));

            if (pen_pressed)
            {
                g_rename_index = index;
                g_rename_focus = true;
                g_rename_was_active = false;
                copy_str(g_rename_buf, sizeof(g_rename_buf), s.names[index]);
                s.selected = index;
                set_status(s, "Editing name");
            }
        }

        if (pressed && !pen_pressed && !renaming)
            s.selected = index;
    }

    float draw_config_cards(ImDrawList* dl, const ImVec2& origin, float width, config_page::state& s,
        bool& blocks_drag)
    {
        float y = origin.y;
        int visible = 0;

        for (int i = 0; i < s.count; ++i)
        {
            if (!matches_search(s.names[i], s.search))
            {
                ImGuiID id = ImGui::GetID((void*)(intptr_t)(i + 100));
                row_anim& anim = g_row_anims[id];
                anim.visible = ImLerp(anim.visible, 0.f, ImMin(1.f, ImGui::GetIO().DeltaTime * 16.f));
                continue;
            }

            const float item_h = ui_scale::s(layout::config_item_h);
            const ImRect item({ origin.x, y }, { origin.x + width, y + item_h });
            draw_config_card(dl, item, s, i, blocks_drag);
            y += item_h + ui_scale::s(layout::config_item_gap);
            ++visible;
        }

        if (visible == 0)
        {
            ImFont* empty_font = fonts::inter(16.f);
            if (empty_font)
            {
                const char* empty = s.search[0] ? "No configs match your search" : "No configs yet";
                const float empty_size = fonts::size(empty_font);
                const ImVec2 ts = empty_font->CalcTextSizeA(empty_size, FLT_MAX, 0.f, empty);
                dl->AddText(empty_font, empty_size,
                    { origin.x + (width - ts.x) * 0.5f, y + ui_scale::s(24.f) },
                    ImGui::GetColorU32(colors::text_muted), empty);
            }
            y += ui_scale::s(48.f);
        }

        return y;
    }

    bool draw_action_button(ImDrawList* dl, const char* id, const char* label, const ImRect& rect,
        bool& blocks_drag, bool accent = false, bool danger = false)
    {
        ImGuiID gid = ImGui::GetID(id);
        ImGui::ItemAdd(rect, gid);

        bool hovered = false, held = false;
        const bool pressed = ImGui::ButtonBehavior(rect, gid, &hovered, &held);
        if (hovered || held)
            blocks_drag = true;

        static std::map<ImGuiID, float> btn_hover;
        float& h = btn_hover[gid];
        h = ImLerp(h, hovered || held ? 1.f : 0.f, ImMin(1.f, ImGui::GetIO().DeltaTime * 18.f));

        ImVec4 bg = colors::control;
        ImVec4 text_col = colors::text;

        if (accent)
        {
            bg = ImLerp(
                ImVec4(colors::accent.x * 0.82f, colors::accent.y * 0.82f, colors::accent.z * 0.82f, 1.f),
                colors::accent,
                h);
            text_col = colors::text;
        }
        else if (danger)
        {
            bg = ImLerp(colors::control, ImVec4(colors::danger.x, colors::danger.y, colors::danger.z, 0.35f), h);
            text_col = ImLerp(colors::text, colors::danger, 0.55f + h * 0.45f);
        }
        else
        {
            bg = ImLerp(colors::control, colors::control_hover, h);
            text_col = ImLerp(colors::text_muted, colors::text, 0.65f + h * 0.35f);
        }

        dl->AddRectFilled(rect.Min, rect.Max, ImGui::GetColorU32(bg), ui_scale::s(layout::control_round));

        if (ImFont* font = fonts::inter(16.f))
        {
            const float label_size = fonts::size(font);
            const ImVec2 ts = font->CalcTextSizeA(label_size, FLT_MAX, 0.f, label);
            dl->AddText(font, label_size,
                { rect.Min.x + (rect.GetWidth() - ts.x) * 0.5f, rect.GetCenter().y - ts.y * 0.5f },
                ImGui::GetColorU32(text_col), label);
        }

        return pressed;
    }

    void draw_section_label(ImDrawList* dl, const ImVec2& pos, const char* label)
    {
        ImFont* font = fonts::inter(14.f, true);
        if (!font) font = fonts::inter(14.f);
        if (font)
            dl->AddText(font, fonts::size(font), pos, ImGui::GetColorU32(colors::header_text), label);
    }

    void draw_profile_summary(ImDrawList* dl, const ImRect& box, config_page::state& s, bool& blocks_drag)
    {
        (void)blocks_drag;

        const float pad = ui_scale::s(layout::box_pad_x);
        const ImRect inner(
            { box.Min.x + pad, box.Min.y + ui_scale::s(layout::box_pad_y) },
            { box.Max.x - pad, box.Min.y + ui_scale::s(layout::box_pad_y + 72.f) });

        dl->AddRectFilled(inner.Min, inner.Max, ImGui::GetColorU32(colors::control), ui_scale::s(layout::config_card_round));

        ImFont* label_font = fonts::inter(14.f, true);
        if (!label_font) label_font = fonts::inter(14.f);
        ImFont* title_font = fonts::inter(18.f, true);
        if (!title_font) title_font = fonts::inter(18.f);
        ImFont* meta_font  = fonts::inter(14.f);

        {
            const float icon_side = ui_scale::s(17.f);
            const ImVec2 icon_min = { inner.Min.x + ui_scale::s(10.f), inner.Min.y + ui_scale::s(11.f) };
            icons::draw(dl, "folder",
                ImRect(icon_min, { icon_min.x + icon_side, icon_min.y + icon_side }),
                ImGui::GetColorU32(colors::accent));
        }

        char date_line[64];
        ImFormatString(date_line, IM_ARRAYSIZE(date_line), "Updated %s", s.dates[s.selected]);

        // Three lines behave as one block centred in the inner card, so a taller or
        // shorter font never leaves lopsided padding above and below.
        struct line { ImFont* font; const char* text; ImVec4 col; float size; ImVec2 ts; };
        line lines[] = {
            { label_font, "Selected profile", colors::header_text, 0.f, {} },
            { title_font, s.names[s.selected], colors::text,       0.f, {} },
            { meta_font,  date_line,           colors::text_muted, 0.f, {} },
        };

        const float line_gap = ui_scale::s(5.f);
        float block_h = 0.f;
        for (int i = 0; i < IM_ARRAYSIZE(lines); ++i)
        {
            if (!lines[i].font)
                continue;
            lines[i].size = fonts::size(lines[i].font);
            lines[i].ts   = lines[i].font->CalcTextSizeA(lines[i].size, FLT_MAX, 0.f, lines[i].text);
            block_h += lines[i].ts.y + (block_h > 0.f ? line_gap : 0.f);
        }

        float y = inner.GetCenter().y - block_h * 0.5f;
        for (const line& l : lines)
        {
            if (!l.font)
                continue;
            dl->AddText(l.font, l.size, { inner.GetCenter().x - l.ts.x * 0.5f, y },
                ImGui::GetColorU32(l.col), l.text);
            y += l.ts.y + line_gap;
        }
    }

    float draw_name_field(ImDrawList* dl, const ImRect& field, config_page::state& s, bool& blocks_drag)
    {
        const bool mouse_on_field = ImGui::IsMouseHoveringRect(field.Min, field.Max, false);

        static float name_hover = 0.f;

        const ImVec4 bg = ImLerp(colors::control, colors::control_hover, name_hover);
        dl->AddRectFilled(field.Min, field.Max, ImGui::GetColorU32(bg), ui_scale::s(layout::control_round));

        ImGui::SetCursorScreenPos({ field.Min.x + ui_scale::s(8.f), field.Min.y + ui_scale::s(5.f) });
        ImGui::PushItemWidth(field.GetWidth() - ui_scale::s(16.f));
        ImGui::PushStyleVar(ImGuiStyleVar_FramePadding, { 0.f, 0.f });
        ImGui::PushStyleColor(ImGuiCol_FrameBg, ImVec4(0.f, 0.f, 0.f, 0.f));
        ImGui::PushStyleColor(ImGuiCol_FrameBgHovered, ImVec4(0.f, 0.f, 0.f, 0.f));
        ImGui::PushStyleColor(ImGuiCol_FrameBgActive, ImVec4(0.f, 0.f, 0.f, 0.f));
        ImGui::PushStyleColor(ImGuiCol_Text, colors::text);

        if (ImFont* font = fonts::inter(18.f))
            ImGui::PushFont(font);

        ImGui::InputTextWithHint("##cfg_new_name_input", "Enter name...", s.new_name, sizeof(s.new_name),
            ImGuiInputTextFlags_CallbackCharFilter, name_filter);

        const bool input_active = ImGui::IsItemActive();
        const bool input_hovered = ImGui::IsItemHovered();
        if (input_active || input_hovered || mouse_on_field)
            blocks_drag = true;

        name_hover = ImLerp(name_hover,
            (mouse_on_field || input_hovered || input_active) ? 1.f : 0.f,
            ImMin(1.f, ImGui::GetIO().DeltaTime * 18.f));

        if (ImFont* font = fonts::inter(18.f))
            ImGui::PopFont();

        ImGui::PopStyleColor(4);
        ImGui::PopStyleVar();
        ImGui::PopItemWidth();

        return field.Max.y;
    }

    // Manage box metrics, in design px. Shared between the height calculation and
    // the grid itself so the Create button can never outgrow its container again.
    constexpr float manage_grid_top = layout::box_pad_y + 82.f;
    constexpr float manage_btn_h    = 32.f;
    constexpr float manage_gap      = 6.f;

    float manage_box_height()
    {
        const float grid_bottom = manage_grid_top + manage_btn_h * 2.f + manage_gap;
        const float sep_y       = grid_bottom + 10.f;
        const float name_bottom = sep_y + 28.f + layout::control_h;
        const float create_bottom = name_bottom + 8.f + manage_btn_h;
        return create_bottom + layout::box_pad_y;
    }

    void draw_action_grid(ImDrawList* dl, const ImRect& box, config_page::state& s, bool& blocks_drag, float& content_bottom)
    {
        const float pad = ui_scale::s(layout::box_pad_x);
        const float top = box.Min.y + ui_scale::s(manage_grid_top);
        const float gap = ui_scale::s(manage_gap);
        const float btn_w = (box.GetWidth() - pad * 2.f - gap) * 0.5f;
        const float btn_h = ui_scale::s(manage_btn_h);

        const ImRect load_bb({ box.Min.x + pad, top }, { box.Min.x + pad + btn_w, top + btn_h });
        const ImRect share_bb({ load_bb.Max.x + gap, top }, { load_bb.Max.x + gap + btn_w, top + btn_h });
        const ImRect dup_bb({ box.Min.x + pad, top + btn_h + gap },
            { box.Min.x + pad + btn_w, top + btn_h * 2.f + gap });
        const ImRect del_bb({ dup_bb.Max.x + gap, dup_bb.Min.y },
            { dup_bb.Max.x + gap + btn_w, dup_bb.Max.y });

        if (draw_action_button(dl, "cfg_load_btn", "Load", load_bb, blocks_drag, true))
        {
            const std::string name = s.names[s.selected];
            start_cloud_job(s, "Loading cloud config...", [name] {
                cloud_job_result r;
                r.kind = cloud_job_kind::load;
                r.name = name;
                long long updated = 0;
                r.ok = api::get_named_config(name, r.content, updated, r.error);
                return r;
            });
        }

        if (draw_action_button(dl, "cfg_share_btn", "Share", share_bb, blocks_drag, true))
            config_page::share_current(s);

        if (draw_action_button(dl, "cfg_dup_btn", "Duplicate", dup_bb, blocks_drag))
        {
            if (s.count >= config_page::state::max_configs)
                set_status(s, "Config list is full");
            else
            {
                char copy_name[config_page::state::name_len];
                ImFormatString(copy_name, IM_ARRAYSIZE(copy_name), "%s copy", s.names[s.selected]);
                // The copy starts from what is in memory, which is the source
                // config's own values.
                const std::string name = copy_name;
                const std::string content = config_store::serialize();
                start_cloud_job(s, "Duplicating cloud config...", [name, content] {
                    cloud_job_result r;
                    r.kind = cloud_job_kind::duplicate;
                    r.name = name;
                    r.content = content;
                    long long updated = 0;
                    r.ok = api::put_named_config(name, content, updated, r.error);
                    return r;
                });
            }
        }

        if (draw_action_button(dl, "cfg_del_btn", "Delete", del_bb, blocks_drag, false, true))
        {
            if (s.count <= 1)
                set_status(s, "Cannot delete last config");
            else
            {
                const std::string name = s.names[s.selected];
                start_cloud_job(s, "Deleting cloud config...", [name] {
                    cloud_job_result r;
                    r.kind = cloud_job_kind::remove;
                    r.name = name;
                    r.ok = api::delete_named_config(name, r.error);
                    return r;
                });
            }
        }

        const float sep_y = del_bb.Max.y + ui_scale::s(10.f);
        dl->AddLine(
            { box.Min.x + pad, sep_y },
            { box.Max.x - pad, sep_y },
            ImGui::GetColorU32(colors::separator), ui_scale::s(layout::separator_h));

        draw_section_label(dl, { box.Min.x + pad, sep_y + ui_scale::s(10.f) }, "Create new");

        const ImRect name_field({ box.Min.x + pad, sep_y + ui_scale::s(28.f) },
            { box.Max.x - pad, sep_y + ui_scale::s(28.f) + ui_scale::s(layout::control_h) });
        draw_name_field(dl, name_field, s, blocks_drag);

        const ImRect create_bb({ box.Min.x + pad, name_field.Max.y + ui_scale::s(8.f) },
            { box.Max.x - pad, name_field.Max.y + ui_scale::s(8.f) + btn_h });
        if (draw_action_button(dl, "cfg_create_btn", "Create config", create_bb, blocks_drag, true))
        {
            if (s.new_name[0] == '\0')
                set_status(s, "Enter a config name");
            else if (name_exists(s, s.new_name))
                set_status(s, "Config already exists");
            else if (s.count >= config_page::state::max_configs)
                set_status(s, "Config list is full");
            else
            {
                const std::string name = s.new_name;
                const std::string content = config_store::serialize();
                if (start_cloud_job(s, "Creating cloud config...", [name, content] {
                    cloud_job_result r;
                    r.kind = cloud_job_kind::create;
                    r.name = name;
                    r.content = content;
                    long long updated = 0;
                    r.ok = api::put_named_config(name, content, updated, r.error);
                    return r;
                }))
                    s.new_name[0] = '\0';
            }
        }

        content_bottom = create_bb.Max.y;
    }

    // The code field accepts the alphabet and nothing else. Rejecting at the
    // keystroke means a pasted SQL fragment, path, or stray whitespace never
    // reaches the buffer, let alone the server — uppercase is folded rather
    // than refused because a code copied out of a chat often arrives that way.
    int share_code_filter(ImGuiInputTextCallbackData* data)
    {
        ImWchar c = data->EventChar;
        if (c >= 'A' && c <= 'Z')
            c = (ImWchar)(c - 'A' + 'a');

        const bool allowed = (c >= 'a' && c <= 'z') || (c >= '0' && c <= '9');
        if (!allowed)
            return 1;

        data->EventChar = c;
        return 0;
    }

    void prepare_share_code(config_page::state& s)
    {
        const int selected = std::clamp(s.selected, 0, ImMax(s.count - 1, 0));
        const std::string name = s.names[selected];
        const std::string content = name == s.active ? config_store::serialize() : std::string();
        s.share.code[0] = '\0';
        set_share_status(s, "Creating share code...");
        if (!start_cloud_job(s, "Creating share code...", [name, content] {
            cloud_job_result r;
            r.kind = cloud_job_kind::share;
            r.name = name;
            r.content = content;
            if (r.content.empty())
            {
                long long updated = 0;
                if (!api::get_named_config(name, r.content, updated, r.error))
                    return r;
            }
            r.ok = api::save_cloud_config(name, r.content, r.code, r.error);
            return r;
        }))
            set_share_status(s, "Cloud request already in progress");
    }

    void draw_share_overlay_impl(config_page::state& s, const ImVec2& panel_min,
        const ImVec2& panel_max, bool& blocks_drag)
    {
        // Closing runs faster than opening: a modal that lingers on the way out
        // reads as lag, while the same speed on the way in reads as snappy.
        const float rate = s.share.open ? 16.f : 26.f;
        s.share.alpha = ImLerp(s.share.alpha, s.share.open ? 1.f : 0.f,
            ImMin(1.f, ImGui::GetIO().DeltaTime * rate));

        if (s.share.alpha < 0.01f && !s.share.open)
            return;

        // The dim + blur cover only the menu shell, not the whole display, so
        // the game behind the panel stays untouched.
        const ImRect panel(panel_min, panel_max);
        blocks_drag = true;

        if (ImGui::IsKeyPressed(ImGuiKey_Escape))
            s.share.open = false;

        // Window draw list, not foreground: InputText renders its text into the
        // window list, so a foreground-drawn modal would paint over its own
        // field. The overlay runs after all page content, so submission order
        // already layers it on top. The host child is clipped to the content
        // area, while the dim has to reach the sidebar and topbar too — hence
        // the widened clip rect around everything the overlay draws.
        //
        // Fading rides the style alpha, which GetColorU32 folds into every
        // element including the live blur. One shared curve looked wrong on the
        // way out though: an opaque card at half alpha still reads as a card,
        // while a half-alpha blur already reads as a sharp background. So the
        // card runs ahead of the backdrop and the frost is the last thing left.
        ImDrawList* dl = ImGui::GetWindowDrawList();
        dl->PushClipRect(panel.Min, panel.Max, false);

        ImGui::PushStyleVar(ImGuiStyleVar_Alpha, ImSqrt(s.share.alpha));
        const float shell_round = ui_scale::s(layout::shell_round);
        blur_renderer::draw_live_blur(dl, panel.Min, panel.Max, shell_round);
        dl->AddRectFilled(panel.Min, panel.Max,
            ImGui::GetColorU32(ImVec4(0.04f, 0.05f, 0.07f, 0.62f)),
            shell_round);
        ImGui::PopStyleVar();

        ImGui::PushStyleVar(ImGuiStyleVar_Alpha, s.share.alpha * s.share.alpha);

        // Compact card: one code field plus a button row, nothing else.
        const float pad = ui_scale::s(16.f);
        const float field_h = ui_scale::s(layout::config_search_h);
        const float btn_h = ui_scale::s(30.f);
        const float btn_gap = ui_scale::s(8.f);
        const float status_h = ui_scale::s(22.f);
        const float modal_w = ImMin(ui_scale::s(430.f), panel.GetWidth() - ui_scale::s(48.f));
        const float modal_h = pad + field_h + ui_scale::s(10.f) + btn_h + status_h + pad;

        const ImVec2 modal_min = {
            panel.GetCenter().x - modal_w * 0.5f,
            panel.GetCenter().y - modal_h * 0.5f
        };
        const ImRect modal(modal_min, { modal_min.x + modal_w, modal_min.y + modal_h });
        const float round = ui_scale::s(layout::config_card_round);

        // No InvisibleButton here: ImGui gives hover to the first item
        // submitted at a position, so a panel-wide button would starve the
        // modal's own controls. The page beneath is disabled by the caller,
        // and outside-click close only needs the raw IO state.
        if (ImGui::IsMouseClicked(0) && s.share.alpha > 0.9f
            && panel.Contains(ImGui::GetIO().MousePos)
            && !modal.Contains(ImGui::GetIO().MousePos))
            s.share.open = false;

        dl->AddRectFilled(modal.Min, modal.Max, ImGui::GetColorU32(colors::dropdown_bg), round);
        dl->AddRect(modal.Min, modal.Max,
            ImGui::GetColorU32(ImVec4(colors::separator.x, colors::separator.y,
                colors::separator.z, 0.55f)),
            round, 0, ui_scale::s(1.f));

        ImFont* body_font = fonts::inter(15.f);
        ImFont* code_font = fonts::inter(16.f);
        if (!code_font)
            code_font = body_font;

        // Single field, both directions: it opens holding this config's code,
        // and pasting a friend's code over it + Import loads theirs.
        const ImRect field({
            modal.Min.x + pad,
            modal.Min.y + pad
        }, {
            modal.Max.x - pad,
            modal.Min.y + pad + field_h
        });

        if (ImGui::IsMouseHoveringRect(field.Min, field.Max, false))
            blocks_drag = true;

        const float control_round = ui_scale::s(layout::control_round);
        dl->AddRectFilled(field.Min, field.Max, ImGui::GetColorU32(colors::control), control_round);
        dl->AddRect(field.Min, field.Max, ImGui::GetColorU32(colors::separator), control_round, 0, 1.f);

        if (s.share.code[0] == '\0' && body_font)
        {
            const float sz = fonts::size(body_font);
            dl->AddText(body_font, sz,
                { field.Min.x + ui_scale::s(12.f), field.GetCenter().y - sz * 0.5f },
                ImGui::GetColorU32(colors::text_muted), "Paste share code...");
        }

        ImGui::SetCursorScreenPos({ field.Min.x + ui_scale::s(12.f),
            field.GetCenter().y - fonts::size(code_font) * 0.5f - ui_scale::s(2.f) });
        ImGui::PushItemWidth(field.GetWidth() - ui_scale::s(24.f));
        ImGui::PushStyleVar(ImGuiStyleVar_FramePadding, { 0.f, 2.f });
        ImGui::PushStyleColor(ImGuiCol_FrameBg, { 0.f, 0.f, 0.f, 0.f });
        ImGui::PushStyleColor(ImGuiCol_FrameBgHovered, { 0.f, 0.f, 0.f, 0.f });
        ImGui::PushStyleColor(ImGuiCol_FrameBgActive, { 0.f, 0.f, 0.f, 0.f });
        ImGui::PushStyleColor(ImGuiCol_Text, colors::text);
        if (code_font)
            ImGui::PushFont(code_font);
        ImGui::InputText("##share_code", s.share.code, sizeof(s.share.code),
            ImGuiInputTextFlags_CharsNoBlank | ImGuiInputTextFlags_CallbackCharFilter,
            share_code_filter);
        const bool field_active = ImGui::IsItemActive();
        if (code_font)
            ImGui::PopFont();
        ImGui::PopStyleColor(4);
        ImGui::PopStyleVar();
        ImGui::PopItemWidth();
        if (field_active)
            blocks_drag = true;

        const float btn_y = field.Max.y + ui_scale::s(10.f);
        const float btn_w = (modal_w - pad * 2.f - btn_gap * 2.f) / 3.f;
        const ImRect copy_bb({ modal.Min.x + pad, btn_y }, { modal.Min.x + pad + btn_w, btn_y + btn_h });
        const ImRect import_bb({ copy_bb.Max.x + btn_gap, btn_y }, { copy_bb.Max.x + btn_gap + btn_w, btn_y + btn_h });
        const ImRect close_bb({ import_bb.Max.x + btn_gap, btn_y }, { modal.Max.x - pad, btn_y + btn_h });

        auto draw_modal_button = [&](const ImRect& rect, const char* label, const char* icon, bool accent = false) -> bool
        {
            ImGuiID id = ImGui::GetID(label);
            ImGui::ItemAdd(rect, id);
            bool hovered = false, held = false;
            const bool pressed = ImGui::ButtonBehavior(rect, id, &hovered, &held);
            if (hovered || held)
                blocks_drag = true;

            ImVec4 bg = accent ? colors::accent : colors::control;
            bg = ImLerp(bg, colors::control_hover, hovered || held ? 0.9f : 0.0f);
            ImVec4 txt = accent ? colors::text : colors::fg(0.82f);

            dl->AddRectFilled(rect.Min, rect.Max, ImGui::GetColorU32(bg), control_round);
            const float icon_side = ui_scale::s(14.f);
            const float gap = ui_scale::s(7.f);
            const float text_size = body_font ? fonts::size(body_font) : 0.f;
            const float label_w = body_font ? body_font->CalcTextSizeA(text_size, FLT_MAX, 0.f, label).x : 0.f;
            const float group_w = icon_side + gap + label_w;
            const float gx = rect.Min.x + (rect.GetWidth() - group_w) * 0.5f;
            icons::draw(dl, icon, {
                { gx, rect.GetCenter().y - icon_side * 0.5f },
                { gx + icon_side, rect.GetCenter().y + icon_side * 0.5f }
            }, ImGui::GetColorU32(txt));
            if (body_font)
                dl->AddText(body_font, text_size,
                    { gx + icon_side + gap, rect.GetCenter().y - text_size * 0.5f },
                    ImGui::GetColorU32(txt), label);
            return pressed;
        };

        if (draw_modal_button(copy_bb, "Copy", "copy", true))
        {
            ImGui::SetClipboardText(s.share.code);
            set_share_status(s, "Share code copied");
        }
        if (draw_modal_button(import_bb, "Import", "cloud-download", true))
        {
            char message[96] = {};
            config_page::import_share(s, s.share.code, message, sizeof(message));
            set_share_status(s, message);
        }
        if (draw_modal_button(close_bb, "Close", "x"))
            s.share.open = false;

        if (body_font)
        {
            const float sz = fonts::size(body_font);
            const float tw = body_font->CalcTextSizeA(sz, FLT_MAX, 0.f, s.share.status).x;
            dl->AddText(body_font, sz,
                { modal.GetCenter().x - tw * 0.5f, close_bb.Max.y + ui_scale::s(6.f) },
                ImGui::GetColorU32(colors::fg(0.62f)), s.share.status);
        }

        dl->PopClipRect();
        ImGui::PopStyleVar();
    }
}

namespace config_page
{
    bool sync_from_cloud(state& s)
    {
        const std::string fallback = config_store::serialize();
        return start_cloud_job(s, "Loading cloud configs...", [fallback] {
            cloud_job_result r;
            r.kind = cloud_job_kind::sync;

            std::vector<api::named_config_meta> cloud;
            if (!api::list_named_configs(cloud, r.error))
                return r;
            if (cloud.empty())
            {
                config_store::migrate_legacy();
                if (!api::list_named_configs(cloud, r.error))
                    return r;
            }

            for (const auto& item : cloud)
                r.entries.push_back({ item.name, item.updated_ts });

            if (r.entries.empty())
            {
                long long updated = 0;
                if (!api::put_named_config("Default", fallback, updated, r.error))
                    return r;
                r.entries.push_back({ "Default", updated });
                r.content = fallback;
                r.ok = true;
                return r;
            }

            r.name = r.entries[0].name;
            long long updated = 0;
            r.ok = api::get_named_config(r.name, r.content, updated, r.error);
            return r;
        });
    }

    void poll(state& s)
    {
        if (!g_cloud_busy || !g_cloud_job.valid()
            || g_cloud_job.wait_for(std::chrono::milliseconds(0)) != std::future_status::ready)
            return;

        cloud_job_result r = g_cloud_job.get();
        g_cloud_busy = false;

        if (!r.ok)
        {
            set_status(s, "Cloud request failed");
            if (r.kind == cloud_job_kind::share || r.kind == cloud_job_kind::import_fetch)
                set_share_status(s, "Could not reach the cloud server");
            return;
        }

        switch (r.kind)
        {
        case cloud_job_kind::sync:
            fill_config_list(s, r.entries);
            if (s.count > 0 && config_store::deserialize(r.content.c_str()))
            {
                s.selected = 0;
                copy_str(s.active, sizeof(s.active), s.names[0]);
                config_store::sync(s.active);
                set_status(s, "Cloud configs loaded");
            }
            else
                set_status(s, "Could not apply cloud config");
            break;

        case cloud_job_kind::save:
            set_status(s, "Config saved to cloud");
            break;

        case cloud_job_kind::load:
            if (config_store::deserialize(r.content.c_str()))
            {
                copy_str(s.active, sizeof(s.active), r.name.c_str());
                config_store::sync(s.active);
                set_status(s, "Config loaded");
            }
            else
                set_status(s, "Could not apply cloud config");
            break;

        case cloud_job_kind::create:
            add_config(s, r.name);
            set_status(s, "Config created in cloud");
            break;

        case cloud_job_kind::duplicate:
            add_config(s, r.name);
            set_status(s, "Config duplicated in cloud");
            break;

        case cloud_job_kind::remove:
        {
            int index = -1;
            for (int i = 0; i < s.count; ++i)
                if (r.name == s.names[i]) { index = i; break; }
            if (index >= 0)
            {
                const bool deleted_active = r.name == s.active;
                for (int i = index; i < s.count - 1; ++i)
                {
                    copy_str(s.names[i], sizeof(s.names[i]), s.names[i + 1]);
                    copy_str(s.dates[i], sizeof(s.dates[i]), s.dates[i + 1]);
                }
                --s.count;
                if (s.selected >= s.count)
                    s.selected = s.count - 1;
                g_rename_index = -1;
                if (deleted_active && s.count > 0)
                {
                    copy_str(s.active, sizeof(s.active), s.names[s.selected]);
                    config_store::sync(s.active);
                }
            }
            set_status(s, "Config deleted from cloud");
            break;
        }

        case cloud_job_kind::rename:
            for (int i = 0; i < s.count; ++i)
            {
                if (r.old_name != s.names[i])
                    continue;
                if (r.old_name == s.active)
                    copy_str(s.active, sizeof(s.active), r.name.c_str());
                copy_str(s.names[i], sizeof(s.names[i]), r.name.c_str());
                break;
            }
            config_store::sync(s.active);
            set_status(s, "Config renamed in cloud");
            break;

        case cloud_job_kind::share:
            copy_str(s.share.code, sizeof(s.share.code), r.code.c_str());
            set_share_status(s, "Share code ready");
            set_status(s, "Share code ready");
            break;

        case cloud_job_kind::import_fetch:
        {
            const std::string name = import_name(s, r.name);
            if (name.empty())
            {
                set_share_status(s, "Could not choose a config name");
                break;
            }
            const std::string content = r.content;
            start_cloud_job(s, "Saving imported config...", [name, content] {
                cloud_job_result out;
                out.kind = cloud_job_kind::import_commit;
                out.name = name;
                out.content = content;
                long long updated = 0;
                out.ok = api::put_named_config(name, content, updated, out.error);
                return out;
            });
            set_share_status(s, "Saving imported config...");
            break;
        }

        case cloud_job_kind::import_commit:
            if (config_store::deserialize(r.content.c_str()))
            {
                add_config(s, r.name);
                set_share_status(s, "Cloud config imported");
                set_status(s, "Cloud config imported");
            }
            else
                set_share_status(s, "Could not apply cloud config");
            break;
        }
    }

    void shutdown()
    {
        if (g_cloud_job.valid())
            g_cloud_job.wait();
        g_cloud_busy = false;
    }

    void save_current_async(state& s)
    {
        const std::string name = s.active;
        const std::string content = config_store::serialize();
        start_cloud_job(s, "Saving cloud config...", [name, content] {
            cloud_job_result r;
            r.kind = cloud_job_kind::save;
            r.name = name;
            r.content = content;
            long long updated = 0;
            r.ok = api::put_named_config(name, content, updated, r.error);
            return r;
        });
    }

    void set_save_result(state& s, bool ok, const char* name)
    {
        char msg[96];
        ImFormatString(msg, IM_ARRAYSIZE(msg), ok ? "Saved to cloud \"%s\"" : "Could not save cloud config \"%s\"", name);
        set_status(s, msg);
    }

    void share_current(state& s)
    {
        prepare_share_code(s);
        s.share.open = true;
    }

    bool import_share(state& s, const char* code, char* error, int error_size)
    {
        std::string normalized;
        for (unsigned char c : std::string(code ? code : ""))
            if (!std::isspace(c))
                normalized.push_back((char)std::tolower(c));

        if (normalized.empty())
        {
            copy_str(error, (std::size_t)ImMax(error_size, 0), "Paste a share code");
            return false;
        }
        if (s.count >= state::max_configs)
        {
            copy_str(error, (std::size_t)ImMax(error_size, 0), "Config list is full");
            return false;
        }

        const bool queued = start_cloud_job(s, "Importing cloud config...", [normalized] {
            cloud_job_result r;
            r.kind = cloud_job_kind::import_fetch;
            r.code = normalized;
            api::cloud_config_data cloud;
            r.ok = api::load_cloud_config(normalized, cloud, r.error);
            if (r.ok)
            {
                r.name = cloud.name;
                r.content = cloud.content;
            }
            return r;
        });
        copy_str(error, (std::size_t)ImMax(error_size, 0),
            queued ? "Importing cloud config..." : "Cloud request already in progress");
        return queued;
    }

    void draw(state& s, bool& blocks_window_drag)
    {
        s.status_alpha = ImLerp(s.status_alpha, 1.f, ImMin(1.f, ImGui::GetIO().DeltaTime * 10.f));

        ImDrawList* dl = ImGui::GetWindowDrawList();
        const ImVec2 content_pos = settings_panel::content_origin();
        const float left_w  = settings_panel::column_left_width();
        const float right_w = settings_panel::column_right_width();

        char header[48];
        ImFormatString(header, IM_ARRAYSIZE(header), "Profiles  %d", s.count);
        settings_panel::draw_header(dl, content_pos, header);

        const float header_h = settings_panel::header_block_h(header);
        const ImVec2 search_pos = { content_pos.x, content_pos.y + header_h };
        const ImRect search_bar(search_pos, search_pos + ImVec2(left_w, ui_scale::s(layout::config_search_h)));
        draw_search_field(dl, search_bar, s, blocks_window_drag);

        const ImVec2 cards_origin = { content_pos.x, search_bar.Max.y + ui_scale::s(10.f) };
        const float cards_bottom = draw_config_cards(dl, cards_origin, left_w, s, blocks_window_drag);

        const ImVec2 right_pos = { content_pos.x + left_w + ui_scale::s(layout::column_gap), content_pos.y };
        settings_panel::draw_header(dl, right_pos, "Manage");
        const ImVec2 actions_pos = { right_pos.x, right_pos.y + settings_panel::header_block_h("Manage") };
        const ImRect actions_box(actions_pos, actions_pos + ImVec2(right_w, ui_scale::s(manage_box_height())));
        dl->AddRectFilled(actions_box.Min, actions_box.Max, ImGui::GetColorU32(colors::box), ui_scale::s(layout::box_round));

        draw_profile_summary(dl, actions_box, s, blocks_window_drag);
        float manage_bottom = actions_box.Min.y;
        draw_action_grid(dl, actions_box, s, blocks_window_drag, manage_bottom);

        if (ImFont* font = fonts::inter(16.f))
        {
            const ImVec2 status_pos = { content_pos.x, cards_bottom + ui_scale::s(10.f) };
            dl->AddText(font, fonts::size(font), status_pos,
                ImGui::GetColorU32(ImVec4(colors::text_muted.x, colors::text_muted.y, colors::text_muted.z, s.status_alpha)),
                s.status);
        }

        const float bottom = ImMax(cards_bottom + ui_scale::s(28.f), manage_bottom + ui_scale::s(layout::content_pad_y));
        settings_panel::finalize_scroll(bottom);
    }

    void draw_share_overlay(state& s, const ImVec2& panel_min, const ImVec2& panel_max,
        bool& blocks_window_drag)
    {
        draw_share_overlay_impl(s, panel_min, panel_max, blocks_window_drag);
    }

    bool share_open(const state& s)
    {
        return s.share.open || s.share.alpha > 0.01f;
    }
}
