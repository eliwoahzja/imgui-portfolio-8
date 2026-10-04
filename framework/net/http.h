#pragma once

#include <string>
#include <vector>
#include <cstdint>

// ships session keys in the clear otherwise.
namespace http
{
    // `path` is the absolute path with query, e.g. "/forum/app.php?op=...".
    bool get(const std::string& path, std::string& out);
    bool get_bytes(const std::string& path, std::vector<unsigned char>& out);

    // Shared transport health. A failed request starts one continuous outage;
    // any later HTTP 200 response clears it. The reconnect probe runs away
    // from the render thread, so these accessors are safe to call every frame.
    void start_monitor();
    void stop_monitor();
    bool is_offline();
    std::uint64_t offline_ms();

    std::string url_encode(const std::string& raw);
}
