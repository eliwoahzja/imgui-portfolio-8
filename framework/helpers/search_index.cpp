#include "search_index.h"

#include <cstring>
#include <vector>

namespace
{
    std::vector<search_index::entry> g_entries;
    int  g_page       = 0;
    char g_section[48] = "General";
    bool g_collecting  = true;

    char  g_highlight[48] = {};
    float g_highlight_time = 0.f;

    constexpr float k_highlight_duration = 2.4f;

    void copy_str(char* dst, int dst_size, const char* src)
    {
        if (!dst || dst_size <= 0)
            return;
        if (!src)
        {
            dst[0] = 0;
            return;
        }
        int i = 0;
        for (; src[i] && i < dst_size - 1; ++i)
            dst[i] = src[i];
        dst[i] = 0;
    }

    char lower(char c)
    {
        return (c >= 'A' && c <= 'Z') ? (char)(c - 'A' + 'a') : c;
    }

    bool contains_ci(const char* haystack, const char* needle)
    {
        if (!needle || !needle[0])
            return true;
        if (!haystack)
            return false;

        for (int i = 0; haystack[i]; ++i)
        {
            int j = 0;
            for (; needle[j]; ++j)
            {
                const char a = lower(haystack[i + j]);
                const char b = lower(needle[j]);
                if (!a || a != b)
                    break;
            }
            if (!needle[j])
                return true;
        }
        return false;
    }

    // Entries whose label starts with the query rank above plain substring hits.
    bool starts_with_ci(const char* haystack, const char* needle)
    {
        if (!haystack || !needle)
            return false;
        for (int i = 0; needle[i]; ++i)
        {
            if (lower(haystack[i]) != lower(needle[i]))
                return false;
        }
        return true;
    }
}

namespace search_index
{
    void set_page(int page)
    {
        g_page = page;
    }

    void set_section(const char* name)
    {
        copy_str(g_section, (int)sizeof(g_section), name && name[0] ? name : "General");
    }

    void note(const char* id, const char* label)
    {
        if (!g_collecting || !id || !id[0] || !label || !label[0])
            return;

        for (const entry& e : g_entries)
        {
            if (std::strcmp(e.id, id) == 0)
                return;
        }

        entry e;
        copy_str(e.id, (int)sizeof(e.id), id);
        copy_str(e.label, (int)sizeof(e.label), label);
        copy_str(e.section, (int)sizeof(e.section), g_section);
        e.page = g_page;
        g_entries.push_back(e);
    }

    void set_collecting(bool on)
    {
        g_collecting = on;
    }

    bool collecting()
    {
        return g_collecting;
    }

    int total()
    {
        return (int)g_entries.size();
    }

    const entry* at(int index)
    {
        if (index < 0 || index >= (int)g_entries.size())
            return nullptr;
        return &g_entries[(size_t)index];
    }

    int find(const char* text, const entry** out, int max_out)
    {
        if (!out || max_out <= 0)
            return 0;

        int count = 0;

        // Prefix matches first, then the rest of the substring matches.
        for (int pass = 0; pass < 2 && count < max_out; ++pass)
        {
            for (const entry& e : g_entries)
            {
                if (count >= max_out)
                    break;

                const bool prefix = starts_with_ci(e.label, text);
                if (pass == 0 && !prefix)
                    continue;
                if (pass == 1 && (prefix || !(contains_ci(e.label, text) || contains_ci(e.section, text))))
                    continue;

                out[count++] = &e;
            }
        }
        return count;
    }

    void highlight(const char* id)
    {
        copy_str(g_highlight, (int)sizeof(g_highlight), id);
        g_highlight_time = g_highlight[0] ? k_highlight_duration : 0.f;
    }

    void tick(float dt)
    {
        if (g_highlight_time <= 0.f)
            return;

        g_highlight_time -= dt;
        if (g_highlight_time <= 0.f)
        {
            g_highlight_time = 0.f;
            g_highlight[0] = 0;
        }
    }

    float highlight_alpha(const char* id)
    {
        if (g_highlight_time <= 0.f || !id || !id[0] || std::strcmp(g_highlight, id) != 0)
            return 0.f;

        // Fade out over the last second so the row settles down on its own.
        const float t = g_highlight_time / k_highlight_duration;
        return t > 0.75f ? 1.f : t / 0.75f;
    }
}
