#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <wininet.h>

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstdint>
#include <mutex>
#include <thread>

#ifdef RF_HTTP_DEBUG
#include <cstdio>
#endif

#include "http.h"

#pragma comment(lib, "wininet.lib")

namespace
{
    constexpr wchar_t host[] = L"YOUR CHEAT PLACE";
    constexpr DWORD  timeout_ms = 5000;

    std::atomic<int> s_health{ 0 }; // 0 unknown, 1 online, 2 offline
    std::atomic<ULONGLONG> s_offline_since{ 0 };
    std::mutex s_monitor_mutex;
    std::condition_variable s_monitor_wake;
    std::thread s_monitor;
    bool s_monitor_stop = false;

    void mark_online()
    {
        s_offline_since.store(0, std::memory_order_release);
        s_health.store(1, std::memory_order_release);
    }

    void mark_offline()
    {
        ULONGLONG empty = 0;
        s_offline_since.compare_exchange_strong(empty, GetTickCount64(),
            std::memory_order_acq_rel);
        s_health.store(2, std::memory_order_release);
        s_monitor_wake.notify_one();
    }

    // WinINet keeps its own per-process cookie store, so PHPSESSID survives
    // between calls without a jar of our own. That only matters against a
    // server whose sid columns are missing; normally `s=` carries the session.
    bool fetch(const std::string& path, std::vector<unsigned char>& out)
    {
        out.clear();

        HINTERNET net = InternetOpenW(L"rf", INTERNET_OPEN_TYPE_PRECONFIG, //Эта хуйня вроде бы для токена.
            nullptr, nullptr, 0);
        if (!net)
        {
            mark_offline();
            return false;
        }

        HINTERNET conn = InternetConnectW(net, host, INTERNET_DEFAULT_HTTPS_PORT,
            nullptr, nullptr, INTERNET_SERVICE_HTTP, 0, 0);
        if (!conn)
        {
            InternetCloseHandle(net);
            mark_offline();
            return false;
        }

        // The path is percent-encoded ASCII by the time it gets here, so a
        // byte-wise widen is exact.
        std::wstring wide;
        wide.reserve(path.size());
        for (unsigned char c : path)
            wide.push_back((wchar_t)c);

        const wchar_t* accept[] = { L"*/*", nullptr };

        // No IGNORE_CERT flags: an invalid certificate must fail the request.
        HINTERNET req = HttpOpenRequestW(conn, L"GET", wide.c_str(), nullptr, nullptr,
            accept,
            INTERNET_FLAG_SECURE | INTERNET_FLAG_RELOAD
            | INTERNET_FLAG_NO_CACHE_WRITE | INTERNET_FLAG_PRAGMA_NOCACHE,
            0);
        if (!req)
        {
            InternetCloseHandle(conn);
            InternetCloseHandle(net);
            mark_offline();
            return false;
        }

        DWORD t = timeout_ms;
        InternetSetOptionW(req, INTERNET_OPTION_CONNECT_TIMEOUT, &t, sizeof(t));
        InternetSetOptionW(req, INTERNET_OPTION_SEND_TIMEOUT,    &t, sizeof(t));
        InternetSetOptionW(req, INTERNET_OPTION_RECEIVE_TIMEOUT, &t, sizeof(t));

        bool ok = HttpSendRequestW(req, nullptr, 0, nullptr, 0) != FALSE;

#ifdef RF_HTTP_DEBUG
        if (!ok)
            printf("[http] send failed, err=%lu\n", GetLastError());
#endif

        if (ok)
        {
            DWORD status = 0, len = sizeof(status);
            if (HttpQueryInfoW(req, HTTP_QUERY_STATUS_CODE | HTTP_QUERY_FLAG_NUMBER,
                    &status, &len, nullptr) && status != 200)
            {
#ifdef RF_HTTP_DEBUG
                printf("[http] status=%lu\n", status);
#endif
                ok = false;
            }
        }

        if (ok)
        {
            unsigned char buf[65536];
            DWORD read = 0;
            for (;;)
            {
                if (!InternetReadFile(req, buf, sizeof(buf), &read))
                {
                    ok = false;
                    break;
                }
                if (!read)
                    break;
                out.insert(out.end(), buf, buf + read);
            }
        }

        InternetCloseHandle(req);
        InternetCloseHandle(conn);
        InternetCloseHandle(net);
        ok = ok && !out.empty();
        if (ok)
            mark_online();
        else
            mark_offline();
        return ok;
    }

    void monitor_worker()
    {
        std::unique_lock<std::mutex> lock(s_monitor_mutex);
        while (!s_monitor_stop)
        {
            lock.unlock();
            std::vector<unsigned char> ignored;
            fetch("/forum/app.php", ignored);
            lock.lock();
            if (!s_monitor_stop)
                s_monitor_wake.wait_for(lock, std::chrono::seconds(10), [] {
                    return s_monitor_stop;
                });
        }
    }
}

namespace http
{
    bool get(const std::string& path, std::string& out)
    {
        std::vector<unsigned char> bytes;
        if (!fetch(path, bytes))
            return false;

        out.assign(bytes.begin(), bytes.end());
        return true;
    }

    bool get_bytes(const std::string& path, std::vector<unsigned char>& out)
    {
        return fetch(path, out);
    }

    void start_monitor()
    {
        std::lock_guard<std::mutex> lock(s_monitor_mutex);
        if (s_monitor.joinable())
            return;
        s_monitor_stop = false;
        s_monitor = std::thread(monitor_worker);
    }

    void stop_monitor()
    {
        {
            std::lock_guard<std::mutex> lock(s_monitor_mutex);
            s_monitor_stop = true;
        }
        s_monitor_wake.notify_all();
        if (s_monitor.joinable())
            s_monitor.join();
    }

    bool is_offline()
    {
        return s_health.load(std::memory_order_acquire) == 2;
    }

    std::uint64_t offline_ms()
    {
        const ULONGLONG since = s_offline_since.load(std::memory_order_acquire);
        return since ? (std::uint64_t)(GetTickCount64() - since) : 0;
    }

    std::string url_encode(const std::string& raw)
    {
        static const char hex[] = "0123456789ABCDEF";

        std::string out;
        out.reserve(raw.size() * 3);

        for (unsigned char c : raw)
        {
            if ((c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z')
                || (c >= '0' && c <= '9')
                || c == '-' || c == '_' || c == '.' || c == '~')
            {
                out.push_back((char)c);
            }
            else
            {
                out.push_back('%');
                out.push_back(hex[c >> 4]);
                out.push_back(hex[c & 0x0F]);
            }
        }
        return out;
    }
}
