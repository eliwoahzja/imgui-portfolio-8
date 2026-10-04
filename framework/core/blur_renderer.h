#pragma once

#include "imgui.h"

struct ID3D11Device;
struct ID3D11DeviceContext;

// Gaussian backdrop blur, DX11.
//
// The panel is a composited overlay, so there is no game frame to sample: the
// backdrop is a still image loaded from disk. That means the blur only has to
// run when the image (or its strength) changes, not every frame — downsample to
// a quarter, then alternate horizontal and vertical passes.
namespace blur_renderer
{
    bool init(ID3D11Device* device);
    void shutdown();

    // Loads the backdrop and builds its blurred copy. Returns false when the
    // file is missing, in which case the UI falls back to a flat fill.
    bool load_backdrop(ID3D11DeviceContext* ctx, const char* path);

    bool  ready();
    ImTextureID backdrop();          // the sharp image
    ImTextureID backdrop_blurred();  // the frosted copy drawn behind the panel
    ImVec2      backdrop_size();     // pixel size of the source image

    // Frosted glass for popups. Unlike the backdrop above this samples the
    // live frame: a draw-list callback snapshots everything rasterised so far
    // (panel, rows, other popups), blurs it through the same gaussian chain,
    // and an AddImageRounded right after the callback draws the result into
    // the given rect. Call it while building the popup's draw list, just
    // before its background fill; respects the current style alpha. No-op
    // when the pipeline is unavailable.
    void draw_live_blur(ImDrawList* dl, const ImVec2& min, const ImVec2& max, float rounding);
}
