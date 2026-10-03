#include "hooks.h"
#include "config.h"
#include "log.h"
#include "xr.h"
#include "window.h"
#include "game.h"
#include "sync.h"

#include <windows.h>
#include <d3d12.h>
#include <dxgi1_6.h>
#include <intrin.h>

namespace {

using PFN_CreateDXGIFactory1 = HRESULT(WINAPI*)(REFIID, void**);
using PFN_CreateDXGIFactory2 = HRESULT(WINAPI*)(UINT, REFIID, void**);
using PFN_CreateSwapChain = HRESULT(STDMETHODCALLTYPE*)(IDXGIFactory*, IUnknown*, DXGI_SWAP_CHAIN_DESC*,
                                                         IDXGISwapChain**);
using PFN_CreateSwapChainForHwnd = HRESULT(STDMETHODCALLTYPE*)(IDXGIFactory2*, IUnknown*, HWND,
                                                                const DXGI_SWAP_CHAIN_DESC1*,
                                                                const DXGI_SWAP_CHAIN_FULLSCREEN_DESC*,
                                                                IDXGIOutput*, IDXGISwapChain1**);
using PFN_Present = HRESULT(STDMETHODCALLTYPE*)(IDXGISwapChain*, UINT, UINT);
using PFN_Present1 = HRESULT(STDMETHODCALLTYPE*)(IDXGISwapChain1*, UINT, UINT, const DXGI_PRESENT_PARAMETERS*);
using PFN_ResizeBuffers = HRESULT(STDMETHODCALLTYPE*)(IDXGISwapChain*, UINT, UINT, UINT, DXGI_FORMAT, UINT);
using PFN_ResizeBuffers1 = HRESULT(STDMETHODCALLTYPE*)(IDXGISwapChain3*, UINT, UINT, UINT, DXGI_FORMAT, UINT,
                                                       const UINT*, IUnknown* const*);

PFN_CreateDXGIFactory1     o_CreateDXGIFactory1 = nullptr;
PFN_CreateDXGIFactory2     o_CreateDXGIFactory2 = nullptr;
PFN_CreateSwapChain        o_CreateSwapChain = nullptr;
PFN_CreateSwapChainForHwnd o_CreateSwapChainForHwnd = nullptr;
PFN_Present                o_Present = nullptr;
PFN_Present1               o_Present1 = nullptr;
PFN_ResizeBuffers          o_ResizeBuffers = nullptr;
PFN_ResizeBuffers1         o_ResizeBuffers1 = nullptr;

thread_local int t_inPresent = 0;  // Present1 may call Present internally on some runtimes

bool PatchPointer(void** slot, void* value, void** original)
{
    if (*slot == value) return true;
    DWORD old;
    if (!VirtualProtect(slot, sizeof(void*), PAGE_READWRITE, &old)) return false;
    if (original && !*original) *original = *slot;
    *slot = value;
    VirtualProtect(slot, sizeof(void*), old, &old);
    return true;
}

UINT SyncInterval(UINT requested) { return g_config.forceNoVsync ? 0 : requested; }

HRESULT STDMETHODCALLTYPE Hook_Present(IDXGISwapChain* self, UINT sync, UINT flags)
{
    if (t_inPresent || (flags & DXGI_PRESENT_TEST)) return o_Present(self, sync, flags);
    t_inPresent++;
    vr::OnPresent(self);
    HRESULT hr = o_Present(self, SyncInterval(sync), flags);
    vr::OnPostPresent();
    t_inPresent--;
    return hr;
}

HRESULT STDMETHODCALLTYPE Hook_Present1(IDXGISwapChain1* self, UINT sync, UINT flags,
                                        const DXGI_PRESENT_PARAMETERS* params)
{
    if (t_inPresent || (flags & DXGI_PRESENT_TEST)) return o_Present1(self, sync, flags, params);
    t_inPresent++;
    vr::OnPresent(self);
    HRESULT hr = o_Present1(self, SyncInterval(sync), flags, params);
    vr::OnPostPresent();
    t_inPresent--;
    return hr;
}

// The engine resizes to the window size it sees; with a shrunk window keep the full render size.
void KeepRenderSize(HWND hwnd, UINT& w, UINT& h)
{
    UINT vw, vh;
    VirtualSize(hwnd, vw, vh);
    if (vw && (w == 0 || h == 0 || (w < vw && h < vh))) {
        w = vw;
        h = vh;
    }
}

HWND SwapChainHwnd(IDXGISwapChain* sc)
{
    HWND h = nullptr;
    IDXGISwapChain1* sc1 = nullptr;
    if (SUCCEEDED(sc->QueryInterface(IID_PPV_ARGS(&sc1)))) {
        sc1->GetHwnd(&h);
        sc1->Release();
    }
    return h;
}

HRESULT STDMETHODCALLTYPE Hook_ResizeBuffers(IDXGISwapChain* self, UINT count, UINT w, UINT h, DXGI_FORMAT fmt,
                                             UINT flags)
{
    vr::OnResizeBuffers();
    HWND hwnd = SwapChainHwnd(self);
    UINT reqW = w, reqH = h;
    KeepRenderSize(hwnd, w, h);
    Log("ResizeBuffers %ux%u (requested %ux%u)", w, h, reqW, reqH);
    HRESULT hr = o_ResizeBuffers(self, count, w, h, fmt, flags);
    if (SUCCEEDED(hr) && w && h) FitWindow(hwnd, w, h);
    return hr;
}

HRESULT STDMETHODCALLTYPE Hook_ResizeBuffers1(IDXGISwapChain3* self, UINT count, UINT w, UINT h, DXGI_FORMAT fmt,
                                              UINT flags, const UINT* masks, IUnknown* const* queues)
{
    vr::OnResizeBuffers();
    HWND hwnd = SwapChainHwnd(self);
    KeepRenderSize(hwnd, w, h);
    Log("ResizeBuffers1 %ux%u", w, h);
    HRESULT hr = o_ResizeBuffers1(self, count, w, h, fmt, flags, masks, queues);
    if (SUCCEEDED(hr) && w && h) FitWindow(hwnd, w, h);
    return hr;
}

void HookSwapChain(IUnknown* queueOrDevice, IUnknown* swapUnk)
{
    IDXGISwapChain3* sc = nullptr;
    if (FAILED(swapUnk->QueryInterface(IID_PPV_ARGS(&sc)))) {
        Log("swap chain is not IDXGISwapChain3; VR disabled");
        return;
    }
    void** vt = *(void***)sc;
    PatchPointer(&vt[8], (void*)Hook_Present, (void**)&o_Present);
    PatchPointer(&vt[13], (void*)Hook_ResizeBuffers, (void**)&o_ResizeBuffers);
    PatchPointer(&vt[22], (void*)Hook_Present1, (void**)&o_Present1);
    PatchPointer(&vt[39], (void*)Hook_ResizeBuffers1, (void**)&o_ResizeBuffers1);

    ID3D12CommandQueue* queue = nullptr;
    if (queueOrDevice && SUCCEEDED(queueOrDevice->QueryInterface(IID_PPV_ARGS(&queue)))) {
        vr::OnSwapChainCreated(queue, sc);
        queue->Release();
    } else {
        Log("swap chain was not created from a D3D12 queue (D3D_12 renderer required)");
    }
    sc->Release();
}

// Second chance for everything that needs the game's code: by the time the game creates its swap
// chain its own startup has run, so a DRM wrapper has decrypted the code and rebuilt the import
// table. The Lua mod and vehicles load much later, so nothing that depends on these is missed.
void LateGameInit()
{
    static bool done = false;
    if (done) return;
    done = true;
    InstallGamePatches(true);
    InstallEyeSync(true);
    InstallWindowHooks();  // re-applies import hooks a wrapper may have overwritten
}

HRESULT STDMETHODCALLTYPE Hook_CreateSwapChainForHwnd(IDXGIFactory2* self, IUnknown* device, HWND hwnd,
                                                      const DXGI_SWAP_CHAIN_DESC1* desc,
                                                      const DXGI_SWAP_CHAIN_FULLSCREEN_DESC* fs,
                                                      IDXGIOutput* output, IDXGISwapChain1** out)
{
    LateGameInit();
    DXGI_SWAP_CHAIN_DESC1 d = *desc;
    d.BufferUsage |= DXGI_USAGE_SHADER_INPUT;  // we sample the backbuffer
    if (g_config.fitWindow) d.Scaling = DXGI_SCALING_STRETCH;  // window may be smaller than the render size
    Log("CreateSwapChainForHwnd %ux%u fmt %d buffers %u flags 0x%x", d.Width, d.Height, d.Format, d.BufferCount,
        d.Flags);
    HRESULT hr = o_CreateSwapChainForHwnd(self, device, hwnd, &d, fs, output, out);
    if (FAILED(hr)) {
        Log("  failed (0x%08x) with SHADER_INPUT, retrying unmodified", hr);
        hr = o_CreateSwapChainForHwnd(self, device, hwnd, desc, fs, output, out);
    }
    if (SUCCEEDED(hr) && out && *out) {
        HookSwapChain(device, *out);
        DXGI_SWAP_CHAIN_DESC1 actual = {};
        (*out)->GetDesc1(&actual);
        FitWindow(hwnd, actual.Width, actual.Height);
    }
    return hr;
}

HRESULT STDMETHODCALLTYPE Hook_CreateSwapChain(IDXGIFactory* self, IUnknown* device, DXGI_SWAP_CHAIN_DESC* desc,
                                               IDXGISwapChain** out)
{
    LateGameInit();
    if (desc) desc->BufferUsage |= DXGI_USAGE_SHADER_INPUT;
    HRESULT hr = o_CreateSwapChain(self, device, desc, out);
    if (SUCCEEDED(hr) && out && *out) HookSwapChain(device, *out);
    return hr;
}

void HookFactory(void* factory)
{
    IDXGIFactory2* f2 = nullptr;
    if (FAILED(((IUnknown*)factory)->QueryInterface(IID_PPV_ARGS(&f2)))) return;
    void** vt = *(void***)f2;
    PatchPointer(&vt[10], (void*)Hook_CreateSwapChain, (void**)&o_CreateSwapChain);
    PatchPointer(&vt[15], (void*)Hook_CreateSwapChainForHwnd, (void**)&o_CreateSwapChainForHwnd);
    f2->Release();
}

HRESULT WINAPI Hook_CreateDXGIFactory1(REFIID riid, void** out)
{
    HRESULT hr = o_CreateDXGIFactory1(riid, out);
    if (SUCCEEDED(hr)) HookFactory(*out);
    return hr;
}

HRESULT WINAPI Hook_CreateDXGIFactory2(UINT flags, REFIID riid, void** out)
{
    HRESULT hr = o_CreateDXGIFactory2(flags, riid, out);
    if (SUCCEEDED(hr)) HookFactory(*out);
    return hr;
}

// Replaces an entry in a module's import address table.
bool PatchIat(HMODULE mod, const char* dll, const char* func, void* hook, void** original)
{
    auto base = (BYTE*)mod;
    auto dos = (IMAGE_DOS_HEADER*)base;
    auto nt = (IMAGE_NT_HEADERS*)(base + dos->e_lfanew);
    auto& dir = nt->OptionalHeader.DataDirectory[IMAGE_DIRECTORY_ENTRY_IMPORT];
    if (!dir.VirtualAddress) return false;
    for (auto imp = (IMAGE_IMPORT_DESCRIPTOR*)(base + dir.VirtualAddress); imp->Name; imp++) {
        if (_stricmp((char*)(base + imp->Name), dll) != 0) continue;
        auto names = (IMAGE_THUNK_DATA*)(base + imp->OriginalFirstThunk);
        auto addrs = (IMAGE_THUNK_DATA*)(base + imp->FirstThunk);
        for (; names->u1.AddressOfData; names++, addrs++) {
            if (IMAGE_SNAP_BY_ORDINAL(names->u1.Ordinal)) continue;
            auto ibn = (IMAGE_IMPORT_BY_NAME*)(base + names->u1.AddressOfData);
            if (strcmp((char*)ibn->Name, func) == 0)
                return PatchPointer((void**)&addrs->u1.Function, hook, original);
        }
    }
    return false;
}

} // namespace

bool WriteJump(void* target, void* detour)
{
    BYTE code[12] = {0x48, 0xB8, 0, 0, 0, 0, 0, 0, 0, 0, 0xFF, 0xE0};  // mov rax, imm64; jmp rax
    memcpy(code + 2, &detour, 8);
    DWORD old;
    if (!VirtualProtect(target, sizeof(code), PAGE_EXECUTE_READWRITE, &old)) return false;
    memcpy(target, code, sizeof(code));
    VirtualProtect(target, sizeof(code), old, &old);
    FlushInstructionCache(GetCurrentProcess(), target, sizeof(code));
    return true;
}

// Since v1.20 the game obtains DXGI through NVIDIA Streamline (sl.interposer.dll) and frame
// generation libraries, which call the real dxgi.dll themselves, so the import hooks above never
// see the factory. Patching the real factory's vtable directly catches every swap chain, whichever
// layer creates it. Runs on a worker thread: creating DXGI objects inside DllMain is unsafe.
DWORD WINAPI HookRealFactory(void*)
{
    IDXGIFactory2* f = nullptr;
    HRESULT hr = CreateDXGIFactory1(IID_PPV_ARGS(&f));  // our own import, not the game's
    if (FAILED(hr) || !f) {
        Log("dxgi: could not create a factory to hook (0x%08x)", hr);
        return 0;
    }
    HookFactory(f);
    f->Release();
    Log("dxgi: real factory hooked");
    return 0;
}

bool InstallDxgiHooks()
{
    HMODULE exe = GetModuleHandleW(nullptr);
    bool a = PatchIat(exe, "dxgi.dll", "CreateDXGIFactory1", (void*)Hook_CreateDXGIFactory1,
                      (void**)&o_CreateDXGIFactory1);
    bool b = PatchIat(exe, "dxgi.dll", "CreateDXGIFactory2", (void*)Hook_CreateDXGIFactory2,
                      (void**)&o_CreateDXGIFactory2);
    Log("dxgi import hooks: CreateDXGIFactory1=%d CreateDXGIFactory2=%d", a, b);
    HANDLE t = CreateThread(nullptr, 0, HookRealFactory, nullptr, 0, nullptr);
    if (t) CloseHandle(t);
    return true;
}
