#include "ShaderManager.h"
#include <d3dcompiler.h>
#include <iostream>
#include <vector>

#pragma comment(lib, "d3dcompiler.lib")

ShaderManager::ShaderManager(ID3D11Device* device, ID3D11DeviceContext* context) 
    : m_device(device), m_context(context) {
}

ShaderManager::~ShaderManager() {
}

bool ShaderManager::Initialize(int width, int height) {
    if (!LoadShader()) return false;
    if (!CreateBuffers(width, height)) return false;
    return true;
}

bool ShaderManager::LoadShader() {
    ComPtr<ID3DBlob> blob;
    ComPtr<ID3DBlob> errorBlob;
    
    // Compile from file (assuming file is next to exe or in src location - adjustment needed for deployment)
    // For Debug: absolute path or relative to build? 
    // We will try running from current working dir or source dir.
    // Ideally we embed headers or copy .hlsl to bin.
    // Let's assume it's copied to bin/shaders/YUVConvert.hlsl
    
    HRESULT hr = D3DCompileFromFile(L"src/shaders/YUVConvert.hlsl", nullptr, nullptr, "main", "cs_5_0", 0, 0, &blob, &errorBlob);
    
    if (FAILED(hr)) {
        if (errorBlob) {
            std::cerr << "Shader Compile Error: " << static_cast<char*>(errorBlob->GetBufferPointer()) << std::endl;
        } else {
             std::cerr << "Shader Compile Failed (File not found?)" << std::endl;
        }
        return false;
    }
    
    hr = m_device->CreateComputeShader(blob->GetBufferPointer(), blob->GetBufferSize(), nullptr, &m_computeShader);
    return SUCCEEDED(hr);
}

bool ShaderManager::CreateBuffers(int width, int height) {
    m_width = width;
    m_height = height;
    
    D3D11_TEXTURE2D_DESC desc = {};
    desc.Width = width;  // 1920 for standard resolution
    desc.Height = height;
    desc.MipLevels = 1;
    desc.ArraySize = 1;
    desc.Format = DXGI_FORMAT_R32_UINT; // Packed ARGB (1 uint32 per pixel)
    desc.SampleDesc.Count = 1;
    desc.Usage = D3D11_USAGE_DEFAULT;
    desc.BindFlags = D3D11_BIND_UNORDERED_ACCESS;
    desc.CPUAccessFlags = 0;
    
    // Output Texture (1920x1080 ARGB)
    if (FAILED(m_device->CreateTexture2D(&desc, nullptr, &m_outputTexture))) return false;
    if (FAILED(m_device->CreateUnorderedAccessView(m_outputTexture.Get(), nullptr, &m_outputUAV))) return false;
    
    // Staging Texture
    desc.Usage = D3D11_USAGE_STAGING;
    desc.BindFlags = 0;
    desc.CPUAccessFlags = D3D11_CPU_ACCESS_READ;
    
    if (FAILED(m_device->CreateTexture2D(&desc, nullptr, &m_stagingOutput))) return false;

    // Constant Buffer
    D3D11_BUFFER_DESC cbDesc = {};
    cbDesc.ByteWidth = sizeof(float) * 4; // 16 bytes (float + padding)
    cbDesc.Usage = D3D11_USAGE_DYNAMIC;
    cbDesc.BindFlags = D3D11_BIND_CONSTANT_BUFFER;
    cbDesc.CPUAccessFlags = D3D11_CPU_ACCESS_WRITE;
    
    if (FAILED(m_device->CreateBuffer(&cbDesc, nullptr, &m_constantBuffer))) return false;

    return true;
}

void ShaderManager::ConvertAndDownload(ID3D11ShaderResourceView* inputSRV1, ID3D11ShaderResourceView* inputSRV2, void* outputBuffer) {
    if (!m_computeShader || !inputSRV1 || !inputSRV2) return;

    // Update Constant Buffer
    D3D11_MAPPED_SUBRESOURCE mappedResource;
    if (SUCCEEDED(m_context->Map(m_constantBuffer.Get(), 0, D3D11_MAP_WRITE_DISCARD, 0, &mappedResource))) {
        float* data = (float*)mappedResource.pData;
        data[0] = m_alphaThreshold;
        data[1] = 0.0f; // padding
        data[2] = 0.0f; // padding
        data[3] = 0.0f; // padding
        m_context->Unmap(m_constantBuffer.Get(), 0);
    }

    // Set Shader
    m_context->CSSetShader(m_computeShader.Get(), nullptr, 0);
    
    // Set Constant Buffer
    ID3D11Buffer* cbs[] = { m_constantBuffer.Get() };
    m_context->CSSetConstantBuffers(0, 1, cbs);
    
    // Set Resources (t0, t1)
    ID3D11ShaderResourceView* srvs[] = { inputSRV1, inputSRV2 };
    m_context->CSSetShaderResources(0, 2, srvs);
    
    ID3D11UnorderedAccessView* uavs[] = { m_outputUAV.Get() };
    m_context->CSSetUnorderedAccessViews(0, 1, uavs, nullptr);
    
    // Dispatch
    // Thread group size in shader is (16, 16, 1) = 256 threads.
    // Each thread processes 1 pixel (ARGB).
    // X Groups = Ceil(1920 / 16) = 120
    // Y Groups = Ceil(1080 / 16) = 68
    
    m_context->Dispatch(120, 68, 1);
    
    // Unbind UAVs
    ID3D11UnorderedAccessView* nullUAVs[] = { nullptr };
    m_context->CSSetUnorderedAccessViews(0, 1, nullUAVs, nullptr);
    
    ID3D11ShaderResourceView* nullSRVs[] = { nullptr, nullptr };
    m_context->CSSetShaderResources(0, 2, nullSRVs);

    // Copy to Staging
    m_context->CopyResource(m_stagingOutput.Get(), m_outputTexture.Get());
    
    // Map and Download (ARGB)
    D3D11_MAPPED_SUBRESOURCE mapped;
    if (SUCCEEDED(m_context->Map(m_stagingOutput.Get(), 0, D3D11_MAP_READ, 0, &mapped))) {
        uint8_t* src = (uint8_t*)mapped.pData;
        uint8_t* dst = (uint8_t*)outputBuffer;
        // ARGB: 1920 pixels wide, 4 bytes per pixel
        // Copy row by row
        for (int y = 0; y < m_height; ++y) {
             memcpy(dst + y * (m_width * 4), src + y * mapped.RowPitch, m_width * 4);
        }
        
        m_context->Unmap(m_stagingOutput.Get(), 0);
    }
}

void ShaderManager::SetAlphaThreshold(float threshold) {
    m_alphaThreshold = threshold;
}
