#pragma once

#include <string>
#include <vector>

namespace api
{
    struct profile_data
    {
        int         uid         = 0;
        long long   end_ts      = 0;   // 0 means lifetime (admin)
        long long   avatar_date = 0;   // 0 means no avatar uploaded
        std::string username;
    };

    struct cloud_config_data
    {
        int         uid = 0;
        std::string code;
        std::string name;
        std::string username;
        std::string content;
    };

    struct named_config_meta
    {
        std::string name;
        long long   updated_ts = 0;
    };

    // Handshake against /forum/app.php, then op=profile authorised by the
    // loader token from token::read_own. Blocking; call off the UI thread.
    // On failure `err` holds the server's error code or a short local reason.
    bool fetch_profile(profile_data& out, std::string& err);

    // Cloud configs are immutable snapshots. Saving returns a share code;
    // loading that code returns the exact serialized config body.
    bool save_cloud_config(const std::string& name, const std::string& content,
        std::string& out_code, std::string& err);
    bool load_cloud_config(const std::string& code, cloud_config_data& out,
        std::string& err);

    bool list_named_configs(std::vector<named_config_meta>& out, std::string& err);
    bool get_named_config(const std::string& name, std::string& out_content,
        long long& out_updated_ts, std::string& err);
    bool put_named_config(const std::string& name, const std::string& content,
        long long& out_updated_ts, std::string& err);
    bool delete_named_config(const std::string& name, std::string& err);
    bool rename_named_config(const std::string& old_name, const std::string& new_name,
        long long& out_updated_ts, std::string& err);

    // Forum path of a user's avatar, following XenForo's sharded layout.
    // Empty when the user has no avatar.
    std::string avatar_path(int uid, long long avatar_date);
}
