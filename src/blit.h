#pragma once
#include <d3d12.h>
#include <dxgi1_4.h>

// Mouse pointer drawn into the copied image (the game uses the OS cursor, which is not in it).
struct CursorDraw {
    bool  visible = false;
    float x = 0, y = 0;   // tip position in source pixels
    float unit = 1;       // pixels per arrow unit (the arrow is ~12x19 units)
};

// Draws a game backbuffer into an OpenXR swapchain image with a fullscreen triangle.
// The source holds display-referred (gamma encoded) values; when the target RTV is an
// *_SRGB format the shader decodes them first so the hardware re-encode is lossless.
class Blitter {
public:
    bool Init(ID3D12Device* device, DXGI_FORMAT dstFormat);
    void Shutdown();

    // Records the draw. 'src' must be in PIXEL_SHADER_RESOURCE state, the RTV in RENDER_TARGET.
    void Record(ID3D12GraphicsCommandList* cl, ID3D12Resource* src, DXGI_FORMAT srcFormat,
                D3D12_CPU_DESCRIPTOR_HANDLE rtv, UINT dstW, UINT dstH, const CursorDraw* cursor = nullptr);

    bool Ready() const { return m_pso != nullptr; }

private:
    ID3D12Device*         m_device = nullptr;
    ID3D12RootSignature*  m_root = nullptr;
    ID3D12PipelineState*  m_pso = nullptr;
    ID3D12DescriptorHeap* m_srvHeap = nullptr;
    UINT                  m_srvInc = 0;
    UINT                  m_srvNext = 0;
    bool                  m_decodeSrgb = false;
};

bool IsSrgbFormat(DXGI_FORMAT f);
