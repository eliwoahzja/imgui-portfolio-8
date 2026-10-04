#pragma once

#include <string>
#include <vector>
#include <cstddef>

namespace config_page
{
    struct state;
}

namespace cloud_sync
{
    enum class status
    {
        idle,
        syncing,
        success,
        error
    };

    struct config_meta
    {
        std::string name;
        std::string date;
        std::string data;
    };

    // Initialize cloud sync with API endpoint and auth token
    void init(const std::string& endpoint, const std::string& token);

    // Fetch all configs from cloud
    status fetch_configs(std::vector<config_meta>& out, std::string& error);

    // Save single config to cloud
    status save_config(const std::string& name, const std::string& data, std::string& error);

    // Load single config from cloud
    status load_config(const std::string& name, std::string& out_data, std::string& error);

    // Delete config from cloud
    status delete_config(const std::string& name, std::string& error);

    // Rename config on cloud
    status rename_config(const std::string& old_name, const std::string& new_name, std::string& error);

    // Get last sync status
    status get_status();

    // Get last error message
    const std::string& get_last_error();

    // Sync page state with cloud (replaces local save/load)
    void sync_configs_from_cloud(config_page::state& s);
    void sync_config_to_cloud(const std::string& name, const std::string& data);
}
