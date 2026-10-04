#pragma once

#include <string>

#include "imgui.h"

struct ID3D11Device;

// Fetches the logged-in user's profile from the forum and hands the UI a
// ready-to-draw view of it. The network round trip runs on its own thread; the
// widget never blocks and never sees a half-filled struct.
namespace profile_svc
{
    enum class state
    {
        loading,
        ready,
        error
    };

    struct view
    {
        state        st = state::loading;
        std::string  username;   // or a placeholder for loading/error
        std::string  till;       // "till: 01.01.2026" / "till: lifetime" / "till: —"
        std::string  initial;    // single character drawn when there is no avatar
        ImTextureID  avatar = 0; // 0 until the image is decoded and uploaded
    };

    void init(ID3D11Device* device);
    void poll();               // UI thread: publishes the result, uploads the avatar
    const view& get();
    void shutdown();
}
