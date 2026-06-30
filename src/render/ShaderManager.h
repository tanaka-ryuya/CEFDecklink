#pragma once
#include <d3d11.h>
#include <string>
#include <wrl/client.h>

using Microsoft::WRL::ComPtr;

class ShaderManager {
public:
    ShaderManager(ID3D11Device* device, ID3D11DeviceContext* context);
    ~ShaderManager();

    // Initialize shaders and buffers
    bool Initialize(int width, int height);

    // Convert BGRA texture to ARGB for DeckLink
    // inputSRV1: SRV of the CEF BGRA texture (Frame T)
    // inputSRV2: SRV of the CEF BGRA texture (Frame T+1)
    // outputBuffer: Pointer to the DeckLink ARGB buffer memory
    void ConvertAndDownload(ID3D11ShaderResourceView* inputSRV1, ID3D11ShaderResourceView* inputSRV2, void* outputBuffer);

    // Set alpha threshold for unpremultiply
    void SetAlphaThreshold(float threshold);

    // Set View Mode
    // 0 = Interlace, 1 = Diff, 2 = Progressive F1, 3 = Progressive F2
    void SetViewMode(int mode);

    // Set License Status
    void SetLicensed(bool licensed);

private:
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
    float m_alphaThreshold = 0.01f; // Default threshold
    int m_viewMode = 0;
    bool m_isLicensed = false;

    int m_width = 0;
    int m_height = 0;

    bool LoadShader();
    bool CreateBuffers(int width, int height);
};
