#pragma once

#include "imgui.h"

namespace config_page
{
    struct state
    {
        static constexpr int max_configs = 10;
        static constexpr int name_len    = 48;
        static constexpr int date_len     = 24;

        struct share_state
        {
            // Codes are base32 over [a-z0-9], 16-24 chars. The buffer is sized
            // to that and the field filters anything else out, so nothing but a
            // code can ever be typed, pasted, or sent to the server.
            static constexpr int code_len = 25;

            bool open = false;
            float alpha = 0.f;
            // One field, both directions: opens holding this config's code, and
            // pasting a friend's code over it + Import loads theirs instead.
            char code[code_len] = {};
            char status[96] = "Ready";
        } share;

        char names[max_configs][name_len] = { "Default", "Legit", "Rage" };
        char dates[max_configs][date_len] = { "2 days ago", "Yesterday", "Just now" };

        int  count    = 3;
        int  selected = 0;

        // The config the page's values actually came from, which is not always
        // the selected one: selecting a card only moves the cursor, Load applies
        // it. Autosave writes here so browsing the list cannot overwrite a
        // config the user never opened.
        char active[name_len] = "Default";

        char new_name[name_len] = {};
        char search[48] = {};
        char status[96] = "Ready";
        float status_alpha = 1.f;
    };

    void draw(state& s, bool& blocks_window_drag);
    bool sync_from_cloud(state& s);
    void poll(state& s);
    void shutdown();
    void save_current_async(state& s);

    void share_current(state& s);
    bool import_share(state& s, const char* code, char* error, int error_size);
    void draw_share_overlay(state& s, const ImVec2& panel_min, const ImVec2& panel_max,
        bool& blocks_window_drag);
    bool share_open(const state& s);

    // Status line feedback for saves triggered outside the page (the top bar
    // button, autosave).
    void set_save_result(state& s, bool ok, const char* name);
}
