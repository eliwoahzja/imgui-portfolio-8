#pragma once

#include "imgui.h"

namespace colors
{
    enum class theme
    {
        night,   // Figma default: dark glass over the wallpaper
        light,   // frosted white glass, dark ink
        count_
    };

    inline ImVec4 accent = { 0x61 / 255.f, 0x5D / 255.f, 0xCE / 255.f, 1.f };  // #615DCE from Figma

    // The palette is mutable so apply_theme() can rewrite it. Values below are the
    // Night defaults, which is what the app starts on.
    inline ImVec4 bg            = { 0.f, 0.f, 0.f, 0.5f };           // rgba(0,0,0,0.5)
    inline ImVec4 panel         = { 0.f, 0.f, 0.f, 0.5f };           // main window backdrop
    inline ImVec4 sidebar       = { 0.f, 0.f, 0.f, 0.4f };           // sidebar backdrop blur(2px)
    inline ImVec4 box           = { 0.f, 0.f, 0.f, 0.4f };           // content boxes
    inline ImVec4 control       = { 0.f, 0.f, 0.f, 0.5f };           // inputs/buttons
    inline ImVec4 control_hover = { 0.f, 0.f, 0.f, 0.6f };
    inline ImVec4 circle_checkbox_hover = { 0x86 / 255.f, 0x86 / 255.f, 0x86 / 255.f, 1.f };
    inline ImVec4 widget        = control;
    inline ImVec4 widget_hover  = control_hover;
    inline ImVec4 text          = { 1.f, 1.f, 1.f, 1.f };            // #FFFFFF
    inline ImVec4 text_muted    = { 1.f, 1.f, 1.f, 0.6f };           // rgba(255,255,255,0.6)
    inline ImVec4 header_text   = { 1.f, 1.f, 1.f, 0.26f };          // rgba(255,255,255,0.26)
    inline ImVec4 separator     = { 0.f, 0.f, 0.f, 0.25f };          // border separator
    inline ImVec4 sidebar_sep   = { 1.f, 1.f, 1.f, 0.17f };          // selected tab gradient start
    inline ImVec4 dropdown_bg   = { 0.f, 0.f, 0.f, 0.7f };           // dropdown/popup bg
    inline ImVec4 config_sel    = { 0x25 / 255.f, 0x25 / 255.f, 0x25 / 255.f, 0.5f };  // selected config
    inline ImVec4 config_hover  = { 0.f, 0.f, 0.f, 0.5f };
    inline ImVec4 success       = { 0.35f, 0.78f, 0.48f, 1.f };
    inline ImVec4 danger        = { 0xFF / 255.f, 0x70 / 255.f, 0x70 / 255.f, 1.f };  // #FF7070

    // Additional Figma palette
    inline ImVec4 slider_bg     = { 0x2B / 255.f, 0x2B / 255.f, 0x2B / 255.f, 1.f };  // #2B2B2B
    inline ImVec4 slider_fill   = { 0x86 / 255.f, 0x86 / 255.f, 0x86 / 255.f, 1.f };  // #868686
    inline ImVec4 toggle_off    = { 0x1A / 255.f, 0x1A / 255.f, 0x1A / 255.f, 1.f };  // #1A1A1A
    inline ImVec4 toggle_on     = { 0x86 / 255.f, 0x86 / 255.f, 0x86 / 255.f, 1.f };  // #868686
    inline ImVec4 knob_off      = { 0x6E / 255.f, 0x6E / 255.f, 0x6E / 255.f, 1.f };  // #6E6E6E

    // Foreground base for the many one-off text alphas the widgets use, and the
    // base for the faint hover washes painted over panels. Both flip with the
    // theme so a light panel gets dark ink and dark highlights.
    inline ImVec4 ink  = { 1.f, 1.f, 1.f, 1.f };
    inline ImVec4 haze = { 1.f, 1.f, 1.f, 1.f };

    inline ImVec4 fg(float alpha)   { return { ink.x,  ink.y,  ink.z,  alpha }; }
    inline ImVec4 wash(float alpha) { return { haze.x, haze.y, haze.z, alpha }; }

    inline theme active = theme::night;

    inline void apply_theme(theme t)
    {
        active = t;

        if (t == theme::light)
        {
            const float w = 1.f;
            bg            = { w, w, w, 0.55f };
            panel         = { w, w, w, 0.55f };
            sidebar       = { w, w, w, 0.45f };
            box           = { w, w, w, 0.45f };
            control       = { w, w, w, 0.58f };
            control_hover = { w, w, w, 0.72f };
            circle_checkbox_hover = { 0x6E / 255.f, 0x6E / 255.f, 0x6E / 255.f, 1.f };
            text          = { 0x14 / 255.f, 0x14 / 255.f, 0x1A / 255.f, 1.f };
            text_muted    = { 0.f, 0.f, 0.f, 0.58f };
            header_text   = { 0.f, 0.f, 0.f, 0.34f };
            separator     = { 0.f, 0.f, 0.f, 0.12f };
            sidebar_sep   = { 0.f, 0.f, 0.f, 0.14f };
            dropdown_bg   = { w, w, w, 0.86f };
            config_sel    = { w, w, w, 0.66f };
            config_hover  = { w, w, w, 0.5f };
            slider_bg     = { 0xD2 / 255.f, 0xD2 / 255.f, 0xD6 / 255.f, 1.f };
            slider_fill   = { 0x6E / 255.f, 0x6E / 255.f, 0x6E / 255.f, 1.f };
            toggle_off    = { 0xC7 / 255.f, 0xC7 / 255.f, 0xCC / 255.f, 1.f };
            toggle_on     = { 0x6E / 255.f, 0x6E / 255.f, 0x6E / 255.f, 1.f };
            knob_off      = { w, w, w, 1.f };
            ink           = { 0x14 / 255.f, 0x14 / 255.f, 0x1A / 255.f, 1.f };
            haze          = { 0.f, 0.f, 0.f, 1.f };
        }
        else
        {
            bg            = { 0.f, 0.f, 0.f, 0.5f };
            panel         = { 0.f, 0.f, 0.f, 0.5f };
            sidebar       = { 0.f, 0.f, 0.f, 0.4f };
            box           = { 0.f, 0.f, 0.f, 0.4f };
            control       = { 0.f, 0.f, 0.f, 0.5f };
            control_hover = { 0.f, 0.f, 0.f, 0.6f };
            circle_checkbox_hover = { 0x86 / 255.f, 0x86 / 255.f, 0x86 / 255.f, 1.f };
            text          = { 1.f, 1.f, 1.f, 1.f };
            text_muted    = { 1.f, 1.f, 1.f, 0.6f };
            header_text   = { 1.f, 1.f, 1.f, 0.26f };
            separator     = { 0.f, 0.f, 0.f, 0.25f };
            sidebar_sep   = { 1.f, 1.f, 1.f, 0.17f };
            dropdown_bg   = { 0.f, 0.f, 0.f, 0.7f };
            config_sel    = { 0x25 / 255.f, 0x25 / 255.f, 0x25 / 255.f, 0.5f };
            config_hover  = { 0.f, 0.f, 0.f, 0.5f };
            slider_bg     = { 0x2B / 255.f, 0x2B / 255.f, 0x2B / 255.f, 1.f };
            slider_fill   = { 0x86 / 255.f, 0x86 / 255.f, 0x86 / 255.f, 1.f };
            toggle_off    = { 0x1A / 255.f, 0x1A / 255.f, 0x1A / 255.f, 1.f };
            toggle_on     = { 0x86 / 255.f, 0x86 / 255.f, 0x86 / 255.f, 1.f };
            knob_off      = { 0x6E / 255.f, 0x6E / 255.f, 0x6E / 255.f, 1.f };
            ink           = { 1.f, 1.f, 1.f, 1.f };
            haze          = { 1.f, 1.f, 1.f, 1.f };
        }

        widget       = control;
        widget_hover = control_hover;
    }
}
