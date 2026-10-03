#include "profile.h"
#include "config.h"
#include "log.h"

#include <windows.h>
#include <atomic>
#include <cstdio>
#include <mutex>

namespace prof {
namespace {

constexpr int kSlots = 16;
enum Kind { kStart = 0, kGameEnd = 1, kVrEnd = 2, kKinds = 3 };

struct Slot {
    uint64_t frame = ~0ull;
    bool     pending = false;   // VR end queued, results not read yet
    bool     hasStart = false;
    UINT64   fence = 0;
    CpuFrame cpu;
    bool     cpuSet = false;
    ID3D12CommandAllocator*    alloc[kKinds] = {};
    ID3D12GraphicsCommandList* list[kKinds] = {};
};

std::mutex            g_mtx;
bool                  g_ready = false;
ID3D12Device*         g_device = nullptr;
ID3D12CommandQueue*   g_queue = nullptr;
ID3D12QueryHeap*      g_heap = nullptr;
ID3D12Resource*       g_readback = nullptr;
ID3D12Fence*          g_fence = nullptr;
HANDLE                g_event = nullptr;
UINT64                g_fenceValue = 0;
UINT64                g_freq = 1;
Slot                  g_slots[kSlots];
std::atomic<uint64_t> g_startFrame{~0ull};  // frame whose GPU start marker is still to be placed
thread_local bool     t_ours = false;
UINT64                g_lastVrEnd = 0;
uint64_t              g_nextToProcess = 0;
double                g_lastRunFrame = 0;
FILE*                 g_csv = nullptr;
Summary               g_sum;
int                   g_sumL = 0, g_sumR = 0;

using PFN_ECL = void(STDMETHODCALLTYPE*)(ID3D12CommandQueue*, UINT, ID3D12CommandList* const*);
PFN_ECL o_ECL = nullptr;

Slot& SlotFor(uint64_t frame) { return g_slots[frame % kSlots]; }

// Waits until the slot's previous use has finished on the GPU, then reopens one of its lists.
ID3D12GraphicsCommandList* Begin(Slot& s, Kind k)
{
    if (s.fence && g_fence->GetCompletedValue() < s.fence) {
        g_fence->SetEventOnCompletion(s.fence, g_event);
        WaitForSingleObject(g_event, 100);
    }
    s.alloc[k]->Reset();
    s.list[k]->Reset(s.alloc[k], nullptr);
    return s.list[k];
}

void Submit(ID3D12GraphicsCommandList* cl)
{
    cl->Close();
    ID3D12CommandList* lists[] = {cl};
    t_ours = true;
    g_queue->ExecuteCommandLists(1, lists);
    t_ours = false;
}

void STDMETHODCALLTYPE Hook_ECL(ID3D12CommandQueue* q, UINT n, ID3D12CommandList* const* lists)
{
    if (!t_ours && q == g_queue && g_ready) {
        uint64_t f = g_startFrame.exchange(~0ull);
        if (f != ~0ull) {
            std::lock_guard<std::mutex> lock(g_mtx);
            Slot& s = SlotFor(f);
            if (!s.pending) {
                s.frame = f;
                ID3D12GraphicsCommandList* cl = Begin(s, kStart);
                cl->EndQuery(g_heap, D3D12_QUERY_TYPE_TIMESTAMP, (UINT)((f % kSlots) * kKinds + kStart));
                Submit(cl);
                s.hasStart = true;
            }
        }
    }
    o_ECL(q, n, lists);
}

double Ms(UINT64 ticks) { return (double)ticks * 1000.0 / (double)g_freq; }

void Process()
{
    UINT64 done = g_fence->GetCompletedValue();
    while (true) {
        Slot& s = SlotFor(g_nextToProcess);
        if (s.frame != g_nextToProcess) {
            // frame never reached the GPU path (e.g. skipped); move on if a later one exists
            bool later = false;
            for (auto& o : g_slots)
                if (o.pending && o.frame > g_nextToProcess && o.frame != ~0ull) later = true;
            if (!later) return;
            g_nextToProcess++;
            continue;
        }
        if (!s.pending || !s.cpuSet || s.fence > done) return;

        int base = (int)(g_nextToProcess % kSlots) * kKinds;
        D3D12_RANGE range = {(SIZE_T)base * 8, (SIZE_T)(base + kKinds) * 8};
        UINT64* t = nullptr;
        if (SUCCEEDED(g_readback->Map(0, &range, (void**)&t))) {
            UINT64 t0 = t[base + kStart], t1 = t[base + kGameEnd], t2 = t[base + kVrEnd];
            D3D12_RANGE none = {0, 0};
            g_readback->Unmap(0, &none);
            UINT64 start = g_lastVrEnd;
            if (s.hasStart && t0 > start) start = t0;
            if (!start || start > t1) start = t0;
            double gpuFrame = t1 > start ? Ms(t1 - start) : 0.0;
            double gpuVr = t2 > t1 ? Ms(t2 - t1) : 0.0;
            g_lastVrEnd = t2;

            const CpuFrame& c = s.cpu;
            if (g_csv)
                fprintf(g_csv, "%llu,%d,%d,%.3f,%.3f,%.3f,%.3f,%.3f,%.3f,%.3f,%d\n",
                        (unsigned long long)s.frame, c.eye, c.shouldRender, c.game * 1000, c.runFrame * 1000,
                        c.submit * 1000, c.present * 1000, c.wait * 1000, gpuFrame, gpuVr, s.hasStart);
            g_sum.gpuFrame += gpuFrame;
            g_sum.gpuVr += gpuVr;
            g_sum.samples++;
            if (c.eye == 0) g_sum.gpuFrameL += gpuFrame, g_sum.runFrameL += c.runFrame * 1000, g_sumL++;
            if (c.eye == 1) g_sum.gpuFrameR += gpuFrame, g_sum.runFrameR += c.runFrame * 1000, g_sumR++;
        }
        s.pending = false;
        s.cpuSet = false;
        s.hasStart = false;
        g_nextToProcess++;
    }
}

} // namespace

bool Enabled() { return g_config.profile; }

void Init(ID3D12Device* device, ID3D12CommandQueue* queue)
{
    std::lock_guard<std::mutex> lock(g_mtx);
    if (!g_config.profile || g_ready) return;
    g_device = device;
    g_queue = queue;
    D3D12_QUERY_HEAP_DESC qd = {D3D12_QUERY_HEAP_TYPE_TIMESTAMP, kSlots * kKinds, 0};
    if (FAILED(device->CreateQueryHeap(&qd, IID_PPV_ARGS(&g_heap)))) return;
    D3D12_HEAP_PROPERTIES hp = {D3D12_HEAP_TYPE_READBACK};
    D3D12_RESOURCE_DESC rd = {};
    rd.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER;
    rd.Width = kSlots * kKinds * 8;
    rd.Height = 1;
    rd.DepthOrArraySize = 1;
    rd.MipLevels = 1;
    rd.SampleDesc.Count = 1;
    rd.Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;
    if (FAILED(device->CreateCommittedResource(&hp, D3D12_HEAP_FLAG_NONE, &rd, D3D12_RESOURCE_STATE_COPY_DEST,
                                               nullptr, IID_PPV_ARGS(&g_readback))))
        return;
    if (FAILED(device->CreateFence(0, D3D12_FENCE_FLAG_NONE, IID_PPV_ARGS(&g_fence)))) return;
    g_event = CreateEventW(nullptr, FALSE, FALSE, nullptr);
    for (auto& s : g_slots)
        for (int k = 0; k < kKinds; k++) {
            if (FAILED(device->CreateCommandAllocator(D3D12_COMMAND_LIST_TYPE_DIRECT, IID_PPV_ARGS(&s.alloc[k]))))
                return;
            if (FAILED(device->CreateCommandList(0, D3D12_COMMAND_LIST_TYPE_DIRECT, s.alloc[k], nullptr,
                                                 IID_PPV_ARGS(&s.list[k]))))
                return;
            s.list[k]->Close();
        }
    queue->GetTimestampFrequency(&g_freq);
    g_csv = _wfopen((ModuleDir() + L"fs25vr_profile.csv").c_str(), L"w");
    if (g_csv)
        fprintf(g_csv, "frame,eye,should_render,cpu_game_ms,cpu_runframe_ms,cpu_vr_submit_ms,cpu_present_ms,"
                       "cpu_wait_headset_ms,gpu_frame_ms,gpu_vr_ms,gpu_start_marked\n");
    g_ready = true;
    Log("profiler: ready (timestamp frequency %llu Hz), writing fs25vr_profile.csv", g_freq);
}

void HookQueue(ID3D12CommandQueue* queue)
{
    if (!g_config.profile || o_ECL) return;
    void** vt = *(void***)queue;
    DWORD old;
    VirtualProtect(&vt[10], sizeof(void*), PAGE_READWRITE, &old);
    o_ECL = (PFN_ECL)vt[10];
    vt[10] = (void*)Hook_ECL;
    VirtualProtect(&vt[10], sizeof(void*), old, &old);
}

void GameWorkDone(uint64_t frame)
{
    std::lock_guard<std::mutex> lock(g_mtx);
    if (!g_ready) return;
    Slot& s = SlotFor(frame);
    if (s.pending) return;  // ring overrun, drop this frame
    if (s.frame != frame) s.hasStart = false;
    s.frame = frame;
    ID3D12GraphicsCommandList* cl = Begin(s, kGameEnd);
    cl->EndQuery(g_heap, D3D12_QUERY_TYPE_TIMESTAMP, (UINT)((frame % kSlots) * kKinds + kGameEnd));
    Submit(cl);
}

void VrWorkDone(uint64_t frame)
{
    std::lock_guard<std::mutex> lock(g_mtx);
    if (!g_ready) return;
    Slot& s = SlotFor(frame);
    if (s.frame != frame || s.pending) return;
    ID3D12GraphicsCommandList* cl = Begin(s, kVrEnd);
    UINT base = (UINT)((frame % kSlots) * kKinds);
    cl->EndQuery(g_heap, D3D12_QUERY_TYPE_TIMESTAMP, base + kVrEnd);
    if (!s.hasStart) cl->EndQuery(g_heap, D3D12_QUERY_TYPE_TIMESTAMP, base + kStart);  // keep slot defined
    cl->ResolveQueryData(g_heap, D3D12_QUERY_TYPE_TIMESTAMP, base, kKinds, g_readback, (UINT64)base * 8);
    Submit(cl);
    s.fence = ++g_fenceValue;
    t_ours = true;
    g_queue->Signal(g_fence, s.fence);
    t_ours = false;
    s.pending = true;
}

void EndFrame(uint64_t frame, const CpuFrame& cpu)
{
    std::lock_guard<std::mutex> lock(g_mtx);
    if (!g_ready) return;
    Slot& s = SlotFor(frame);
    if (s.frame == frame) {
        s.cpu = cpu;
        s.cpu.runFrame = g_lastRunFrame;
        s.cpuSet = true;
    }
    g_startFrame = frame + 1;  // the next game submission starts the next frame
    Process();
    if (g_csv && (frame % 90) == 0) fflush(g_csv);
}

void RunFrameTime(double seconds) { g_lastRunFrame = seconds; }

Summary TakeSummary()
{
    std::lock_guard<std::mutex> lock(g_mtx);
    Summary r = g_sum;
    if (r.samples) r.gpuFrame /= r.samples, r.gpuVr /= r.samples;
    if (g_sumL) r.gpuFrameL /= g_sumL, r.runFrameL /= g_sumL;
    if (g_sumR) r.gpuFrameR /= g_sumR, r.runFrameR /= g_sumR;
    g_sum = Summary{};
    g_sumL = g_sumR = 0;
    return r;
}

} // namespace prof
