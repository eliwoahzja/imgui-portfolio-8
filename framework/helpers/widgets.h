#pragma once

#include "imgui.h"
#include "imgui_internal.h"

namespace widgets
{
    void draw_section_header(ImDrawList* dl, const ImVec2& pos, const char* title);

    ImRect draw_settings_box(ImDrawList* dl, const ImVec2& pos, float width, int rows);

    bool checkbox_row(const char* id, const char* label, bool* value, const ImRect& row, bool& blocks_drag);
    bool slider_row(const char* id, const char* label, float* value, float min, float max, const ImRect& row, bool& blocks_drag);
    bool combo_row(const char* id, const char* label, int* index, const char* const items[], int count, const ImRect& row, bool& blocks_drag);
    bool multicombo_row(const char* id, const char* label, unsigned* mask, const char* const items[], int count, const ImRect& row, bool& blocks_drag);
    bool keybind_row(const char* id, const char* label, int* key, const ImRect& row, bool& blocks_drag);
    bool color_row(const char* id, const char* label, ImVec4* value, const ImRect& row, bool& blocks_drag);
    bool button_row(const char* id, const char* label, const ImRect& row, bool& blocks_drag, ImVec4 color = ImVec4(0.f, 0.f, 0.f, 0.f));
    bool text_input_row(const char* id, const char* label, char* buf, int buf_size, const ImRect& row, bool& blocks_drag);

    // Right-click bind popup + on-screen keybind list. Call once per frame
    // after the pages so it draws over everything. `menu_open` gates the
    // "Always" entries in the list.
    void draw_bind_popup(bool& blocks_drag);
    void draw_keybind_list(bool menu_open);
}
