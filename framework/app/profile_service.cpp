#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <d3d11.h>

#include <cstdio>
#include <ctime>
#include <mutex>
#include <thread>
#include <vector>

#include "profile_service.h"

#include "../net/api.h"
#include "../net/http.h"

#include "../../thirdparty/stb/stb_image.h"

namespace
{
    ID3D11Device* s_device = nullptr;

    std::thread s_worker;

    std::mutex  s_mutex;
    profile_svc::view s_pending;                 // written by the worker
    std::vector<unsigned char> s_pixels;         // RGBA, worker-decoded
    int s_pixels_w = 0;
    int s_pixels_h = 0;

    profile_svc::view s_published;               // read by the UI thread
    ID3D11Texture2D*          s_avatar_tex = nullptr;
    ID3D11ShaderResourceView* s_avatar_srv = nullptr;

    std::string format_till(long long end_ts)
    {
        if (end_ts <= 0)
            return "till: lifetime";

        const __time64_t t = (__time64_t)end_ts;
        tm parts = {};
        if (_localtime64_s(&parts, &t) != 0)
            return "till: \xE2\x80\x94";

        char buf[32];
        sprintf_s(buf, "till: %02d.%02d.%04d",
            parts.tm_mday, parts.tm_mon + 1, parts.tm_year + 1900);
        return buf;
    }

    std::string first_letter(const std::string& name)
    {
        if (name.empty())
            return "?";

        const unsigned char c = (unsigned char)name[0];

        // Multi-byte lead bytes carry no meaning on their own, so a non-ASCII
        // name falls back to the glyph the placeholder already used.
        if (c < 0x20 || c > 0x7E)
            return "?";

        char buf[2] = { (char)toupper(c), '\0' };
        return buf;
    }

    void fetch_avatar(const api::profile_data& p)
    {
        const std::string path = api::avatar_path(p.uid, p.avatar_date);
        if (path.empty())
            return;

        std::vector<unsigned char> jpeg;
        if (!http::get_bytes(path, jpeg))
            return;

        int w = 0, h = 0, channels = 0;
        unsigned char* rgba = stbi_load_from_memory(
            jpeg.data(), (int)jpeg.size(), &w, &h, &channels, 4);
        if (!rgba)
            return;

        if (w > 0 && h > 0)
        {
            std::lock_guard<std::mutex> lock(s_mutex);
            s_pixels.assign(rgba, rgba + (size_t)w * h * 4);
            s_pixels_w = w;
            s_pixels_h = h;
        }

        stbi_image_free(rgba);
    }

    void worker()
    {
        api::profile_data data;
        std::string err;

        profile_svc::view result;

        if (api::fetch_profile(data, err))
        {
            result.st       = profile_svc::state::ready;
            result.username = data.username;
            result.till     = format_till(data.end_ts);
            result.initial  = first_letter(data.username);
        }
        else
        {
            // The reason is no longer shown in the sidebar, so it goes to the
            // debugger output instead of vanishing.
            OutputDebugStringA(("[profile] fetch failed: " + err + "\n").c_str());

            result.st       = profile_svc::state::error;
            result.username = "offline";
            result.till     = "till: \xE2\x80\x94";
            result.initial  = "?";
        }

        {
            std::lock_guard<std::mutex> lock(s_mutex);
            s_pending = result;
        }

        if (result.st == profile_svc::state::ready)
            fetch_avatar(data);
    }

    // Upload happens on the UI thread because the device context is not
    // free-threaded; the worker only ever produces the pixel buffer.
    void upload_avatar(const std::vector<unsigned char>& pixels, int w, int h)
    {
        if (!s_device || pixels.empty() || w <= 0 || h <= 0)
            return;

        D3D11_TEXTURE2D_DESC desc = {};
        desc.Width = (UINT)w;
        desc.Height = (UINT)h;
        desc.MipLevels = 1;
        desc.ArraySize = 1;
        desc.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
        desc.SampleDesc.Count = 1;
        desc.Usage = D3D11_USAGE_IMMUTABLE;
        desc.BindFlags = D3D11_BIND_SHADER_RESOURCE;

        D3D11_SUBRESOURCE_DATA init = {};
        init.pSysMem = pixels.data();
        init.SysMemPitch = (UINT)w * 4;

        if (FAILED(s_device->CreateTexture2D(&desc, &init, &s_avatar_tex)))
            return;

        if (FAILED(s_device->CreateShaderResourceView(s_avatar_tex, nullptr, &s_avatar_srv)))
        {
            s_avatar_tex->Release();
            s_avatar_tex = nullptr;
            return;
        }

        s_published.avatar = (ImTextureID)s_avatar_srv;
    }
}

namespace profile_svc
{
    void init(ID3D11Device* device)
    {
        s_device = device;

        s_published.st      = state::loading;
        s_published.username = "loading\xE2\x80\xA6";
        s_published.till    = "till: \xE2\x80\x94";
        s_published.initial = "?";

        s_worker = std::thread(worker);
    }

    void poll()
    {
        std::vector<unsigned char> pixels;
        int w = 0, h = 0;

        {
            std::lock_guard<std::mutex> lock(s_mutex);

            if (s_published.st == state::loading && s_pending.st != state::loading)
            {
                const ImTextureID keep = s_published.avatar;
                s_published = s_pending;
                s_published.avatar = keep;
            }

            if (!s_avatar_srv && !s_pixels.empty())
            {
                pixels.swap(s_pixels);
                w = s_pixels_w;
                h = s_pixels_h;
            }
        }

        if (!pixels.empty())
            upload_avatar(pixels, w, h);
    }

    const view& get()
    {
        return s_published;
    }

    void shutdown()
    {
        if (s_worker.joinable())
            s_worker.join();

        if (s_avatar_srv) { s_avatar_srv->Release(); s_avatar_srv = nullptr; }
        if (s_avatar_tex) { s_avatar_tex->Release(); s_avatar_tex = nullptr; }

        s_published.avatar = 0;
        s_device = nullptr;
    }
}
