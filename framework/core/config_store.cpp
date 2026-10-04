#include "config_store.h"
#include "net/api.h"
#include "pages/aim_page.h"
#include "pages/settings_page.h"
#include "pages/visuals_page.h"

#include "imgui.h"

#include <cstdio>
#include <condition_variable>
#include <cstdlib>
#include <cstring>
#include <iterator>
#include <mutex>
#include <thread>
#include <vector>

#include <windows.h>

namespace
{
    // Field table: one entry per persisted value. Keeping the mapping in data
    // rather than in two hand-written functions means a new setting is one line
    // and cannot be saved-but-not-loaded.
    enum class kind { boolean, real, integer, uinteger, color };

    struct field
    {
        const char* key;
        kind        type;
        // Offset into the state the field belongs to, resolved per section.
        std::size_t offset;
    };

#define F(struct_, member_, kind_) { #member_, kind_, offsetof(struct_, member_) }

    const field k_aim[] = {
        F(aim_page::state, enabled,        kind::boolean),
        F(aim_page::state, smoothing,      kind::real),
        F(aim_page::state, aimbone,        kind::integer),
        F(aim_page::state, keybind,        kind::integer),
        F(aim_page::state, visible_only,   kind::boolean),
        F(aim_page::state, silent_aim,     kind::boolean),
        F(aim_page::state, auto_shoot,     kind::boolean),
        F(aim_page::state, ignore_team,    kind::boolean),
        F(aim_page::state, prediction,     kind::boolean),
        F(aim_page::state, ignore_knocked, kind::boolean),
        F(aim_page::state, auto_scope,     kind::boolean),
        F(aim_page::state, fov,            kind::real),
        F(aim_page::state, hitchance,      kind::real),
        F(aim_page::state, max_distance,   kind::real),
        F(aim_page::state, recoil_control, kind::real),
        F(aim_page::state, target_priority,kind::integer),
        F(aim_page::state, force_body,     kind::integer),
    };

    const field k_visuals[] = {
        F(visuals_page::state, esp_enabled,   kind::boolean),
        F(visuals_page::state, box_esp,       kind::boolean),
        F(visuals_page::state, corner_box,    kind::boolean),
        F(visuals_page::state, name_esp,      kind::boolean),
        F(visuals_page::state, health_bar,    kind::boolean),
        F(visuals_page::state, skeleton,      kind::boolean),
        F(visuals_page::state, head_dot,      kind::boolean),
        F(visuals_page::state, tracers,       kind::boolean),
        F(visuals_page::state, team_check,    kind::boolean),
        F(visuals_page::state, visible_only,  kind::boolean),
        F(visuals_page::state, max_distance,  kind::real),
        F(visuals_page::state, box_style,     kind::integer),
        F(visuals_page::state, tracer_origin, kind::integer),
        F(visuals_page::state, esp_flags,     kind::uinteger),
        F(visuals_page::state, chams_enabled, kind::boolean),
        F(visuals_page::state, chams_visible, kind::boolean),
        F(visuals_page::state, chams_occluded,kind::boolean),
        F(visuals_page::state, chams_wireframe, kind::boolean),
        F(visuals_page::state, chams_material,kind::integer),
        F(visuals_page::state, chams_opacity, kind::real),
        F(visuals_page::state, crosshair,     kind::boolean),
        F(visuals_page::state, fov_circle,    kind::boolean),
        F(visuals_page::state, hitmarker,     kind::boolean),
        F(visuals_page::state, watermark,     kind::boolean),
        F(visuals_page::state, streamproof,   kind::boolean),
        F(visuals_page::state, fov_radius,    kind::real),
    };

    // ui_scale and dpi_aware are deliberately absent: they belong to the
    // machine, not the config, and importing someone else's would resize the
    // menu out from under the user.
    const field k_settings[] = {
        F(settings_page::state, accent_color,    kind::color),
        F(settings_page::state, menu_opacity,    kind::real),
        F(settings_page::state, animation_speed, kind::real),
        F(settings_page::state, corner_rounding, kind::real),
        F(settings_page::state, theme_preset,    kind::integer),
        F(settings_page::state, language,        kind::integer),
        F(settings_page::state, show_watermark,  kind::boolean),
        F(settings_page::state, show_keybinds,   kind::boolean),
        F(settings_page::state, streamproof,     kind::boolean),
        F(settings_page::state, discord_rpc,     kind::boolean),
        F(settings_page::state, menu_key,        kind::integer),
    };

#undef F

    struct section
    {
        const char*  name;
        const field* fields;
        int          count;
        void*        base;
    };

    config_store::bundle g_bundle;

    void collect(section (&out)[3], int& out_count)
    {
        out_count = 0;
        if (g_bundle.aim)
            out[out_count++] = { "aim", k_aim, (int)std::size(k_aim), g_bundle.aim };
        if (g_bundle.visuals)
            out[out_count++] = { "visuals", k_visuals, (int)std::size(k_visuals), g_bundle.visuals };
        if (g_bundle.settings)
            out[out_count++] = { "settings", k_settings, (int)std::size(k_settings), g_bundle.settings };
    }

    void append_value(std::string& out, const section& sec, const field& f)
    {
        char buf[128];
        void* p = (char*)sec.base + f.offset;

        switch (f.type)
        {
        case kind::boolean:
            out += *(bool*)p ? "1" : "0";
            return;
        case kind::real:
            std::snprintf(buf, sizeof(buf), "%.4f", *(float*)p);
            break;
        case kind::integer:
            std::snprintf(buf, sizeof(buf), "%d", *(int*)p);
            break;
        case kind::uinteger:
            std::snprintf(buf, sizeof(buf), "%u", *(unsigned*)p);
            break;
        case kind::color:
        {
            const ImVec4& c = *(ImVec4*)p;
            std::snprintf(buf, sizeof(buf), "%.4f,%.4f,%.4f,%.4f", c.x, c.y, c.z, c.w);
            break;
        }
        }
        out += buf;
    }

    void parse_value(const section& sec, const field& f, const char* value)
    {
        void* p = (char*)sec.base + f.offset;

        switch (f.type)
        {
        case kind::boolean:
            *(bool*)p = value[0] == '1';
            return;
        case kind::real:
            *(float*)p = (float)std::atof(value);
            return;
        case kind::integer:
            *(int*)p = std::atoi(value);
            return;
        case kind::uinteger:
            *(unsigned*)p = (unsigned)std::strtoul(value, nullptr, 10);
            return;
        case kind::color:
        {
            ImVec4& c = *(ImVec4*)p;
            float v[4] = { c.x, c.y, c.z, c.w };
            const char* cursor = value;
            for (int i = 0; i < 4 && cursor && *cursor; ++i)
            {
                v[i] = (float)std::atof(cursor);
                cursor = std::strchr(cursor, ',');
                if (cursor)
                    ++cursor;
            }
            c = { v[0], v[1], v[2], v[3] };
            return;
        }
        }
    }

    // --- paths ------------------------------------------------------------

    // Config names come from a user-editable field, so a name could otherwise
    // walk out of the configs folder. Only the safe set survives; anything else
    // becomes '_', and an empty result is rejected by the callers.
    std::string sanitize(const char* name)
    {
        std::string out;
        for (const char* c = name; c && *c && out.size() < 48; ++c)
        {
            const bool ok = (*c >= 'a' && *c <= 'z') || (*c >= 'A' && *c <= 'Z')
                || (*c >= '0' && *c <= '9') || *c == ' ' || *c == '-' || *c == '_';
            out += ok ? *c : '_';
        }

        while (!out.empty() && out.back() == ' ')
            out.pop_back();

        return out;
    }

    std::string configs_dir()
    {
        char exe[MAX_PATH]{};
        if (GetModuleFileNameA(nullptr, exe, MAX_PATH) == 0)
            return {};

        char* slash = std::strrchr(exe, '\\');
        if (!slash)
            return {};
        *(slash + 1) = '\0';

        std::string dir = std::string(exe) + "configs";
        CreateDirectoryA(dir.c_str(), nullptr);
        return dir;
    }

    std::string config_path(const char* name)
    {
        const std::string safe = sanitize(name);
        if (safe.empty())
            return {};

        const std::string dir = configs_dir();
        if (dir.empty())
            return {};

        return dir + "\\" + safe + ".cfg";
    }

    // --- autosave debounce -------------------------------------------------

    std::string g_last_serialized;
    std::string g_last_name;
    float       g_pending = 0.f;

    constexpr float k_debounce = 1.5f;

    class cloud_writer
    {
    public:
        cloud_writer() : thread_([this] { run(); }) {}

        ~cloud_writer()
        {
            {
                std::lock_guard<std::mutex> lock(mutex_);
                stopping_ = true;
            }
            ready_.notify_one();
            if (thread_.joinable())
                thread_.join();
        }

        void enqueue(std::string name, std::string content)
        {
            {
                std::lock_guard<std::mutex> lock(mutex_);
                name_ = std::move(name);
                content_ = std::move(content);
                pending_ = true;
            }
            ready_.notify_one();
        }

    private:
        void run()
        {
            for (;;)
            {
                std::string name;
                std::string content;
                {
                    std::unique_lock<std::mutex> lock(mutex_);
                    ready_.wait(lock, [this] { return stopping_ || pending_; });
                    if (stopping_ && !pending_)
                        return;
                    name = std::move(name_);
                    content = std::move(content_);
                    pending_ = false;
                }

                long long updated = 0;
                std::string error;
                api::put_named_config(name, content, updated, error);
            }
        }

        std::mutex              mutex_;
        std::condition_variable ready_;
        std::thread             thread_;
        bool                    stopping_ = false;
        bool                    pending_ = false;
        std::string             name_;
        std::string             content_;
    };

    cloud_writer& background_writer()
    {
        static cloud_writer writer;
        return writer;
    }
}

namespace config_store
{
    void bind(const bundle& b)
    {
        g_bundle = b;
    }

    std::string serialize()
    {
        section sections[3];
        int count = 0;
        collect(sections, count);

        std::string out;
        out.reserve(1024);
        out += "v=1\n";

        for (int i = 0; i < count; ++i)
        {
            const section& sec = sections[i];
            for (int j = 0; j < sec.count; ++j)
            {
                out += sec.name;
                out += '.';
                out += sec.fields[j].key;
                out += '=';
                append_value(out, sec, sec.fields[j]);
                out += '\n';
            }
        }

        return out;
    }

    bool deserialize(const char* text)
    {
        if (!text)
            return false;

        section sections[3];
        int count = 0;
        collect(sections, count);

        for (const char* line = text; *line;)
        {
            const char* end = std::strchr(line, '\n');
            if (!end)
                end = line + std::strlen(line);

            const char* eq = std::strchr(line, '=');
            if (eq && eq < end)
            {
                const std::string key(line, eq - line);
                std::string value(eq + 1, end - eq - 1);
                while (!value.empty() && (value.back() == '\r' || value.back() == ' '))
                    value.pop_back();

                const std::size_t dot = key.find('.');
                if (dot != std::string::npos)
                {
                    const std::string sec_name = key.substr(0, dot);
                    const std::string field_name = key.substr(dot + 1);

                    for (int i = 0; i < count; ++i)
                    {
                        if (sec_name != sections[i].name)
                            continue;
                        for (int j = 0; j < sections[i].count; ++j)
                            if (field_name == sections[i].fields[j].key)
                                parse_value(sections[i], sections[i].fields[j], value.c_str());
                    }
                }
            }

            line = *end ? end + 1 : end;
        }

        return true;
    }

    bool list(std::vector<entry>& out)
    {
        out.clear();

        std::vector<api::named_config_meta> cloud;
        std::string error;
        if (!api::list_named_configs(cloud, error))
            return false;

        out.reserve(cloud.size());
        for (const auto& item : cloud)
            out.push_back({ item.name, item.updated_ts });
        return true;
    }

    int migrate_legacy()
    {
        const std::string dir = configs_dir();
        if (dir.empty())
            return 0;

        WIN32_FIND_DATAA found{};
        HANDLE search = FindFirstFileA((dir + "\\*.cfg").c_str(), &found);
        if (search == INVALID_HANDLE_VALUE)
            return 0;

        int migrated = 0;
        do
        {
            if (found.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY)
                continue;

            std::string filename = found.cFileName;
            if (filename.size() <= 4 || filename.substr(filename.size() - 4) != ".cfg")
                continue;

            std::string name = filename.substr(0, filename.size() - 4);
            if (name.empty() || sanitize(name.c_str()) != name)
                continue;

            const std::string path = dir + "\\" + filename;
            FILE* f = nullptr;
            if (fopen_s(&f, path.c_str(), "rb") != 0 || !f)
                continue;

            std::fseek(f, 0, SEEK_END);
            const long size = std::ftell(f);
            std::fseek(f, 0, SEEK_SET);
            if (size <= 0 || size > 1 << 20)
            {
                std::fclose(f);
                continue;
            }

            std::string content((std::size_t)size, '\0');
            const std::size_t got = std::fread(content.data(), 1, content.size(), f);
            std::fclose(f);
            if (got != content.size())
                continue;

            long long updated = 0;
            std::string error;
            if (api::put_named_config(name, content, updated, error))
                ++migrated;
        }
        while (FindNextFileA(search, &found));

        FindClose(search);
        return migrated;
    }

    bool exists(const char* name)
    {
        std::string content;
        long long updated = 0;
        std::string error;
        return name && api::get_named_config(name, content, updated, error);
    }

    bool read(const char* name, std::string& out)
    {
        out.clear();
        long long updated = 0;
        std::string error;
        return name && api::get_named_config(name, out, updated, error);
    }

    bool save(const char* name)
    {
        if (!name || !name[0])
            return false;

        const std::string text = serialize();
        long long updated = 0;
        std::string error;
        if (!api::put_named_config(name, text, updated, error))
            return false;

        g_last_name       = name;
        g_last_serialized = text;
        g_pending         = 0.f;
        return true;
    }

    bool load(const char* name)
    {
        std::string data;
        if (!read(name, data))
            return false;

        if (!deserialize(data.c_str()))
            return false;

        sync(name);
        return true;
    }

    bool remove(const char* name)
    {
        std::string error;
        return name && api::delete_named_config(name, error);
    }

    bool rename(const char* from, const char* to)
    {
        if (!from || !from[0] || !to || !to[0])
            return false;

        long long updated = 0;
        std::string error;
        if (!api::rename_named_config(from, to, updated, error))
            return false;

        if (g_last_name == from)
            g_last_name = to;
        return true;
    }

    void sync(const char* name)
    {
        g_last_name       = name ? name : "";
        g_last_serialized = serialize();
        g_pending         = 0.f;
    }

    bool autosave(const char* name, float dt)
    {
        const std::string text = serialize();
        const bool same_config = g_last_name == (name ? name : "");

        if (same_config && text == g_last_serialized)
        {
            g_pending = 0.f;
            return false;
        }

        // A different config than the last write means the user just switched;
        // its file is the truth, so record the new baseline instead of
        // overwriting it with whatever was in memory.
        if (!same_config)
        {
            sync(name);
            return false;
        }

        g_pending += dt;
        if (g_pending < k_debounce)
            return false;

        background_writer().enqueue(name ? name : "", text);
        g_last_name       = name ? name : "";
        g_last_serialized = text;
        g_pending         = 0.f;
        return true;
    }
}
