#include "core/device.h"
#include "core/window.h"
#include "app/gui.h"
#include "helpers/screenshot.h"

#include <windows.h>

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <vector>

#include "theme/layout.h"
#include "theme/ui_scale.h"
#include "imgui.h"
#include "imgui_impl_dx11.h"
#include "imgui_impl_win32.h"

namespace
{
    // --script: a synthetic input tape for autoshot verification. Nothing here
    // is reachable without the flag, so the shipped build behaves as before.
    //
    //   --script="move:X,Y; click:X,Y; text:hello; key:enter; wait:20"
    //
    // Coordinates are panel-relative design pixels — the same numbers a
    // screenshot is measured in — so a fix can be aimed straight at what the
    // previous capture showed. Each step spans at least two frames because
    // ImGui needs a frame with the button down and a frame with it released to
    // register a click, and hover-driven widgets need a settled mouse first.
    struct script_step
    {
        enum kind { move, click, text, key, wait } op = wait;
        float       x = 0.f, y = 0.f;
        int         frames = 0;
        std::string payload;
    };

    std::vector<script_step> parse_script(const char* spec)
    {
        std::vector<script_step> steps;
        std::string all(spec);

        size_t pos = 0;
        while (pos <= all.size())
        {
            size_t end = all.find(';', pos);
            if (end == std::string::npos) end = all.size();
            std::string item = all.substr(pos, end - pos);
            pos = end + 1;

            size_t b = item.find_first_not_of(" \t");
            if (b == std::string::npos) continue;
            item = item.substr(b, item.find_last_not_of(" \t") - b + 1);

            const size_t colon = item.find(':');
            if (colon == std::string::npos) continue;
            const std::string op  = item.substr(0, colon);
            const std::string arg = item.substr(colon + 1);

            script_step s;
            if (op == "move" || op == "click")
            {
                s.op = (op == "move") ? script_step::move : script_step::click;
                if (sscanf_s(arg.c_str(), "%f,%f", &s.x, &s.y) != 2) continue;
                s.frames = (s.op == script_step::click) ? 6 : 3;
            }
            else if (op == "text")
            {
                s.op = script_step::text;
                s.payload = arg;
                s.frames = 3;
            }
            else if (op == "key")
            {
                s.op = script_step::key;
                s.payload = arg;
                s.frames = 4;
            }
            else if (op == "wait")
            {
                s.op = script_step::wait;
                s.frames = std::atoi(arg.c_str());
                if (s.frames <= 0) s.frames = 10;
            }
            else continue;

            steps.push_back(s);
        }
        return steps;
    }

    ImGuiKey key_from_name(const std::string& name)
    {
        if (name == "enter")  return ImGuiKey_Enter;
        if (name == "tab")    return ImGuiKey_Tab;
        if (name == "escape") return ImGuiKey_Escape;
        if (name == "backspace") return ImGuiKey_Backspace;
        return ImGuiKey_None;
    }
}

int main(int argc, char** argv)
{
    // --autoshot[=N]: capture the panel to bin/screenshots on frame N, then quit.
    // --tab=N: open tab N (0-based) at startup — pairs with --autoshot.
    // --script=...: replay synthetic input before the capture (see parse_script).
    // --shotfull: capture the whole display instead of just the panel, for popups
    //   that anchor outside the shell (menu settings, the colour picker).
    // Used to verify the UI against the Figma reference without manual interaction.
    int autoshot_frame = 0;
    int start_tab = -1;
    bool shot_full = false;
    std::vector<script_step> script;
    for (int i = 1; i < argc; ++i)
    {
        if (std::strncmp(argv[i], "--tab=", 6) == 0)
        {
            start_tab = std::atoi(argv[i] + 6);
            continue;
        }
        if (std::strcmp(argv[i], "--shotfull") == 0)
        {
            shot_full = true;
            continue;
        }
        if (std::strncmp(argv[i], "--script=", 9) == 0)
        {
            script = parse_script(argv[i] + 9);
            continue;
        }
        if (std::strncmp(argv[i], "--autoshot", 10) != 0)
            continue;

        autoshot_frame = argv[i][10] == '=' ? std::atoi(argv[i] + 11) : 0;
        if (autoshot_frame <= 0)
            autoshot_frame = 90;
    }

    ImGui_ImplWin32_EnableDpiAwareness();

    WNDCLASSEXW wc = { sizeof(wc), CS_CLASSDC, app_wnd_proc, 0L, 0L,
        GetModuleHandle(nullptr), nullptr, nullptr, nullptr, nullptr,
        L"ImguiPortfolio8Shell", nullptr };
    ::RegisterClassExW(&wc);

    GfxShared shared = {};
    if (!gfx_create_shared(shared))
    {
        gfx_destroy_shared(shared);
        ::UnregisterClassW(wc.lpszClassName, wc.hInstance);
        return 1;
    }

    AppSurface surface = {};

    const int win_w = ::GetSystemMetrics(SM_CXSCREEN);
    const int win_h = ::GetSystemMetrics(SM_CYSCREEN);

    HWND hwnd = ::CreateWindowExW(
        WS_EX_NOREDIRECTIONBITMAP,
        wc.lpszClassName, L"Imgui Portifolio #8", WS_POPUP,
        0, 0, win_w, win_h,
        nullptr, nullptr, wc.hInstance, nullptr);

    if (hwnd)
        ::SetWindowLongPtrW(hwnd, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(&surface));

    if (!hwnd || !gfx_create_surface(shared, surface, hwnd))
    {
        gfx_destroy_surface(surface);
        gfx_destroy_shared(shared);
        ::UnregisterClassW(wc.lpszClassName, wc.hInstance);
        return 1;
    }

    ::ShowWindow(hwnd, SW_SHOWDEFAULT);
    ::UpdateWindow(hwnd);

    IMGUI_CHECKVERSION();
    ImGui::CreateContext();
    ImGuiIO& io = ImGui::GetIO();
    io.IniFilename = nullptr;
    io.LogFilename = nullptr;

    ImGui_ImplWin32_Init(hwnd);
    ImGui_ImplDX11_Init(shared.device, shared.context);
    gui::initialize(shared.device, shared.context);
    if (start_tab >= 0)
        gui::select_tab(start_tab);
    gui::set_shot_full(shot_full);

    surface.imgui_win32_route   = true;
    surface.quit_app_on_destroy = true;

    bool done = false;
    int  frame_index = 0;
    size_t script_step_index = 0;
    int    script_step_frame = 0;
    while (!done)
    {
        MSG msg;
        while (::PeekMessage(&msg, nullptr, 0U, 0U, PM_REMOVE))
        {
            ::TranslateMessage(&msg);
            ::DispatchMessage(&msg);
            if (msg.message == WM_QUIT)
                done = true;
        }
        if (done) break;

        unsigned rw = 0, rh = 0;
        if (app_surface_consume_resize(surface, rw, rh))
            gfx_resize_surface(shared, surface, rw, rh);

        ImGui_ImplDX11_NewFrame();
        ImGui_ImplWin32_NewFrame();
        app_update_io_for_hwnd(hwnd, io, script.empty());

        // Replay one script step across its frame budget. The capture waits for
        // the tape to finish, so --autoshot=N is a floor, not an exact frame.
        bool script_running = false;
        if (!script.empty())
        {
            static float cursor_x = -FLT_MAX, cursor_y = -FLT_MAX;
            float ox = 0.f, oy = 0.f;
            gui::panel_origin(ox, oy);

            if (script_step_index < script.size())
            {
                script_running = true;
                const script_step& s = script[script_step_index];
                const int f = script_step_frame;

                switch (s.op)
                {
                case script_step::move:
                case script_step::click:
                    cursor_x = ox + ui_scale::s(s.x);
                    cursor_y = oy + ui_scale::s(s.y);
                    break;
                case script_step::text:
                    // One frame settles focus, then the whole string goes in.
                    if (f == 1)
                        for (char c : s.payload)
                            io.AddInputCharacter((unsigned)(unsigned char)c);
                    break;
                case script_step::key:
                    if (ImGuiKey k = key_from_name(s.payload); k != ImGuiKey_None)
                        io.AddKeyEvent(k, f == 1 || f == 2);
                    break;
                case script_step::wait:
                    break;
                }

                io.AddMousePosEvent(cursor_x, cursor_y);
                // Press on the middle frames only: ImGui needs a settled hover
                // beforehand and a released frame afterwards to latch the click.
                io.AddMouseButtonEvent(0,
                    s.op == script_step::click && (f == 2 || f == 3));

                if (++script_step_frame >= s.frames)
                {
                    script_step_frame = 0;
                    ++script_step_index;
                }
            }
            else
            {
                io.AddMousePosEvent(cursor_x, cursor_y);
                io.AddMouseButtonEvent(0, false);
            }
        }

        static int shot_frame = -1;
        if (autoshot_frame > 0 && shot_frame < 0
            && frame_index >= autoshot_frame && !script_running)
        {
            screenshot::request();
            shot_frame = frame_index;
        }
        screenshot::begin_frame();
        gui::prepare_frame();
        ImGui::NewFrame();

        gui::render(hwnd);

        ImGui::Render();
        gfx_render_surface(shared, surface, ImGui::GetDrawData());

        if (shot_frame >= 0 && frame_index >= shot_frame + 2)
            done = true;
        ++frame_index;
    }

    gui::shutdown();
    ImGui_ImplDX11_Shutdown();
    ImGui_ImplWin32_Shutdown();
    ImGui::DestroyContext();

    gfx_destroy_surface(surface);
    gfx_destroy_shared(shared);
    ::DestroyWindow(hwnd);
    ::UnregisterClassW(wc.lpszClassName, wc.hInstance);
    return 0;
}
