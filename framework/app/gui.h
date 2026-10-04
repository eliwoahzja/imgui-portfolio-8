#pragma once

#include <windows.h>

struct ID3D11Device;
struct ID3D11DeviceContext;

namespace gui
{
    void initialize(ID3D11Device* device, ID3D11DeviceContext* context);
    void shutdown();
    void select_tab(int index);   // dev aid for --tab=N autoshots
    void set_shot_full(bool on);  // dev aid: capture the display, not just the panel
    // Top-left of the panel in client pixels, i.e. the origin the autoshot crop
    // uses. Dev aid: lets --click coordinates be read straight off a screenshot.
    void panel_origin(float& x, float& y);
    void prepare_frame();
    void render(HWND hwnd);
}
