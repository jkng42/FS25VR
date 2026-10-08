#include "blit.h"
#include "log.h"
#include <d3dcompiler.h>
#include <cmath>

static const char* kShader = R"(
cbuffer C : register(b0)
{
    float2 scale; float decode; float smoothing;           // scale: 1 / source size; smoothing: blended edge
    float2 cursorPos; float cursorUnit; float cursorOn;   // cursor tip in source pixels, size of one arrow unit
    float4 rect;                                          // destination rect (x, y, w, h) the source fills
};
Texture2D<float4> src : register(t0);
SamplerState smp : register(s0);

struct V { float4 pos : SV_Position; };

V vs(uint id : SV_VertexID)
{
    V o;
    float2 uv = float2((id << 1) & 2, id & 2);
    o.pos = float4(uv * float2(2, -2) + float2(-1, 1), 0, 1);
    return o;
}

float3 SrgbToLinear(float3 c)
{
    return c <= 0.04045 ? c / 12.92 : pow((c + 0.055) / 1.055, 2.4);
}

// Classic arrow pointer, tip at the origin, in 12x19 "arrow units".
static const float2 kArrow[7] = {
    float2(0, 0), float2(0, 16), float2(4, 12.5), float2(7, 19), float2(9.5, 18), float2(6.5, 11.5), float2(11.5, 11.5)
};

bool InArrow(float2 q)
{
    bool inside = false;
    [unroll] for (int a = 0, b = 6; a < 7; b = a++) {
        float2 pa = kArrow[a], pb = kArrow[b];
        if (((pa.y > q.y) != (pb.y > q.y)) && (q.x < (pb.x - pa.x) * (q.y - pa.y) / (pb.y - pa.y + 1e-6) + pa.x))
            inside = !inside;
    }
    return inside;
}

float4 ps(V i) : SV_Target
{
    float2 uv = (i.pos.xy - rect.xy) / rect.zw;
    float2 sp = uv / scale;
    // a smaller target: the average of all source texels in the target pixel (up to 4x4), not one
    // bilinear sample (which skips texels and keeps their aliasing)
    int2 n = clamp((int2)ceil(1 / (scale * rect.zw) - 0.01), 1, 4);
    float2 step = 1 / (rect.zw * n);
    float2 first = uv - 0.5 / rect.zw + 0.5 * step;
    float4 c = 0;
    for (int y = 0; y < n.y; y++)
        for (int x = 0; x < n.x; x++) c += src.SampleLevel(smp, first + float2(x, y) * step, 0);
    c /= n.x * n.y;
    c.rgb = saturate(c.rgb);
    // the focus view laid over the eye's image: fades out towards its edges
    float a = 1;
    if (smoothing > 0) {
        float2 e = smoothstep((float2)0, (float2)smoothing, uv) * smoothstep((float2)0, (float2)smoothing, 1 - uv);
        a = e.x * e.y;
    }
    if (cursorOn > 0.5) {
        float2 q = (sp - cursorPos) / cursorUnit;
        if (q.x > -2 && q.y > -2 && q.x < 14 && q.y < 21) {
            if (InArrow(q)) {
                c.rgb = 1;  // white body (display-referred, so 1 stays 1 after decoding)
            } else if (InArrow(q + float2(1.2, 0)) || InArrow(q - float2(1.2, 0)) ||
                       InArrow(q + float2(0, 1.2)) || InArrow(q - float2(0, 1.2))) {
                c.rgb = 0;  // black outline
            }
        }
    }
    if (decode > 0.5) c.rgb = SrgbToLinear(c.rgb);
    return float4(c.rgb, a);
}
)";

// A flat panel in front of the eye (the game's HUD placed in 3D): every target pixel's ray from the
// eye is intersected with the panel's plane, so both eyes see it at its real distance.
static const char* kPanelShader = R"(
cbuffer C : register(b0)
{
    float4 tans;     // tangents of the eye image's frustum: left, right, up, down
    float4 eyeDec;   // eye position in head space (x, y, z), target is sRGB
    float4 panel;    // panel centre (x, y) and half size (w, h) in head space, on the plane z = -dist
    float4 misc;     // dist, unused, target size (w, h)
};
Texture2D<float4> src : register(t0);
SamplerState smp : register(s0);

struct V { float4 pos : SV_Position; };

V vs(uint id : SV_VertexID)
{
    V o;
    float2 uv = float2((id << 1) & 2, id & 2);
    o.pos = float4(uv * float2(2, -2) + float2(-1, 1), 0, 1);
    return o;
}

float3 LinearToSrgb(float3 c)
{
    return c <= 0.0031308 ? c * 12.92 : 1.055 * pow(c, 1 / 2.4) - 0.055;
}

float4 ps(V i) : SV_Target
{
    float2 t = i.pos.xy / misc.zw;
    float2 dir = float2(lerp(tans.x, tans.y, t.x), lerp(tans.z, tans.w, t.y));
    float2 p = eyeDec.xy + (misc.x + eyeDec.z) * dir;
    float2 uv = float2((p.x - panel.x) / (2 * panel.z) + 0.5, 0.5 - (p.y - panel.y) / (2 * panel.w));
    if (any(uv < 0) || any(uv > 1)) discard;
    // the game's HUD: sRGB view (linear values), premultiplied colours, alpha = how much of the scene
    // shows through
    float4 c = src.SampleLevel(smp, uv, 0);
    c.a = 1 - c.a;
    if (eyeDec.w < 0.5) c.rgb = c.a > 0 ? LinearToSrgb(saturate(c.rgb / c.a)) * c.a : float3(0, 0, 0);
    return c;
}
)";

bool IsSrgbFormat(DXGI_FORMAT f)
{
    return f == DXGI_FORMAT_R8G8B8A8_UNORM_SRGB || f == DXGI_FORMAT_B8G8R8A8_UNORM_SRGB ||
           f == DXGI_FORMAT_B8G8R8X8_UNORM_SRGB;
}

using PFN_D3DCompile = HRESULT(WINAPI*)(LPCVOID, SIZE_T, LPCSTR, const D3D_SHADER_MACRO*, ID3DInclude*, LPCSTR,
                                        LPCSTR, UINT, UINT, ID3DBlob**, ID3DBlob**);

static ID3DBlob* Compile(PFN_D3DCompile compile, const char* entry, const char* target, const char* source = kShader)
{
    ID3DBlob* code = nullptr;
    ID3DBlob* err = nullptr;
    HRESULT hr = compile(source, strlen(source), "fs25vr_blit", nullptr, nullptr, entry, target,
                         D3DCOMPILE_OPTIMIZATION_LEVEL3, 0, &code, &err);
    if (FAILED(hr)) {
        Log("blit: shader %s failed: %s", entry, err ? (const char*)err->GetBufferPointer() : "?");
        if (err) err->Release();
        return nullptr;
    }
    if (err) err->Release();
    return code;
}

bool Blitter::Init(ID3D12Device* device, DXGI_FORMAT dstFormat)
{
    Shutdown();
    m_device = device;
    m_decodeSrgb = IsSrgbFormat(dstFormat);

    HMODULE dc = LoadLibraryW(L"d3dcompiler_47.dll");
    auto compile = dc ? (PFN_D3DCompile)GetProcAddress(dc, "D3DCompile") : nullptr;
    if (!compile) {
        Log("blit: d3dcompiler_47.dll not available");
        return false;
    }
    ID3DBlob* vs = Compile(compile, "vs", "vs_5_0");
    ID3DBlob* ps = Compile(compile, "ps", "ps_5_0");
    ID3DBlob* panelPs = Compile(compile, "ps", "ps_5_0", kPanelShader);
    if (!vs || !ps || !panelPs) return false;

    D3D12_DESCRIPTOR_RANGE range = {};
    range.RangeType = D3D12_DESCRIPTOR_RANGE_TYPE_SRV;
    range.NumDescriptors = 1;
    D3D12_ROOT_PARAMETER params[2] = {};
    params[0].ParameterType = D3D12_ROOT_PARAMETER_TYPE_32BIT_CONSTANTS;
    params[0].Constants.Num32BitValues = 16;
    params[0].ShaderVisibility = D3D12_SHADER_VISIBILITY_PIXEL;
    params[1].ParameterType = D3D12_ROOT_PARAMETER_TYPE_DESCRIPTOR_TABLE;
    params[1].DescriptorTable.NumDescriptorRanges = 1;
    params[1].DescriptorTable.pDescriptorRanges = &range;
    params[1].ShaderVisibility = D3D12_SHADER_VISIBILITY_PIXEL;
    D3D12_STATIC_SAMPLER_DESC sampler = {};
    sampler.Filter = D3D12_FILTER_MIN_MAG_MIP_LINEAR;
    sampler.AddressU = sampler.AddressV = sampler.AddressW = D3D12_TEXTURE_ADDRESS_MODE_CLAMP;
    sampler.MaxLOD = D3D12_FLOAT32_MAX;
    sampler.ShaderVisibility = D3D12_SHADER_VISIBILITY_PIXEL;
    D3D12_ROOT_SIGNATURE_DESC rsd = {};
    rsd.NumParameters = 2;
    rsd.pParameters = params;
    rsd.NumStaticSamplers = 1;
    rsd.pStaticSamplers = &sampler;
    ID3DBlob* rsBlob = nullptr;
    ID3DBlob* rsErr = nullptr;
    if (FAILED(D3D12SerializeRootSignature(&rsd, D3D_ROOT_SIGNATURE_VERSION_1, &rsBlob, &rsErr))) {
        Log("blit: root signature serialize failed");
        return false;
    }
    HRESULT hr = device->CreateRootSignature(0, rsBlob->GetBufferPointer(), rsBlob->GetBufferSize(),
                                             IID_PPV_ARGS(&m_root));
    rsBlob->Release();
    if (FAILED(hr)) {
        Log("blit: CreateRootSignature failed 0x%08x", hr);
        return false;
    }

    D3D12_GRAPHICS_PIPELINE_STATE_DESC pd = {};
    pd.pRootSignature = m_root;
    pd.VS = {vs->GetBufferPointer(), vs->GetBufferSize()};
    pd.PS = {ps->GetBufferPointer(), ps->GetBufferSize()};
    pd.BlendState.RenderTarget[0].RenderTargetWriteMask = D3D12_COLOR_WRITE_ENABLE_ALL;
    pd.SampleMask = 0xffffffff;
    pd.RasterizerState.FillMode = D3D12_FILL_MODE_SOLID;
    pd.RasterizerState.CullMode = D3D12_CULL_MODE_NONE;
    pd.RasterizerState.DepthClipEnable = TRUE;
    pd.PrimitiveTopologyType = D3D12_PRIMITIVE_TOPOLOGY_TYPE_TRIANGLE;
    pd.NumRenderTargets = 1;
    pd.RTVFormats[0] = dstFormat;
    pd.SampleDesc.Count = 1;
    hr = device->CreateGraphicsPipelineState(&pd, IID_PPV_ARGS(&m_pso));
    if (SUCCEEDED(hr)) {
        // the same, alpha-blended over the target (focus views)
        D3D12_RENDER_TARGET_BLEND_DESC& b = pd.BlendState.RenderTarget[0];
        b.BlendEnable = TRUE;
        b.SrcBlend = D3D12_BLEND_SRC_ALPHA;
        b.DestBlend = D3D12_BLEND_INV_SRC_ALPHA;
        b.BlendOp = D3D12_BLEND_OP_ADD;
        b.SrcBlendAlpha = D3D12_BLEND_ONE;
        b.DestBlendAlpha = D3D12_BLEND_ZERO;
        b.BlendOpAlpha = D3D12_BLEND_OP_ADD;
        hr = device->CreateGraphicsPipelineState(&pd, IID_PPV_ARGS(&m_psoBlend));
    }
    if (SUCCEEDED(hr)) {
        // the panel: premultiplied alpha over the target, the target's alpha kept
        D3D12_RENDER_TARGET_BLEND_DESC& b = pd.BlendState.RenderTarget[0];
        b.SrcBlend = D3D12_BLEND_ONE;
        b.DestBlend = D3D12_BLEND_INV_SRC_ALPHA;
        b.SrcBlendAlpha = D3D12_BLEND_ZERO;
        b.DestBlendAlpha = D3D12_BLEND_ONE;
        pd.PS = {panelPs->GetBufferPointer(), panelPs->GetBufferSize()};
        hr = device->CreateGraphicsPipelineState(&pd, IID_PPV_ARGS(&m_psoPanel));
    }
    vs->Release();
    ps->Release();
    panelPs->Release();
    if (FAILED(hr)) {
        Log("blit: CreateGraphicsPipelineState failed 0x%08x", hr);
        return false;
    }

    D3D12_DESCRIPTOR_HEAP_DESC hd = {};
    hd.Type = D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV;
    hd.NumDescriptors = 16;
    hd.Flags = D3D12_DESCRIPTOR_HEAP_FLAG_SHADER_VISIBLE;
    if (FAILED(device->CreateDescriptorHeap(&hd, IID_PPV_ARGS(&m_srvHeap)))) {
        Log("blit: CreateDescriptorHeap failed");
        return false;
    }
    m_srvInc = device->GetDescriptorHandleIncrementSize(D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV);
    Log("blit: ready (dst format %d, decode sRGB %d)", dstFormat, m_decodeSrgb);
    return true;
}

void Blitter::Shutdown()
{
    if (m_pso) m_pso->Release(), m_pso = nullptr;
    if (m_psoBlend) m_psoBlend->Release(), m_psoBlend = nullptr;
    if (m_psoPanel) m_psoPanel->Release(), m_psoPanel = nullptr;
    if (m_root) m_root->Release(), m_root = nullptr;
    if (m_srvHeap) m_srvHeap->Release(), m_srvHeap = nullptr;
}

D3D12_GPU_DESCRIPTOR_HANDLE Blitter::SrvFor(ID3D12Resource* src, DXGI_FORMAT srcFormat)
{
    // Ring of 16 descriptors; at most a few blits are in flight at once.
    UINT slot = m_srvNext++ % 16;
    D3D12_CPU_DESCRIPTOR_HANDLE cpu = m_srvHeap->GetCPUDescriptorHandleForHeapStart();
    D3D12_GPU_DESCRIPTOR_HANDLE gpu = m_srvHeap->GetGPUDescriptorHandleForHeapStart();
    cpu.ptr += (SIZE_T)slot * m_srvInc;
    gpu.ptr += (UINT64)slot * m_srvInc;

    D3D12_SHADER_RESOURCE_VIEW_DESC sd = {};
    sd.Format = srcFormat;
    sd.ViewDimension = D3D12_SRV_DIMENSION_TEXTURE2D;
    sd.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
    sd.Texture2D.MipLevels = 1;
    m_device->CreateShaderResourceView(src, &sd, cpu);
    return gpu;
}

void Blitter::RecordPanel(ID3D12GraphicsCommandList* cl, ID3D12Resource* src, DXGI_FORMAT srcFormat,
                          D3D12_CPU_DESCRIPTOR_HANDLE rtv, UINT dstW, UINT dstH, const PanelDraw& p)
{
    D3D12_GPU_DESCRIPTOR_HANDLE gpu = SrvFor(src, srcFormat);
    float consts[16] = {p.tanLeft, p.tanRight, p.tanUp, p.tanDown,
                        p.eye[0], p.eye[1], p.eye[2], m_decodeSrgb ? 1.0f : 0.0f,
                        p.x, p.y, 0.5f * p.w, 0.5f * p.h,
                        p.distance, 0, (float)dstW, (float)dstH};
    cl->SetGraphicsRootSignature(m_root);
    cl->SetPipelineState(m_psoPanel);
    cl->SetDescriptorHeaps(1, &m_srvHeap);
    cl->SetGraphicsRoot32BitConstants(0, 16, consts, 0);
    cl->SetGraphicsRootDescriptorTable(1, gpu);
    D3D12_VIEWPORT vp = {0, 0, (float)dstW, (float)dstH, 0, 1};
    D3D12_RECT sc = {0, 0, (LONG)dstW, (LONG)dstH};
    cl->RSSetViewports(1, &vp);
    cl->RSSetScissorRects(1, &sc);
    cl->OMSetRenderTargets(1, &rtv, FALSE, nullptr);
    cl->IASetPrimitiveTopology(D3D_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
    cl->DrawInstanced(3, 1, 0, 0);
}

void Blitter::Record(ID3D12GraphicsCommandList* cl, ID3D12Resource* src, DXGI_FORMAT srcFormat,
                     D3D12_CPU_DESCRIPTOR_HANDLE rtv, UINT dstW, UINT dstH, const CursorDraw* cursor,
                     const BlitRect* over)
{
    D3D12_GPU_DESCRIPTOR_HANDLE gpu = SrvFor(src, srcFormat);

    D3D12_RESOURCE_DESC srcDesc = src->GetDesc();
    float rx = over ? over->x : 0, ry = over ? over->y : 0;
    float rw = over ? over->w : (float)dstW, rh = over ? over->h : (float)dstH;
    float consts[12] = {1.0f / (float)srcDesc.Width, 1.0f / (float)srcDesc.Height, m_decodeSrgb ? 1.0f : 0.0f,
                        over ? over->smoothing : 0.0f, 0, 0, 1, 0, rx, ry, rw, rh};
    if (cursor && cursor->visible) {
        consts[4] = cursor->x;
        consts[5] = cursor->y;
        consts[6] = cursor->unit;
        consts[7] = 1.0f;
    }

    cl->SetGraphicsRootSignature(m_root);
    cl->SetPipelineState(over ? m_psoBlend : m_pso);
    cl->SetDescriptorHeaps(1, &m_srvHeap);
    cl->SetGraphicsRoot32BitConstants(0, 12, consts, 0);
    cl->SetGraphicsRootDescriptorTable(1, gpu);
    D3D12_VIEWPORT vp = {rx, ry, rw, rh, 0, 1};
    D3D12_RECT sc = {(LONG)rx, (LONG)ry, (LONG)ceilf(rx + rw), (LONG)ceilf(ry + rh)};
    cl->RSSetViewports(1, &vp);
    cl->RSSetScissorRects(1, &sc);
    cl->OMSetRenderTargets(1, &rtv, FALSE, nullptr);
    cl->IASetPrimitiveTopology(D3D_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
    cl->DrawInstanced(3, 1, 0, 0);
}
