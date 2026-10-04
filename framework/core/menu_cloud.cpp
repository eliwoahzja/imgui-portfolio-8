#include "menu_cloud.h"

#include "core/config_store.h"
#include "net/api.h"
#include "pages/config_page.h"

#include <algorithm>
#include <cctype>
#include <cstdio>
#include <cstring>
#include <string>

namespace
{
    void copy_str(char* dst, std::size_t dst_size, const char* src)
    {
        if (dst && dst_size)
            strncpy_s(dst, dst_size, src ? src : "", _TRUNCATE);
    }

    const char* friendly_error(const std::string& error)
    {
        if (error == "no_token")            return "Token not found next to the app";
        if (error == "invalid_credentials") return "Token is invalid or revoked";
        if (error == "banned")              return "This account cannot use cloud configs";
        if (error == "not_found")           return "Share code was not found";
        if (error == "limit_reached")       return "Cloud config limit reached";
        if (error == "too_large")           return "Config is too large for cloud storage";
        if (error == "bad_request")         return "Invalid config or share code";
        if (error == "network")             return "Could not reach the cloud server";
        if (error == "data_error")          return "Cloud storage is not initialized";
        if (error == "already_exists")      return "A cloud config with this name already exists";
        return "Cloud request failed";
    }

    bool name_exists(const config_page::state& s, const char* name)
    {
        for (int i = 0; i < s.count; ++i)
            if (std::strcmp(s.names[i], name) == 0)
                return true;
        return false;
    }

    std::string safe_name(const std::string& input)
    {
        std::string out;
        out.reserve(config_page::state::name_len - 1);
        for (unsigned char c : input)
        {
            const bool allowed = std::isalnum(c) || c == ' ' || c == '-' || c == '_';
            if (allowed && out.size() + 1 < config_page::state::name_len)
                out.push_back((char)c);
        }

        while (!out.empty() && out.back() == ' ')
            out.pop_back();
        return out.empty() ? std::string("Imported config") : out;
    }

    std::string unique_name(const config_page::state& s, const std::string& source)
    {
        std::string base = safe_name(source);
        if (!name_exists(s, base.c_str()))
            return base;

        for (int n = 2; n < 100; ++n)
        {
            char suffix[24];
            sprintf_s(suffix, " import %d", n);

            std::string candidate = base;
            const std::size_t max_base = config_page::state::name_len - 1 - std::strlen(suffix);
            if (candidate.size() > max_base)
                candidate.resize(max_base);
            candidate += suffix;

            if (!name_exists(s, candidate.c_str()))
                return candidate;
        }
        return {};
    }
}

namespace menu_cloud
{
    bool export_code(const config_page::state& s, char* out, std::size_t out_size,
        char* error, std::size_t error_size)
    {
        if (!out || out_size == 0 || s.count <= 0)
            return false;

        out[0] = '\0';
        const int selected = std::clamp(s.selected, 0, s.count - 1);
        const char* name = s.names[selected];

        std::string content;
        if (std::strcmp(name, s.active) == 0)
            content = config_store::serialize();
        else if (!config_store::read(name, content))
        {
            copy_str(error, error_size, "Save this config before sharing it");
            return false;
        }

        std::string code;
        std::string api_error;
        if (!api::save_cloud_config(name, content, code, api_error))
        {
            copy_str(error, error_size, friendly_error(api_error));
            return false;
        }

        copy_str(out, out_size, code.c_str());
        copy_str(error, error_size, "Share code ready");
        return true;
    }

    bool import_code(const char* code, config_page::state& s, char* error,
        std::size_t error_size)
    {
        if (!code || !code[0])
        {
            copy_str(error, error_size, "Paste a share code");
            return false;
        }
        if (s.count >= config_page::state::max_configs)
        {
            copy_str(error, error_size, "Config list is full");
            return false;
        }

        std::string normalized;
        for (const unsigned char c : std::string(code))
        {
            if (std::isspace(c))
                continue;
            normalized.push_back((char)std::tolower(c));
        }

        api::cloud_config_data cloud;
        std::string api_error;
        if (!api::load_cloud_config(normalized, cloud, api_error))
        {
            copy_str(error, error_size, friendly_error(api_error));
            return false;
        }

        const std::string name = unique_name(s, cloud.name);
        if (name.empty())
        {
            copy_str(error, error_size, "Could not choose a local config name");
            return false;
        }

        if (!config_store::deserialize(cloud.content.c_str()) || !config_store::save(name.c_str()))
        {
            copy_str(error, error_size, "Could not cache the cloud config");
            return false;
        }

        copy_str(s.names[s.count], sizeof(s.names[s.count]), name.c_str());
        copy_str(s.dates[s.count], sizeof(s.dates[s.count]), "Just now");
        s.selected = s.count;
        ++s.count;
        copy_str(s.active, sizeof(s.active), name.c_str());
        config_store::sync(s.active);

        copy_str(error, error_size, "Cloud config imported");
        return true;
    }
}
