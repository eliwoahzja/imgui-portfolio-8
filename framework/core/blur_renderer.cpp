#include "blur_renderer.h"

#include <d3d11.h>
#include <d3dcompiler.h>
#include <cstring>
#include <cstdio>

#include "../../thirdparty/stb/stb_image.h"

#pragma comment(lib, "d3dcompiler.lib")

namespace
{
    // Shader sources: fullscreen triangle, a 13-tap downsample and the two
    // separable Gaussian passes. Kept verbatim from the reference pipeline.
    const char* k_fullscreen_vs = R"(
struct VS_OUT {
    float4 pos : SV_POSITION;
    float2 uv  : TEXCOORD0;
};

VS_OUT main(uint id : SV_VertexID) {
    float2 uv = float2((id << 1) & 2, id & 2);
    float2 pos = uv * 2.0 - 1.0;

    VS_OUT o;
    o.pos = float4(pos.x, -pos.y, 0, 1);
    o.uv  = uv;
    return o;
}
)";

    const char* k_downsample_ps = R"(
Texture2D input_tex : register(t0);
SamplerState linear_sampler : register(s0);

cbuffer BlurParams : register(b2)
{
    float2 texel_size;
    float blur_strength;
    float _pad;
}

float4 main(float4 pos : SV_POSITION, float2 uv : TEXCOORD0) : SV_Target
{
    float4 result = 0.0;

    result += input_tex.Sample(linear_sampler, uv) * 0.125;

    float2 offset = texel_size;
    result += input_tex.Sample(linear_sampler, uv + float2(-offset.x, -offset.y)) * 0.125;
    result += input_tex.Sample(linear_sampler, uv + float2( offset.x, -offset.y)) * 0.125;
    result += input_tex.Sample(linear_sampler, uv + float2(-offset.x,  offset.y)) * 0.125;
    result += input_tex.Sample(linear_sampler, uv + float2( offset.x,  offset.y)) * 0.125;

    offset *= 0.5;
    result += input_tex.Sample(linear_sampler, uv + float2(-offset.x * 2, 0)) * 0.0625;
    result += input_tex.Sample(linear_sampler, uv + float2( offset.x * 2, 0)) * 0.0625;
    result += input_tex.Sample(linear_sampler, uv + float2(0, -offset.y * 2)) * 0.0625;
    result += input_tex.Sample(linear_sampler, uv + float2(0,  offset.y * 2)) * 0.0625;

    return float4(result.rgb, 1.0);
}
)";

    // One source for both directions: texel_size carries the axis, so the
    // horizontal pass gets (1/w, 0) and the vertical one (0, 1/h).
    const char* k_blur_ps = R"(
Texture2D input_tex : register(t0);
SamplerState linear_sampler : register(s0);

cbuffer BlurParams : register(b2)
{
    float2 texel_size;
    float blur_strength;
    float _pad;
}

static const float weights[9] = {
    0.13298, 0.23227, 0.1353, 0.0511, 0.01253, 0.00198, 0.00020, 0.00001, 0.0
};

float4 main(float4 pos : SV_POSITION, float2 uv : TEXCOORD0) : SV_Target
{
    float3 result = input_tex.Sample(linear_sampler, uv).rgb * weights[0];

    for (int i = 1; i < 9; ++i)
    {
        float2 offset = texel_size * (float)i * blur_strength;
        result += input_tex.Sample(linear_sampler, uv + offset).rgb * weights[i];
        result += input_tex.Sample(linear_sampler, uv - offset).rgb * weights[i];
    }

    return float4(result, 1.0);
}
)";

    struct BlurCB
    {
        float texel[2];
        float strength;
        float pad;
    };

    struct RenderTexture
    {
        ID3D11Texture2D*          tex = nullptr;
        ID3D11RenderTargetView*   rtv = nullptr;
        ID3D11ShaderResourceView* srv = nullptr;

        void release()
        {
            if (srv) { srv->Release(); srv = nullptr; }
            if (rtv) { rtv->Release(); rtv = nullptr; }
            if (tex) { tex->Release(); tex = nullptr; }
        }
    };

    ID3D11Device*             s_device      = nullptr;
    ID3D11VertexShader*       s_vs          = nullptr;
    ID3D11PixelShader*        s_ps_down     = nullptr;
    ID3D11PixelShader*        s_ps_blur     = nullptr;
    ID3D11Buffer*             s_cb          = nullptr;
    ID3D11SamplerState*       s_sampler     = nullptr;
    ID3D11BlendState*         s_blend       = nullptr;
    ID3D11RasterizerState*    s_raster      = nullptr;
    ID3D11DepthStencilState*  s_depth       = nullptr;

    ID3D11Texture2D*          s_src_tex     = nullptr;
    ID3D11ShaderResourceView* s_src_srv     = nullptr;
    RenderTexture             s_ping, s_pong;
    ImVec2                    s_size        = {};
    bool                      s_ready       = false;

    bool compile(const char* src, const char* target, ID3DBlob** blob)
    {
        ID3DBlob* error = nullptr;
        const HRESULT hr = D3DCompile(src, strlen(src), nullptr, nullptr, nullptr,
            "main", target, D3DCOMPILE_OPTIMIZATION_LEVEL3, 0, blob, &error);
        if (error) error->Release();
        return SUCCEEDED(hr);
    }

    bool make_render_texture(RenderTexture& out, UINT w, UINT h)
    {
        out.release();

        D3D11_TEXTURE2D_DESC desc = {};
        desc.Width          = w;
        desc.Height         = h;
        desc.MipLevels      = 1;
        desc.ArraySize      = 1;
        desc.Format         = DXGI_FORMAT_R8G8B8A8_UNORM;
        desc.SampleDesc.Count = 1;
        desc.Usage          = D3D11_USAGE_DEFAULT;
        desc.BindFlags      = D3D11_BIND_RENDER_TARGET | D3D11_BIND_SHADER_RESOURCE;

        if (FAILED(s_device->CreateTexture2D(&desc, nullptr, &out.tex)))          return false;
        if (FAILED(s_device->CreateRenderTargetView(out.tex, nullptr, &out.rtv))) return false;
        if (FAILED(s_device->CreateShaderResourceView(out.tex, nullptr, &out.srv))) return false;
        return true;
    }

    void set_params(ID3D11DeviceContext* ctx, float tx, float ty, float strength)
    {
        D3D11_MAPPED_SUBRESOURCE mapped = {};
        if (FAILED(ctx->Map(s_cb, 0, D3D11_MAP_WRITE_DISCARD, 0, &mapped)))
            return;
        BlurCB data = { { tx, ty }, strength, 0.f };
        memcpy(mapped.pData, &data, sizeof(data));
        ctx->Unmap(s_cb, 0);
    }

    void full_screen_pass(ID3D11DeviceContext* ctx, ID3D11RenderTargetView* rtv,
        ID3D11ShaderResourceView* src, ID3D11PixelShader* ps, UINT w, UINT h)
    {
        ID3D11ShaderResourceView* null_srv[1] = { nullptr };
        ctx->PSSetShaderResources(0, 1, null_srv);

        D3D11_VIEWPORT vp = {};
        vp.Width    = (float)w;
        vp.Height   = (float)h;
        vp.MaxDepth = 1.f;
        ctx->RSSetViewports(1, &vp);

        ctx->OMSetRenderTargets(1, &rtv, nullptr);
        ctx->PSSetShader(ps, nullptr, 0);
        ctx->PSSetShaderResources(0, 1, &src);
        ctx->Draw(3, 0);

        ctx->PSSetShaderResources(0, 1, null_srv);
    }

    void release_backdrop()
    {
        s_ping.release();
        s_pong.release();
        if (s_src_srv) { s_src_srv->Release(); s_src_srv = nullptr; }
        if (s_src_tex) { s_src_tex->Release(); s_src_tex = nullptr; }
        s_size  = {};
        s_ready = false;
    }

    // --- live popup blur ----------------------------------------------------
    // Resources for the frosted-popup path: a full-res copy of the back buffer
    // (flip-model buffers have no SHADER_RESOURCE bind, so they cannot be
    // sampled directly) plus a quarter-res ping-pong pair. Kept separate from
    // the backdrop chain above — s_ping holds the precomputed panel frost and
    // must survive between frames.
    RenderTexture             s_live_a, s_live_b;
    UINT                      s_live_dw = 0, s_live_dh = 0;
    ID3D11Texture2D*          s_capture_tex = nullptr;
    ID3D11ShaderResourceView* s_capture_srv = nullptr;
    D3D11_TEXTURE2D_DESC      s_capture_desc = {};

    void release_live()
    {
        s_live_a.release();
        s_live_b.release();
        if (s_capture_srv) { s_capture_srv->Release(); s_capture_srv = nullptr; }
        if (s_capture_tex) { s_capture_tex->Release(); s_capture_tex = nullptr; }
        s_live_dw = s_live_dh = 0;
        s_capture_desc = {};
    }

    // The ping-pong pair has to exist at submit time: AddImageRounded records
    // the SRV pointer immediately, the callback only fills the pixels later.
    bool ensure_live_targets(UINT dw, UINT dh)
    {
        if (s_live_a.srv && s_live_b.srv && s_live_dw == dw && s_live_dh == dh)
            return true;

        if (!make_render_texture(s_live_a, dw, dh) || !make_render_texture(s_live_b, dw, dh))
        {
            s_live_a.release();
            s_live_b.release();
            s_live_dw = s_live_dh = 0;
            return false;
        }
        s_live_dw = dw;
        s_live_dh = dh;
        return true;
    }

    bool ensure_capture(const D3D11_TEXTURE2D_DESC& bb)
    {
        if (s_capture_tex && s_capture_desc.Width == bb.Width &&
            s_capture_desc.Height == bb.Height && s_capture_desc.Format == bb.Format)
            return true;

        if (s_capture_srv) { s_capture_srv->Release(); s_capture_srv = nullptr; }
        if (s_capture_tex) { s_capture_tex->Release(); s_capture_tex = nullptr; }

        // CopyResource requires an identical format, so mirror the back buffer.
        D3D11_TEXTURE2D_DESC desc = {};
        desc.Width          = bb.Width;
        desc.Height         = bb.Height;
        desc.MipLevels      = 1;
        desc.ArraySize      = 1;
        desc.Format         = bb.Format;
        desc.SampleDesc.Count = 1;
        desc.Usage          = D3D11_USAGE_DEFAULT;
        desc.BindFlags      = D3D11_BIND_SHADER_RESOURCE;

        if (FAILED(s_device->CreateTexture2D(&desc, nullptr, &s_capture_tex)) ||
            FAILED(s_device->CreateShaderResourceView(s_capture_tex, nullptr, &s_capture_srv)))
        {
            if (s_capture_srv) { s_capture_srv->Release(); s_capture_srv = nullptr; }
            if (s_capture_tex) { s_capture_tex->Release(); s_capture_tex = nullptr; }
            s_capture_desc = {};
            return false;
        }
        s_capture_desc = desc;
        return true;
    }

    // Runs inside ImGui_ImplDX11_RenderDrawData, when everything submitted
    // before the popup is already rasterised into the back buffer. Snapshot
    // it, blur into s_live_a, then hand the render target back. The
    // ImDrawCallback_ResetRenderState queued right after restores ImGui's own
    // pipeline state; the render target binding is ours to put back.
    void live_blur_callback(const ImDrawList*, const ImDrawCmd*)
    {
        if (!s_device || !s_live_a.rtv || !s_live_b.rtv)
            return;

        ID3D11DeviceContext* ctx = nullptr;
        s_device->GetImmediateContext(&ctx);
        if (!ctx)
            return;

        ID3D11RenderTargetView* rtv = nullptr;
        ID3D11DepthStencilView* dsv = nullptr;
        ctx->OMGetRenderTargets(1, &rtv, &dsv);

        ID3D11Resource*  res = nullptr;
        ID3D11Texture2D* bb  = nullptr;
        if (rtv) rtv->GetResource(&res);
        if (res) res->QueryInterface(__uuidof(ID3D11Texture2D), (void**)&bb);

        if (bb)
        {
            D3D11_TEXTURE2D_DESC desc = {};
            bb->GetDesc(&desc);
            if (desc.SampleDesc.Count == 1 && ensure_capture(desc))
            {
                ctx->CopyResource(s_capture_tex, bb);

                UINT vp_count = 1;
                D3D11_VIEWPORT prev_vp = {};
                ctx->RSGetViewports(&vp_count, &prev_vp);

                ctx->IASetInputLayout(nullptr);
                ctx->IASetPrimitiveTopology(D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
                ID3D11Buffer* no_vb = nullptr;
                UINT zero = 0;
                ctx->IASetVertexBuffers(0, 1, &no_vb, &zero, &zero);
                ctx->VSSetShader(s_vs, nullptr, 0);
                ctx->GSSetShader(nullptr, nullptr, 0);
                ctx->PSSetSamplers(0, 1, &s_sampler);
                ctx->PSSetConstantBuffers(2, 1, &s_cb);
                ctx->RSSetState(s_raster);
                ctx->OMSetDepthStencilState(s_depth, 0);
                const float blend_factor[4] = { 0.f, 0.f, 0.f, 0.f };
                ctx->OMSetBlendState(s_blend, blend_factor, 0xFFFFFFFF);

                set_params(ctx, 1.f / (float)desc.Width, 1.f / (float)desc.Height, 1.f);
                full_screen_pass(ctx, s_live_a.rtv, s_capture_srv, s_ps_down, s_live_dw, s_live_dh);

                // Figma popups spec "background blur 12.5" — two ping-pong
                // rounds at quarter res land close enough. The result ends in
                // s_live_a, the texture the popup referenced at submit time.
                const float texel_x = 1.f / (float)s_live_dw;
                const float texel_y = 1.f / (float)s_live_dh;
                for (int i = 0; i < 2; ++i)
                {
                    const float strength = 1.5f + (float)i * 0.9f;
                    set_params(ctx, texel_x, 0.f, strength);
                    full_screen_pass(ctx, s_live_b.rtv, s_live_a.srv, s_ps_blur, s_live_dw, s_live_dh);
                    set_params(ctx, 0.f, texel_y, strength);
                    full_screen_pass(ctx, s_live_a.rtv, s_live_b.srv, s_ps_blur, s_live_dw, s_live_dh);
                }

                ctx->OMSetRenderTargets(1, &rtv, dsv);
                if (vp_count)
                    ctx->RSSetViewports(1, &prev_vp);
            }
        }

        if (bb)  bb->Release();
        if (res) res->Release();
        if (dsv) dsv->Release();
        if (rtv) rtv->Release();
        ctx->Release();
    }
}

namespace blur_renderer
{
    bool init(ID3D11Device* device)
    {
        shutdown();
        if (!device)
            return false;

        s_device = device;
        s_device->AddRef();

        ID3DBlob* vs_blob = nullptr;
        ID3DBlob* down_blob = nullptr;
        ID3DBlob* blur_blob = nullptr;
        bool ok = compile(k_fullscreen_vs, "vs_4_0", &vs_blob)
               && compile(k_downsample_ps, "ps_4_0", &down_blob)
               && compile(k_blur_ps,       "ps_4_0", &blur_blob);

        if (ok)
        {
            ok = SUCCEEDED(s_device->CreateVertexShader(vs_blob->GetBufferPointer(), vs_blob->GetBufferSize(), nullptr, &s_vs))
              && SUCCEEDED(s_device->CreatePixelShader(down_blob->GetBufferPointer(), down_blob->GetBufferSize(), nullptr, &s_ps_down))
              && SUCCEEDED(s_device->CreatePixelShader(blur_blob->GetBufferPointer(), blur_blob->GetBufferSize(), nullptr, &s_ps_blur));
        }

        if (vs_blob)   vs_blob->Release();
        if (down_blob) down_blob->Release();
        if (blur_blob) blur_blob->Release();
        if (!ok)
            return false;

        D3D11_BUFFER_DESC cb = {};
        cb.Usage          = D3D11_USAGE_DYNAMIC;
        cb.BindFlags      = D3D11_BIND_CONSTANT_BUFFER;
        cb.CPUAccessFlags = D3D11_CPU_ACCESS_WRITE;
        cb.ByteWidth      = sizeof(BlurCB);
        if (FAILED(s_device->CreateBuffer(&cb, nullptr, &s_cb)))
            return false;

        // Clamp so the taps at the border do not wrap the image around.
        D3D11_SAMPLER_DESC samp = {};
        samp.Filter   = D3D11_FILTER_MIN_MAG_MIP_LINEAR;
        samp.AddressU = D3D11_TEXTURE_ADDRESS_CLAMP;
        samp.AddressV = D3D11_TEXTURE_ADDRESS_CLAMP;
        samp.AddressW = D3D11_TEXTURE_ADDRESS_CLAMP;
        samp.ComparisonFunc = D3D11_COMPARISON_ALWAYS;
        samp.MaxLOD   = D3D11_FLOAT32_MAX;
        if (FAILED(s_device->CreateSamplerState(&samp, &s_sampler)))
            return false;

        D3D11_BLEND_DESC blend = {};
        blend.RenderTarget[0].BlendEnable = FALSE;
        blend.RenderTarget[0].RenderTargetWriteMask = D3D11_COLOR_WRITE_ENABLE_ALL;
        if (FAILED(s_device->CreateBlendState(&blend, &s_blend)))
            return false;

        D3D11_RASTERIZER_DESC raster = {};
        raster.FillMode = D3D11_FILL_SOLID;
        raster.CullMode = D3D11_CULL_NONE;
        raster.DepthClipEnable = TRUE;
        if (FAILED(s_device->CreateRasterizerState(&raster, &s_raster)))
            return false;

        D3D11_DEPTH_STENCIL_DESC depth = {};
        depth.DepthEnable   = FALSE;
        depth.StencilEnable = FALSE;
        return SUCCEEDED(s_device->CreateDepthStencilState(&depth, &s_depth));
    }

    void shutdown()
    {
        release_backdrop();
        release_live();
        if (s_depth)   { s_depth->Release();   s_depth   = nullptr; }
        if (s_raster)  { s_raster->Release();  s_raster  = nullptr; }
        if (s_blend)   { s_blend->Release();   s_blend   = nullptr; }
        if (s_sampler) { s_sampler->Release(); s_sampler = nullptr; }
        if (s_cb)      { s_cb->Release();      s_cb      = nullptr; }
        if (s_ps_blur) { s_ps_blur->Release(); s_ps_blur = nullptr; }
        if (s_ps_down) { s_ps_down->Release(); s_ps_down = nullptr; }
        if (s_vs)      { s_vs->Release();      s_vs      = nullptr; }
        if (s_device)  { s_device->Release();  s_device  = nullptr; }
    }

    bool load_backdrop(ID3D11DeviceContext* ctx, const char* path)
    {
        if (!s_device || !ctx || !path)
            return false;

        release_backdrop();

        int w = 0, h = 0, channels = 0;
        unsigned char* pixels = stbi_load(path, &w, &h, &channels, 4);
        if (!pixels)
            return false;

        D3D11_TEXTURE2D_DESC desc = {};
        desc.Width          = (UINT)w;
        desc.Height         = (UINT)h;
        desc.MipLevels      = 1;
        desc.ArraySize      = 1;
        desc.Format         = DXGI_FORMAT_R8G8B8A8_UNORM;
        desc.SampleDesc.Count = 1;
        desc.Usage          = D3D11_USAGE_IMMUTABLE;
        desc.BindFlags      = D3D11_BIND_SHADER_RESOURCE;

        D3D11_SUBRESOURCE_DATA init = {};
        init.pSysMem     = pixels;
        init.SysMemPitch = (UINT)w * 4;

        const bool tex_ok = SUCCEEDED(s_device->CreateTexture2D(&desc, &init, &s_src_tex))
                         && SUCCEEDED(s_device->CreateShaderResourceView(s_src_tex, nullptr, &s_src_srv));
        stbi_image_free(pixels);
        if (!tex_ok)
        {
            release_backdrop();
            return false;
        }

        s_size = { (float)w, (float)h };

        // Quarter-res working buffers: the blur radius is measured in those
        // texels, so a /4 chain is both cheaper and four times wider.
        const UINT dw = (UINT)(w / 4) > 0 ? (UINT)(w / 4) : 1u;
        const UINT dh = (UINT)(h / 4) > 0 ? (UINT)(h / 4) : 1u;
        if (!make_render_texture(s_ping, dw, dh) || !make_render_texture(s_pong, dw, dh))
        {
            release_backdrop();
            return false;
        }

        // Save whatever the caller had bound — this runs mid-startup.
        ID3D11RenderTargetView* prev_rtv = nullptr;
        ID3D11DepthStencilView* prev_dsv = nullptr;
        ctx->OMGetRenderTargets(1, &prev_rtv, &prev_dsv);
        UINT vp_count = 1;
        D3D11_VIEWPORT prev_vp = {};
        ctx->RSGetViewports(&vp_count, &prev_vp);

        ctx->IASetInputLayout(nullptr);
        ctx->IASetPrimitiveTopology(D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
        ID3D11Buffer* no_vb = nullptr;
        UINT zero = 0;
        ctx->IASetVertexBuffers(0, 1, &no_vb, &zero, &zero);
        ctx->VSSetShader(s_vs, nullptr, 0);
        ctx->GSSetShader(nullptr, nullptr, 0);
        ctx->PSSetSamplers(0, 1, &s_sampler);
        ctx->PSSetConstantBuffers(2, 1, &s_cb);
        ctx->RSSetState(s_raster);
        ctx->OMSetDepthStencilState(s_depth, 0);
        const float blend_factor[4] = { 0.f, 0.f, 0.f, 0.f };
        ctx->OMSetBlendState(s_blend, blend_factor, 0xFFFFFFFF);

        set_params(ctx, 1.f / (float)w, 1.f / (float)h, 1.f);
        full_screen_pass(ctx, s_ping.rtv, s_src_srv, s_ps_down, dw, dh);

        // Four ping-pong iterations put the frosted-glass look roughly where
        // the design has it without needing a bigger kernel.
        const float texel_x = 1.f / (float)dw;
        const float texel_y = 1.f / (float)dh;
        for (int i = 0; i < 4; ++i)
        {
            const float strength = 1.6f + (float)i * 0.9f;
            set_params(ctx, texel_x, 0.f, strength);
            full_screen_pass(ctx, s_pong.rtv, s_ping.srv, s_ps_blur, dw, dh);
            set_params(ctx, 0.f, texel_y, strength);
            full_screen_pass(ctx, s_ping.rtv, s_pong.srv, s_ps_blur, dw, dh);
        }

        ctx->OMSetRenderTargets(1, &prev_rtv, prev_dsv);
        if (prev_rtv) prev_rtv->Release();
        if (prev_dsv) prev_dsv->Release();
        if (vp_count) ctx->RSSetViewports(1, &prev_vp);

        s_ready = true;
        return true;
    }

    bool ready()
    {
        return s_ready;
    }

    ImTextureID backdrop()
    {
        return s_ready ? (ImTextureID)s_src_srv : (ImTextureID)0;
    }

    ImTextureID backdrop_blurred()
    {
        return s_ready ? (ImTextureID)s_ping.srv : (ImTextureID)0;
    }

    ImVec2 backdrop_size()
    {
        return s_size;
    }

    void draw_live_blur(ImDrawList* dl, const ImVec2& min, const ImVec2& max, float rounding)
    {
        // Needs the shader chain from init(); the disk backdrop is irrelevant here.
        if (!dl || !s_device || !s_vs || !s_ps_down || !s_ps_blur || !s_cb)
            return;
        if (max.x <= min.x || max.y <= min.y)
            return;

        const ImVec2 display = ImGui::GetIO().DisplaySize;
        if (display.x < 1.f || display.y < 1.f)
            return;

        const UINT dw = (UINT)(display.x / 4.f) > 0 ? (UINT)(display.x / 4.f) : 1u;
        const UINT dh = (UINT)(display.y / 4.f) > 0 ? (UINT)(display.y / 4.f) : 1u;
        if (!ensure_live_targets(dw, dh))
            return;

        // The popup rect maps straight onto the back buffer, so plain
        // screen-space UVs pick out the pixels the popup covers.
        const auto saturate = [](float v) { return v < 0.f ? 0.f : (v > 1.f ? 1.f : v); };
        const ImVec2 uv0 = { saturate(min.x / display.x), saturate(min.y / display.y) };
        const ImVec2 uv1 = { saturate(max.x / display.x), saturate(max.y / display.y) };

        // Snapshot + blur happens between the two callbacks; the image command
        // in the middle then samples the freshly blurred copy. GetColorU32
        // folds the current style alpha in, so the frost fades with its popup.
        dl->AddCallback(live_blur_callback, nullptr);
        dl->AddCallback(ImDrawCallback_ResetRenderState, nullptr);
        dl->AddImageRounded((ImTextureID)s_live_a.srv, min, max, uv0, uv1,
            ImGui::GetColorU32(ImVec4(1.f, 1.f, 1.f, 1.f)), rounding);
    }
}
