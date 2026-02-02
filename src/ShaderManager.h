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

    // Convert BGRA texture to UYVY and Key buffers
    // inputSRV1: SRV of the CEF BGRA texture (Frame T)
    // inputSRV2: SRV of the CEF BGRA texture (Frame T+1)
    // outputBuffer: Pointer to the DeckLink YUV buffer memory
    // keyBuffer: Pointer to the DeckLink Key buffer memory
    void ConvertAndDownload(ID3D11ShaderResourceView* inputSRV1, ID3D11ShaderResourceView* inputSRV2, void* outputBuffer, void* keyBuffer);

    // Set alpha threshold for unpremultiply
    void SetAlphaThreshold(float threshold);

private:
    ComPtr<ID3D11Device> m_device;
    ComPtr<ID3D11DeviceContext> m_context;
    
    ComPtr<ID3D11ComputeShader> m_computeShader;
    
    // GPU Buffers for Output
    ComPtr<ID3D11Texture2D> m_outputTexture; // YUV
    ComPtr<ID3D11UnorderedAccessView> m_outputUAV;
    
    ComPtr<ID3D11Texture2D> m_keyTexture;    // Key
    ComPtr<ID3D11UnorderedAccessView> m_keyUAV;
    
    // CPU Readback Buffers (Staging)
    ComPtr<ID3D11Texture2D> m_stagingOutput;
    ComPtr<ID3D11Texture2D> m_stagingKey;

    // Constant Buffer for Parameters
    ComPtr<ID3D11Buffer> m_constantBuffer;
    float m_alphaThreshold = 0.01f; // Default threshold

    int m_width = 0;
    int m_height = 0;

    bool LoadShader();
    bool CreateBuffers(int width, int height);
};
