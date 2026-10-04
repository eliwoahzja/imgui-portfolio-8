#pragma once

// Registry of every function row drawn by the widgets, used by the top bar
// search field. Rows register themselves while they are drawn, so the index
// needs no hand-maintained table: gui runs one hidden indexing pass over all
// pages at startup and the registry stays valid from there on.
namespace search_index
{
    struct entry
    {
        char id[48]      = {};
        char label[64]   = {};
        char section[48] = {};
        int  page        = 0;
    };

    // --- filled while drawing -------------------------------------------
    void set_page(int page);
    void set_section(const char* name);
    void note(const char* id, const char* label);

    // Rows only register while this is enabled; gui turns it off after the
    // indexing pass so later frames just do the (cheap) duplicate check.
    void set_collecting(bool on);
    bool collecting();

    // --- reading ---------------------------------------------------------
    int total();
    const entry* at(int index);

    // Substring match on label and section, case-insensitive.
    // Returns how many entries were written to out.
    int find(const char* text, const entry** out, int max_out);

    // --- highlight -------------------------------------------------------
    // Jumps to a row: the page switch is done by gui, the pulse lives here.
    void highlight(const char* id);
    void tick(float dt);
    // 0 when the row is not the highlighted one.
    float highlight_alpha(const char* id);
}
