#include "cloud_sync.h"
#include "pages/config_page.h"
#include "core/config_store.h"

#include <windows.h>
#include <wininet.h>
#include <nlohmann/json.hpp>
#include <ctime>

#pragma comment(lib, "wininet.lib")

using json = nlohmann::json;

namespace
{
    std::string g_endpoint;
    std::string g_token;
    cloud_sync::status g_status = cloud_sync::status::idle;
    std::string g_last_error;

    std::string get_timestamp()
    {
        time_t now = time(nullptr);
        struct tm timeinfo;
        localtime_s(&timeinfo, &now);
        char buf[64];
        strftime(buf, sizeof(buf), "%Y-%m-%d %H:%M:%S", &timeinfo);
        return buf;
    }

    std::string http_request(const std::string& method, const std::string& path, const std::string& data = "")
    {
        HINTERNET hSession = InternetOpenA("ConfigSync/1.0", INTERNET_OPEN_TYPE_DIRECT, nullptr, nullptr, 0);
        if (!hSession)
        {
            g_last_error = "Failed to open internet session";
            g_status = cloud_sync::status::error;
            return "";
        }

        HINTERNET hConnect = InternetConnectA(hSession, g_endpoint.c_str(), 443, nullptr, nullptr, INTERNET_SERVICE_HTTP, INTERNET_FLAG_SECURE, 0);
        if (!hConnect)
        {
            g_last_error = "Failed to connect to server";
            InternetCloseHandle(hSession);
            g_status = cloud_sync::status::error;
            return "";
        }

        HINTERNET hRequest = HttpOpenRequestA(hConnect, method.c_str(), path.c_str(), nullptr, nullptr, nullptr, INTERNET_FLAG_SECURE | INTERNET_FLAG_NO_CACHE_WRITE, 0);
        if (!hRequest)
        {
            g_last_error = "Failed to open HTTP request";
            InternetCloseHandle(hConnect);
            InternetCloseHandle(hSession);
            g_status = cloud_sync::status::error;
            return "";
        }

        // Add auth header
        std::string auth_header = "Authorization: Bearer " + g_token;
        HttpAddRequestHeadersA(hRequest, auth_header.c_str(), (DWORD)auth_header.size(), HTTP_ADDREQ_FLAG_REPLACE);
        HttpAddRequestHeadersA(hRequest, "Content-Type: application/json", -1L, HTTP_ADDREQ_FLAG_ADD);

        BOOL send_ok = HttpSendRequestA(hRequest, nullptr, 0, (LPVOID)data.c_str(), (DWORD)data.size());
        if (!send_ok)
        {
            g_last_error = "Failed to send HTTP request";
            InternetCloseHandle(hRequest);
            InternetCloseHandle(hConnect);
            InternetCloseHandle(hSession);
            g_status = cloud_sync::status::error;
            return "";
        }

        std::string response;
        char buf[4096];
        DWORD bytes_read;
        while (InternetReadFile(hRequest, buf, sizeof(buf), &bytes_read) && bytes_read > 0)
            response.append(buf, bytes_read);

        InternetCloseHandle(hRequest);
        InternetCloseHandle(hConnect);
        InternetCloseHandle(hSession);

        return response;
    }
}

namespace cloud_sync
{
    void init(const std::string& endpoint, const std::string& token)
    {
        g_endpoint = endpoint;
        g_token = token;
        g_status = status::idle;
        g_last_error.clear();
    }

    status fetch_configs(std::vector<config_meta>& out, std::string& error)
    {
        if (g_endpoint.empty() || g_token.empty())
        {
            error = "Cloud sync not initialized";
            return g_status = status::error;
        }

        g_status = status::syncing;
        std::string response = http_request("GET", "/api/configs");

        if (g_status == status::error)
        {
            error = g_last_error;
            return status::error;
        }

        try
        {
            auto j = json::parse(response);
            if (j.contains("configs"))
            {
                for (const auto& cfg : j["configs"])
                {
                    config_meta meta;
                    meta.name = cfg["name"];
                    meta.date = cfg.value("updated", "Unknown");
                    meta.data = cfg["content"];
                    out.push_back(meta);
                }
            }
            g_status = status::success;
            return status::success;
        }
        catch (const std::exception& e)
        {
            error = std::string("JSON parse error: ") + e.what();
            g_last_error = error;
            return g_status = status::error;
        }
    }

    status save_config(const std::string& name, const std::string& data, std::string& error)
    {
        if (g_endpoint.empty() || g_token.empty())
        {
            error = "Cloud sync not initialized";
            return g_status = status::error;
        }

        g_status = status::syncing;
        json payload;
        payload["name"] = name;
        payload["content"] = data;
        payload["updated"] = get_timestamp();

        std::string response = http_request("POST", "/api/configs", payload.dump());

        if (g_status == status::error)
        {
            error = g_last_error;
            return status::error;
        }

        try
        {
            auto j = json::parse(response);
            if (j.value("success", false))
            {
                g_status = status::success;
                return status::success;
            }
            error = j.value("error", "Unknown server error");
            g_last_error = error;
            return g_status = status::error;
        }
        catch (const std::exception& e)
        {
            error = std::string("JSON parse error: ") + e.what();
            g_last_error = error;
            return g_status = status::error;
        }
    }

    status load_config(const std::string& name, std::string& out_data, std::string& error)
    {
        if (g_endpoint.empty() || g_token.empty())
        {
            error = "Cloud sync not initialized";
            return g_status = status::error;
        }

        g_status = status::syncing;
        std::string path = "/api/configs/" + name;
        std::string response = http_request("GET", path);

        if (g_status == status::error)
        {
            error = g_last_error;
            return status::error;
        }

        try
        {
            auto j = json::parse(response);
            if (j.contains("content"))
            {
                out_data = j["content"];
                g_status = status::success;
                return status::success;
            }
            error = j.value("error", "Config not found");
            g_last_error = error;
            return g_status = status::error;
        }
        catch (const std::exception& e)
        {
            error = std::string("JSON parse error: ") + e.what();
            g_last_error = error;
            return g_status = status::error;
        }
    }

    status delete_config(const std::string& name, std::string& error)
    {
        if (g_endpoint.empty() || g_token.empty())
        {
            error = "Cloud sync not initialized";
            return g_status = status::error;
        }

        g_status = status::syncing;
        std::string path = "/api/configs/" + name;
        std::string response = http_request("DELETE", path);

        if (g_status == status::error)
        {
            error = g_last_error;
            return status::error;
        }

        try
        {
            auto j = json::parse(response);
            if (j.value("success", false))
            {
                g_status = status::success;
                return status::success;
            }
            error = j.value("error", "Unknown server error");
            g_last_error = error;
            return g_status = status::error;
        }
        catch (const std::exception& e)
        {
            error = std::string("JSON parse error: ") + e.what();
            g_last_error = error;
            return g_status = status::error;
        }
    }

    status rename_config(const std::string& old_name, const std::string& new_name, std::string& error)
    {
        if (g_endpoint.empty() || g_token.empty())
        {
            error = "Cloud sync not initialized";
            return g_status = status::error;
        }

        g_status = status::syncing;
        json payload;
        payload["new_name"] = new_name;

        std::string path = "/api/configs/" + old_name + "/rename";
        std::string response = http_request("PUT", path, payload.dump());

        if (g_status == status::error)
        {
            error = g_last_error;
            return status::error;
        }

        try
        {
            auto j = json::parse(response);
            if (j.value("success", false))
            {
                g_status = status::success;
                return status::success;
            }
            error = j.value("error", "Unknown server error");
            g_last_error = error;
            return g_status = status::error;
        }
        catch (const std::exception& e)
        {
            error = std::string("JSON parse error: ") + e.what();
            g_last_error = error;
            return g_status = status::error;
        }
    }

    status get_status()
    {
        return g_status;
    }

    const std::string& get_last_error()
    {
        return g_last_error;
    }

    void sync_configs_from_cloud(config_page::state& s)
    {
        std::vector<config_meta> configs;
        std::string error;

        if (fetch_configs(configs, error) != status::success)
        {
            config_store::sync(s.active);
            return;
        }

        s.count = 0;
        for (const auto& cfg : configs)
        {
            if (s.count >= config_page::state::max_configs)
                break;

            strncpy_s(s.names[s.count], sizeof(s.names[s.count]), cfg.name.c_str(), _TRUNCATE);
            strncpy_s(s.dates[s.count], sizeof(s.dates[s.count]), cfg.date.c_str(), _TRUNCATE);

            if (s.count == 0)
            {
                config_store::deserialize(cfg.data.c_str());
                strncpy_s(s.active, sizeof(s.active), cfg.name.c_str(), _TRUNCATE);
            }

            s.count++;
        }

        if (s.count == 0)
        {
            strncpy_s(s.names[0], sizeof(s.names[0]), "Default", _TRUNCATE);
            strncpy_s(s.dates[0], sizeof(s.dates[0]), "Now", _TRUNCATE);
            s.count = 1;
            s.selected = 0;
        }
    }

    void sync_config_to_cloud(const std::string& name, const std::string& data)
    {
        std::string error;
        save_config(name, data, error);
    }
}
