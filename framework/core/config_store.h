#pragma once

#include <string>
#include <vector>

namespace aim_page      { struct state; }
namespace visuals_page  { struct state; }
namespace settings_page { struct state; }

// Config persistence: one text file per config under <exe dir>/configs.
//
// The config page owns the *list* of configs — names and dates only. The values
// live in the page states, so every operation here works on all of them at once
// through the bundle bound at startup.
namespace config_store
{
    struct entry
    {
        std::string name;
        long long   updated_ts = 0;
    };

    struct bundle
    {
        aim_page::state*      aim      = nullptr;
        visuals_page::state*  visuals  = nullptr;
        settings_page::state* settings = nullptr;
    };

    // There is exactly one set of page states in the process, so the bundle is
    // bound once instead of threaded through every call site.
    void bind(const bundle& b);

    // key=value text, stable field order. Also the wire format for cloud
    // configs, which upload this verbatim.
    std::string serialize();

    // Unknown keys are ignored and missing ones keep their current value, so an
    // older file still loads after fields are added.
    bool deserialize(const char* text);

    bool list(std::vector<entry>& out);
    int migrate_legacy();
    bool exists(const char* name);
    bool read(const char* name, std::string& out);
    bool save(const char* name);
    bool load(const char* name);
    bool remove(const char* name);
    bool rename(const char* from, const char* to);

    // Debounced write of `name`. A dragged slider changes values every frame, so
    // the write waits for them to settle. Returns true on the frame it wrote.
    bool autosave(const char* name, float dt);

    // Tells autosave the file already matches memory — after an explicit save or
    // load, and when the active config changes.
    void sync(const char* name);
}
