#pragma once
#include <d3d12.h>
#include <dxgi1_4.h>
#include <cstdint>

// What the Lua camera code needs to render one eye.
struct EyeView {
    int   eye;           // 0 = left, 1 = right
    float pos[3];        // eye position in recentred tracking space (metres * worldScale), camera axes
    float quat[4];       // eye orientation x,y,z,w in recentred tracking space
    float fovY;          // radians, for setFovY
    float offX, offY;    // for setProjectionOffset
    uint64_t frame;      // present index this view will be shown on
    bool  second;        // second frame of a stereo pair (the simulation is frozen on it)
};

namespace vr {

// Render thread (called from the swap chain hooks).
void OnSwapChainCreated(ID3D12CommandQueue* queue, IDXGISwapChain* swap);
void OnPresent(IDXGISwapChain* swap);
void OnPostPresent();
void OnResizeBuffers();

// Lua thread.
bool IsRunning();
bool GetView(EyeView& out);
void RequestRecenter();
void SetSymmetricFrustum(bool on);
bool IsCalibrating();
bool NextFrameIsSecondEye();  // main thread, before the frame runs  // the mod should draw the latency marker this frame
const char* Status();

} // namespace vr
