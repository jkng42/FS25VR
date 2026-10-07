#include "overlay.h"
#include "log.h"

#include <windows.h>
#include <mutex>
#include <unordered_map>

namespace overlay {
namespace {

// vtable slots (d3d12.h declaration order)
constexpr int kDevCreateRenderTargetView = 20;
constexpr int kClResourceBarrier = 26;

using PFN_CreateRTV = void(STDMETHODCALLTYPE*)(ID3D12Device*, ID3D12Resource*, const D3D12_RENDER_TARGET_VIEW_DESC*,
                                              D3D12_CPU_DESCRIPTOR_HANDLE);
using PFN_Barrier = void(STDMETHODCALLTYPE*)(ID3D12GraphicsCommandList*, UINT, const D3D12_RESOURCE_BARRIER*);

PFN_CreateRTV o_CreateRTV = nullptr;
PFN_Barrier   o_Barrier = nullptr;

constexpr D3D12_RESOURCE_STATES kReadable =
    D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE | D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE;

struct Info {
    D3D12_RESOURCE_DESC   desc{};
    DXGI_FORMAT           viewFormat = DXGI_FORMAT_UNKNOWN;
    D3D12_RESOURCE_STATES state = D3D12_RESOURCE_STATE_COMMON;
};

std::mutex g_mtx;
std::unordered_map<ID3D12Resource*, Info> g_targets;  // render targets at the overlay's size
UINT            g_w = 0, g_h = 0;
ID3D12Resource* g_frameImage = nullptr;  // last made readable in the frame being recorded
ID3D12Resource* g_image = nullptr;       // ... in the frame being presented
Info            g_imageInfo;
ID3D12Resource* g_locked = nullptr;      // fixed output (plane stereo)
bool            g_installed = false;

bool Patch(void** slot, void* value, void** original)
{
    if (*slot == value) return true;
    DWORD old;
    if (!VirtualProtect(slot, sizeof(void*), PAGE_READWRITE, &old)) return false;
    if (original && !*original) *original = *slot;
    *slot = value;
    VirtualProtect(slot, sizeof(void*), old, &old);
    return true;
}

bool Is8Bit(DXGI_FORMAT f)
{
    switch (f) {
    case DXGI_FORMAT_R8G8B8A8_TYPELESS: case DXGI_FORMAT_R8G8B8A8_UNORM: case DXGI_FORMAT_R8G8B8A8_UNORM_SRGB:
    case DXGI_FORMAT_B8G8R8A8_TYPELESS: case DXGI_FORMAT_B8G8R8A8_UNORM: case DXGI_FORMAT_B8G8R8A8_UNORM_SRGB:
    case DXGI_FORMAT_R10G10B10A2_TYPELESS: case DXGI_FORMAT_R10G10B10A2_UNORM:
        return true;
    default:
        return false;
    }
}

void STDMETHODCALLTYPE Hook_CreateRTV(ID3D12Device* self, ID3D12Resource* res, const D3D12_RENDER_TARGET_VIEW_DESC* d,
                                      D3D12_CPU_DESCRIPTOR_HANDLE h)
{
    o_CreateRTV(self, res, d, h);
    if (!res) return;
    std::lock_guard<std::mutex> lock(g_mtx);
    if (!g_w || g_targets.count(res)) return;
    D3D12_RESOURCE_DESC rd = res->GetDesc();
    if (rd.Dimension != D3D12_RESOURCE_DIMENSION_TEXTURE2D || rd.Width != g_w || rd.Height != g_h ||
        rd.SampleDesc.Count != 1)
        return;
    Info info;
    info.desc = rd;
    info.viewFormat = d && d->Format != DXGI_FORMAT_UNKNOWN ? d->Format : rd.Format;
    g_targets[res] = info;
    Log("overlay: render target %p %llux%u fmt %d (view %d)", res, rd.Width, rd.Height, rd.Format, info.viewFormat);
}

void STDMETHODCALLTYPE Hook_Barrier(ID3D12GraphicsCommandList* self, UINT n, const D3D12_RESOURCE_BARRIER* b)
{
    o_Barrier(self, n, b);
    std::lock_guard<std::mutex> lock(g_mtx);
    if (g_targets.empty()) return;
    for (UINT i = 0; i < n; i++) {
        if (b[i].Type != D3D12_RESOURCE_BARRIER_TYPE_TRANSITION) continue;
        auto it = g_targets.find(b[i].Transition.pResource);
        if (it == g_targets.end()) continue;
        it->second.state = b[i].Transition.StateAfter;
        // the final image is 8-bit (tonemapped); HDR targets of the overlay come before it
        if ((b[i].Transition.StateAfter & kReadable) && Is8Bit(it->second.viewFormat)) g_frameImage = it->first;
    }
}

} // namespace

void Install(ID3D12Device* device)
{
    if (g_installed || !device) return;
    g_installed = true;
    void** dvt = *(void***)device;
    Patch(&dvt[kDevCreateRenderTargetView], (void*)Hook_CreateRTV, (void**)&o_CreateRTV);
    ID3D12CommandAllocator* alloc = nullptr;
    ID3D12GraphicsCommandList* cl = nullptr;
    if (SUCCEEDED(device->CreateCommandAllocator(D3D12_COMMAND_LIST_TYPE_DIRECT, IID_PPV_ARGS(&alloc))) &&
        SUCCEEDED(device->CreateCommandList(0, D3D12_COMMAND_LIST_TYPE_DIRECT, alloc, nullptr, IID_PPV_ARGS(&cl)))) {
        void** cvt = *(void***)cl;
        Patch(&cvt[kClResourceBarrier], (void*)Hook_Barrier, (void**)&o_Barrier);
        cl->Close();
    }
    if (cl) cl->Release();
    if (alloc) alloc->Release();
    Log("overlay: hooks installed (CreateRenderTargetView %d, ResourceBarrier %d)", o_CreateRTV != nullptr,
        o_Barrier != nullptr);
}

void SetSize(UINT w, UINT h)
{
    std::lock_guard<std::mutex> lock(g_mtx);
    if (w == g_w && h == g_h) return;
    g_w = w;
    g_h = h;
    g_targets.clear();  // a new overlay creates new targets
    g_locked = nullptr;
    g_frameImage = nullptr;
    if (g_image) g_image->Release();
    g_image = nullptr;
    Log("overlay: size %ux%u", w, h);
}

void LockImage(bool on)
{
    std::lock_guard<std::mutex> lock(g_mtx);
    g_locked = on ? g_image : nullptr;
    Log("overlay: image %s (%p)", g_locked ? "locked" : "picked per frame", g_locked);
}

void OnFrameEnd()
{
    std::lock_guard<std::mutex> lock(g_mtx);
    if (g_locked && g_targets.count(g_locked)) g_frameImage = g_locked;
    if (g_image) g_image->Release();
    g_image = g_frameImage;
    g_frameImage = nullptr;
    if (g_image) {
        g_image->AddRef();
        g_imageInfo = g_targets[g_image];
    }
}

ID3D12Resource* Image(DXGI_FORMAT& format, D3D12_RESOURCE_STATES& state)
{
    std::lock_guard<std::mutex> lock(g_mtx);
    if (!g_image) return nullptr;
    format = g_imageInfo.viewFormat;
    state = g_imageInfo.state;
    return g_image;
}

} // namespace overlay
