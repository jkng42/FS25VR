#pragma once
#include <d3d12.h>
#include <cstdint>

// Profiling: GPU timestamps on the game's queue plus a per-frame CSV (x64\fs25vr_profile.csv).
// Enabled with profile=1 in fs25vr.ini.
namespace prof {

bool Enabled();
void Init(ID3D12Device* device, ID3D12CommandQueue* queue);
void HookQueue(ID3D12CommandQueue* queue);  // marks the GPU start of each frame's work

// Called from the Present path, in order, for frame 'frame' (= present index).
void GameWorkDone(uint64_t frame);           // before any VR work is queued
void VrWorkDone(uint64_t frame);             // after the VR copy is queued

struct CpuFrame {
    int    eye = -1;        // -1 = flat frame
    bool   shouldRender = false;
    double game = 0, submit = 0, present = 0, wait = 0, runFrame = 0;
};
void EndFrame(uint64_t frame, const CpuFrame& cpu);  // after Present + xrWaitFrame

// RunFrame (main loop) CPU duration, from the eye sync hook.
void RunFrameTime(double seconds);

// Rolling summary for the 10 s log line.
struct Summary {
    double gpuFrame = 0, gpuVr = 0, gpuFrameL = 0, gpuFrameR = 0, runFrameL = 0, runFrameR = 0;
    int    samples = 0;
};
Summary TakeSummary();

} // namespace prof
