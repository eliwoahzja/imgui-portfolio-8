#pragma once

namespace layout
{
    inline constexpr float window_w    = 1160.f;
    inline constexpr float window_h    = 669.f;
    inline constexpr float shell_round = 14.f;

    inline constexpr float sidebar_w           = 243.f;
    inline constexpr float sidebar_pad         = 24.f;
    inline constexpr float sidebar_tab_h       = 51.f;
    inline constexpr float sidebar_tab_w       = 241.f;
    inline constexpr float sidebar_tab_icon    = 24.f;
    inline constexpr float sidebar_tab_round   = 0.f;
    inline constexpr float sidebar_logo_size   = 60.f;
    inline constexpr float sidebar_logo_gap    = 16.f;
    inline constexpr float sidebar_tabs_gap    = 0.f;

    inline constexpr float sidebar_logo_x      = 8.f;
    inline constexpr float sidebar_logo_y      = 22.f;
    inline constexpr float sidebar_logo_font   = 36.f;

    // Tabs begin level with the top of the first content box.
    inline constexpr float sidebar_tabs_y      = 87.f;
    inline constexpr float sidebar_tab_icon_x  = 48.f;
    inline constexpr float sidebar_tab_text_gap = 10.f;

    inline constexpr float sidebar_exit_h        = 50.f;
    inline constexpr float sidebar_exit_round    = 0.f;
    inline constexpr float sidebar_bottom_pad    = 16.f;

    // User profile area (Figma: 60x60 avatar at x=35.5, 36.5px above the bottom)
    inline constexpr float user_avatar_size      = 60.f;
    inline constexpr float user_avatar_x         = 35.f;
    inline constexpr float user_bottom_margin    = 36.f;
    inline constexpr float user_area_pad         = 24.f;
    inline constexpr float user_text_gap         = 12.f;
    inline constexpr float user_line_gap         = 4.f;
    inline constexpr float user_name_font        = 16.f;

    // Top bar of the content area (Figma: Save button + search field + gear)
    inline constexpr float topbar_h              = 88.f;
    inline constexpr float topbar_row_y          = 21.f;
    inline constexpr float topbar_row_h          = 49.f;
    inline constexpr float topbar_save_w         = 138.f;
    inline constexpr float topbar_search_w       = 434.f;
    inline constexpr float topbar_logo_w         = 240.f;
    inline constexpr float topbar_icon_size      = 24.f;
    inline constexpr float topbar_icon_gap       = 16.f;
    inline constexpr float topbar_gear_margin    = 30.f;

    inline constexpr float content_gap       = 0.f;
    inline constexpr float content_pad_x     = 19.f;
    inline constexpr float content_pad_y     = 0.f;
    inline constexpr float scroll_bottom_pad = 52.f;

    inline constexpr float column_w          = 434.f;
    inline constexpr float column_gap        = 16.f;
    inline constexpr float content_margin_right = 14.f;

    inline constexpr float section_header_h  = 12.f;
    inline constexpr float section_header_gap= 8.f;
    // Figma row labels measure ~20px on the 2560x1440 design board.
    inline constexpr float row_label_font    = 20.f;
    inline constexpr float box_pad_y         = 8.f;
    inline constexpr float box_pad_x         = 14.f;
    inline constexpr float row_label_y_nudge = -3.f;
    inline constexpr float box_round         = 6.f;
    // Figma row pitch is 48px: 37 + 5*2 + 1 = 48
    inline constexpr float settings_row_h    = 37.f;
    inline constexpr float separator_h       = 1.f;
    inline constexpr float row_separator_gap = 5.f;

    inline constexpr float control_h           = 24.f;
    // Classic Portfolio #8 switch: pill track, accent knob riding inside it.
    inline constexpr float toggle_w            = 42.f;
    inline constexpr float toggle_h            = 22.f;
    inline constexpr float toggle_round        = toggle_h * 0.5f;
    inline constexpr float toggle_knob_r       = 8.f;
    inline constexpr float toggle_knob_travel  = 20.f;
    inline constexpr float toggle_knob_inset   = 11.f;   // knob centre offset from track edge
    inline constexpr float control_round       = 4.f;
    // Classic slider: thin resting track, thicker accent fill, value on the left.
    inline constexpr float slider_h          = 6.f;
    inline constexpr float slider_track_h    = 2.f;
    inline constexpr float slider_value_w    = 55.f;
    inline constexpr float slider_w          = 143.f;
    inline constexpr float compact_slider_w  = 87.f;
    inline constexpr float slider_knob_r     = 7.f;
    inline constexpr float combo_h           = control_h;
    inline constexpr float combo_w           = 157.f;
    inline constexpr float compact_combo_w   = 106.f;
    inline constexpr float combo_round       = control_round;
    inline constexpr float keybind_w         = combo_w;
    inline constexpr float compact_keybind_w = compact_combo_w;
    inline constexpr float keybind_icon_size = 13.f;
    inline constexpr float keybind_icon_pad  = 8.f;
    inline constexpr float dropdown_item_h   = 28.f;
    inline constexpr float color_swatch_size = control_h;
    inline constexpr float button_h          = control_h;
    inline constexpr float config_list_h     = 228.f;
    inline constexpr float config_item_h     = 70.f;
    inline constexpr float config_item_gap   = 9.f;
    inline constexpr float config_search_h   = 46.f;
    inline constexpr float config_card_round = 14.f;
    inline constexpr float config_card_w       = 407.f;
    inline constexpr float config_button_w     = 199.f;
    inline constexpr float config_button_h     = 49.f;
    inline constexpr float config_name_w       = 271.f;
    inline constexpr float config_name_h       = 36.f;
    inline constexpr float config_create_w     = 123.f;
    inline constexpr float config_create_h     = 36.f;

    // Menu settings popup (Figma: 360x239, rgba(0,0,0,0.7), blur 12.5, r14)
    inline constexpr float settings_popup_w      = 360.f;
    // Figma height was 239 for five rows; the Theme row adds one more at the same
    // row pitch ((239 - 2*pad) / 5 = 41.4).
    inline constexpr float settings_popup_h      = 281.f;
    inline constexpr float settings_popup_round  = 14.f;
    inline constexpr float settings_popup_pad    = 16.f;

    // Bind system popup (Figma: 218x225)
    inline constexpr float bind_popup_w       = 218.f;
    inline constexpr float bind_popup_h       = 225.f;
    inline constexpr float bind_popup_round   = 14.f;
    inline constexpr float bind_field_w       = 188.f;
    inline constexpr float bind_field_h       = 36.f;
    inline constexpr float bind_seg_w         = 89.f;
    inline constexpr float bind_seg_h         = 34.f;

    // Colorpicker popup (Figma: 205x247)
    inline constexpr float picker_w           = 205.f;
    inline constexpr float picker_h           = 247.f;
    inline constexpr float picker_round       = 14.f;
    inline constexpr float picker_sv_size     = 190.f;
    inline constexpr float picker_hue_h       = 5.f;
    inline constexpr float picker_alpha_h     = 4.f;

    inline constexpr const char* main_window_id = "##imgui_portfolio_8";
    inline constexpr const char* content_child_id = "##p8_content_scroll";

    inline float row_pitch()
    {
        return settings_row_h + row_separator_gap * 2.f + separator_h;
    }

    inline float settings_box_height(int rows)
    {
        if (rows <= 0)
            return box_pad_y * 2.f;
        return box_pad_y * 2.f
             + (float)rows * settings_row_h
             + (float)(rows - 1) * (row_separator_gap * 2.f + separator_h);
    }

    inline float content_x()
    {
        return sidebar_w + content_gap;
    }
}
