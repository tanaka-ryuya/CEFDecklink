#pragma once
#include <string>

#ifdef _WIN32
#include <d3d11.h>
#include <wrl/client.h>
using Microsoft::WRL::ComPtr;
#endif

#include "CefRenderHandler.h"

class ShaderManager {
public:
#ifdef _WIN32
    ShaderManager(ID3D11Device* device, ID3D11DeviceContext* context);
#else
    ShaderManager();
#endif
    ~ShaderManager();

    // Initialize shaders and buffers
    bool Initialize(int width, int height);

    // Convert BGRA texture to ARGB for DeckLink
    // inputSRV1: CEF BGRA texture (Frame T)
    // inputSRV2: CEF BGRA texture (Frame T+1)
    // outputBuffer: Pointer to the DeckLink ARGB buffer memory
    void ConvertAndDownload(CefFrameResource inputSRV1, CefFrameResource inputSRV2, void* outputBuffer);

    // Set alpha threshold for unpremultiply
    void SetAlphaThreshold(float threshold);

    // Set View Mode
    // 0 = Interlace, 1 = Diff, 2 = Progressive F1, 3 = Progressive F2
    void SetViewMode(int mode);

    // Set Vertical Low-Pass Filter Mode
    // 0 = None, 1 = 3-tap, 2 = 5-tap
    void SetFilterMode(int mode);

    // Set License Status
    void SetLicensed(bool licensed);

private:
#ifdef _WIN32
    ComPtr<ID3D11Device> m_device;
    ComPtr<ID3D11DeviceContext> m_context;
    
    ComPtr<ID3D11ComputeShader> m_computeShader;
    
    // GPU Buffers for Output (Sextuple Buffering)
    static const int kBufferCount = 6;
    ComPtr<ID3D11Texture2D> m_outputTextures[kBufferCount]; // ARGB
    ComPtr<ID3D11UnorderedAccessView> m_outputUAVs[kBufferCount];
    
    // CPU Readback Buffer (Staging) (Triple Buffering)
    ComPtr<ID3D11Texture2D> m_stagingOutputs[kBufferCount];
    
    uint64_t m_frameIndex = 0;

    // Constant Buffer for Parameters
    ComPtr<ID3D11Buffer> m_constantBuffer;
#endif

    float m_alphaThreshold = 0.0f; // Default threshold
    int m_viewMode = 0;
    int m_filterMode = 0;
    bool m_isLicensed = false;

    int m_width = 0;
    int m_height = 0;

#ifdef _WIN32
    bool LoadShader();
    bool CreateBuffers(int width, int height);
#endif
};
