#include "fonts.h"

#include "data/embedded_fonts.h"
#include "theme/layout.h"
#include "imgui_freetype.h"
#include "imgui_impl_dx11.h"

#include <algorithm>
#include <cmath>
#include <set>
#include <vector>

namespace
{
    enum class kind { ui, icon, logo };

    struct font_entry
    {
        float   design = 0.f;
        bool    bold   = false;
        kind    type   = kind::ui;
        ImFont* font   = nullptr;
    };

    std::vector<font_entry> g_entries;
    float g_ui_scale    = 1.f;
    float g_built_scale = -1.f;
    bool  g_pending_rebuild = false;

    // Lucide ships 2000+ glyphs; baking all of them at every size would blow up
    // the atlas, so only the codepoints actually drawn get requested. A new one
    // schedules an atlas rebuild, which lands at the start of the next frame.
    std::set<ImWchar>    g_icon_codepoints;
    std::vector<ImWchar> g_icon_ranges;

    // Cyrillic ranges stop at U+00FF plus the Cyrillic blocks, so the typographic
    // punctuation the copy uses (em dash, curly quotes, ellipsis) would bake as
    // the fallback '?'. Same set for every UI size, so build it once.
    std::vector<ImWchar> g_ui_ranges;

    float pixel_size(float design)
    {
        const float px = roundf(design * g_ui_scale);
        return px < 1.f ? 1.f : px;
    }

    ImFontConfig base_cfg()
    {
        ImFontConfig cfg;
        cfg.FontDataOwnedByAtlas = false;
        cfg.PixelSnapH           = true;
        cfg.OversampleH          = 2;
        cfg.OversampleV          = 1;
        cfg.FontBuilderFlags     = ImGuiFreeTypeBuilderFlags_LightHinting;
        return cfg;
    }

    void build_icon_ranges()
    {
        g_icon_ranges.clear();
        for (ImWchar cp : g_icon_codepoints)
        {
            // The set is ordered, so adjacent codepoints extend the last range.
            if (!g_icon_ranges.empty() && (ImWchar)(g_icon_ranges.back() + 1) == cp)
            {
                g_icon_ranges.back() = cp;
                continue;
            }
            g_icon_ranges.push_back(cp);
            g_icon_ranges.push_back(cp);
        }
        g_icon_ranges.push_back(0);
    }

    void build_ui_ranges()
    {
        if (!g_ui_ranges.empty())
            return;

        ImFontGlyphRangesBuilder builder;
        builder.AddRanges(ImGui::GetIO().Fonts->GetGlyphRangesCyrillic());
        static const ImWchar punctuation[] = { 0x2010, 0x2027, 0 };  // dashes, quotes, ellipsis
        builder.AddRanges(punctuation);

        ImVector<ImWchar> out;
        builder.BuildRanges(&out);
        g_ui_ranges.assign(out.begin(), out.end());
    }

    void rebuild_atlas()
    {
        ImGuiIO& io = ImGui::GetIO();
        ImGui_ImplDX11_InvalidateDeviceObjects();
        io.Fonts->Clear();

        build_icon_ranges();
        build_ui_ranges();

        for (auto& e : g_entries)
            e.font = nullptr;

        for (auto& e : g_entries)
        {
            const float px = pixel_size(e.design);
            ImFontConfig cfg = base_cfg();
            if (e.bold)
                cfg.FontBuilderFlags |= ImGuiFreeTypeBuilderFlags_Bold;

            switch (e.type)
            {
            case kind::icon:
                // Nothing requested yet: the range would be an empty terminator.
                if (g_icon_ranges.size() <= 1)
                    break;
                cfg.GlyphOffset = { 0.f, 0.f };
                e.font = io.Fonts->AddFontFromMemoryTTF(
                    (void*)embedded_fonts::lucide, (int)embedded_fonts::lucide_size,
                    px, &cfg, g_icon_ranges.data());
                break;
            case kind::logo:
                e.font = io.Fonts->AddFontFromMemoryTTF(
                    (void*)embedded_fonts::zendots, (int)embedded_fonts::zendots_size,
                    px, &cfg,
                    io.Fonts->GetGlyphRangesDefault());
                break;
            case kind::ui:
            default:
                e.font = io.Fonts->AddFontFromMemoryTTF(
                    (void*)embedded_fonts::museo, (int)embedded_fonts::museo_size,
                    px, &cfg,
                    g_ui_ranges.data());
                break;
            }
        }

        for (const auto& e : g_entries)
        {
            if (e.type == kind::ui && !e.bold && e.design == 13.f && e.font)
            {
                io.FontDefault = e.font;
                break;
            }
        }

        io.Fonts->Build();
        ImGui_ImplDX11_CreateDeviceObjects();
        g_built_scale = g_ui_scale;
        g_pending_rebuild = false;
    }

    bool scale_step_changed(float new_scale)
    {
        if (g_built_scale < 0.f)
            return true;

        for (const auto& e : g_entries)
        {
            g_ui_scale = g_built_scale;
            const float old_px = pixel_size(e.design);
            g_ui_scale = new_scale;
            const float new_px = pixel_size(e.design);
            if (old_px != new_px)
                return true;
        }
        return false;
    }

    font_entry* find_entry(float design, bool bold, kind type)
    {
        for (auto& e : g_entries)
        {
            if (e.design == design && e.bold == bold && e.type == type)
                return &e;
        }
        return nullptr;
    }

    font_entry& ensure_entry(float design, bool bold, kind type)
    {
        if (font_entry* found = find_entry(design, bold, type))
            return *found;

        g_entries.push_back({ design, bold, type, nullptr });
        g_pending_rebuild = true;
        return g_entries.back();
    }
}

namespace fonts
{
    void init()
    {
        g_ui_scale    = 1.f;
        g_built_scale = -1.f;
        g_icon_codepoints.clear();
        g_entries = {
            { 10.f, false, kind::ui, nullptr },
            { 11.f, false, kind::ui, nullptr },
            { 11.f, true,  kind::ui, nullptr },
            { 12.f, true,  kind::ui, nullptr },
            { 12.f, false, kind::ui, nullptr },
            { 13.f, false, kind::ui, nullptr },
            { 13.f, true,  kind::ui, nullptr },
            { 14.f, false, kind::ui, nullptr },
            { 14.f, true,  kind::ui, nullptr },
        };
        rebuild_atlas();
    }

    void shutdown()
    {
        g_entries.clear();
        g_icon_codepoints.clear();
        g_icon_ranges.clear();
        g_ui_ranges.clear();
        g_built_scale = -1.f;
    }

    void sync_ui_scale(float factor)
    {
        if (factor < 0.01f)
            factor = 0.01f;

        g_ui_scale = factor;

        if (!g_pending_rebuild && !scale_step_changed(factor))
            return;

        rebuild_atlas();
    }

    float design_to_pixel(float design_size)
    {
        return pixel_size(design_size);
    }

    float size(ImFont* font)
    {
        return font ? font->FontSize : 13.f;
    }

    ImFont* inter(float design_size, bool bold)
    {
        return ensure_entry(design_size, bold, kind::ui).font;
    }

    ImFont* icon(float design_size)
    {
        return ensure_entry(design_size, false, kind::icon).font;
    }

    ImFont* logo(float design_size)
    {
        return ensure_entry(design_size, false, kind::logo).font;
    }

    void request_icon(unsigned short codepoint)
    {
        if (codepoint == 0)
            return;
        if (g_icon_codepoints.insert((ImWchar)codepoint).second)
            g_pending_rebuild = true;
    }
}
