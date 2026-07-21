#include "ShaderManager.h"
#include <iostream>
#include <vector>
#include <cmath>
#include <algorithm>

#ifdef _WIN32
#include <d3dcompiler.h>
#pragma comment(lib, "d3dcompiler.lib")
#include <windows.h>

ShaderManager::ShaderManager(ID3D11Device* device, ID3D11DeviceContext* context) 
    : m_device(device), m_context(context) {
}
#else
ShaderManager::ShaderManager() {
}
#endif

ShaderManager::~ShaderManager() {
}

bool ShaderManager::Initialize(int width, int height) {
#ifdef _WIN32
    if (!LoadShader()) return false;
    if (!CreateBuffers(width, height)) return false;
    return true;
#else
    m_width = width;
    m_height = height;
    return true;
#endif
}

#ifdef _WIN32
bool ShaderManager::LoadShader() {
    ComPtr<ID3DBlob> blob;
    ComPtr<ID3DBlob> errorBlob;

    // List of paths to try
    std::vector<std::wstring> possiblePaths;
    
    // 1. Relative to Current Working Directory (Deployment)
    possiblePaths.push_back(L"shaders/YUVConvert.hlsl");
    
    // 2. Relative to Executable (Deployment - Robust)
    wchar_t exePath[MAX_PATH];
    GetModuleFileNameW(nullptr, exePath, MAX_PATH);
    std::wstring exeDir = exePath;
    size_t lastSlash = exeDir.find_last_of(L"\\/");
    if (lastSlash != std::wstring::npos) {
        exeDir = exeDir.substr(0, lastSlash);
        possiblePaths.push_back(exeDir + L"/shaders/YUVConvert.hlsl");
    }

    // 3. Fallback to Source Location (Development - from root)
    possiblePaths.push_back(L"src/render/shaders/YUVConvert.hlsl");
    
    // 4. Fallback to Source Location (Development - from build/Release)
    possiblePaths.push_back(L"../../src/render/shaders/YUVConvert.hlsl");

    HRESULT hr = E_FAIL;
    for (const auto& path : possiblePaths) {
        hr = D3DCompileFromFile(path.c_str(), nullptr, nullptr, "main", "cs_5_0", 0, 0, &blob, &errorBlob);
        if (SUCCEEDED(hr)) {
            std::wcout << L"[Shader] Successfully loaded from: " << path << std::endl;
            break;
        }
    }
    
    if (FAILED(hr)) {
        if (errorBlob) {
            std::cerr << "Shader Compile Error: " << static_cast<char*>(errorBlob->GetBufferPointer()) << std::endl;
        } else {
             std::cerr << "Shader Compile Failed. Tried paths:" << std::endl;
             for (const auto& path : possiblePaths) {
                  std::wcerr << L"  " << path << std::endl;
             }
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
    
    // Staging Texture Desc
    D3D11_TEXTURE2D_DESC stagingDesc = desc;
    stagingDesc.Usage = D3D11_USAGE_STAGING;
    stagingDesc.BindFlags = 0;
    stagingDesc.CPUAccessFlags = D3D11_CPU_ACCESS_READ;

    for (int i = 0; i < kBufferCount; ++i) {
        // Output Textures
        if (FAILED(m_device->CreateTexture2D(&desc, nullptr, &m_outputTextures[i]))) return false;
        if (FAILED(m_device->CreateUnorderedAccessView(m_outputTextures[i].Get(), nullptr, &m_outputUAVs[i]))) return false;
        
        // Staging Textures
        if (FAILED(m_device->CreateTexture2D(&stagingDesc, nullptr, &m_stagingOutputs[i]))) return false;
    }

    // Constant Buffer
    D3D11_BUFFER_DESC cbDesc = {};
    cbDesc.ByteWidth = sizeof(float) * 4; // 16 bytes (float + padding)
    cbDesc.Usage = D3D11_USAGE_DYNAMIC;
    cbDesc.BindFlags = D3D11_BIND_CONSTANT_BUFFER;
    cbDesc.CPUAccessFlags = D3D11_CPU_ACCESS_WRITE;
    
    if (FAILED(m_device->CreateBuffer(&cbDesc, nullptr, &m_constantBuffer))) return false;

    return true;
}

void ShaderManager::ConvertAndDownload(CefFrameResource inputSRV1, CefFrameResource inputSRV2, void* outputBuffer) {
    if (!m_computeShader || !inputSRV1 || !inputSRV2) return;

    static int logCounter = 0;
    bool doLog = (++logCounter % 120 == 0); 
    
    int bufferIdx = m_frameIndex % kBufferCount;

    // Update Constant Buffer
    D3D11_MAPPED_SUBRESOURCE mappedResource;
    HRESULT hr = m_context->Map(m_constantBuffer.Get(), 0, D3D11_MAP_WRITE_DISCARD, 0, &mappedResource);
    if (SUCCEEDED(hr)) {
        float* data = (float*)mappedResource.pData;
        data[0] = m_alphaThreshold;
        data[1] = (float)m_viewMode; // View Mode
        data[2] = 1.0f; // isLicensed (always 1.0 / free open source)
        data[3] = (float)m_filterMode; // 0=None, 1=3-tap, 2=5-tap vertical LPF
        m_context->Unmap(m_constantBuffer.Get(), 0);
    } else {
        if (doLog) std::cerr << "[Shader] Failed to map constant buffer: " << std::hex << hr << std::endl;
        return;
    }

    // Set Shader
    m_context->CSSetShader(m_computeShader.Get(), nullptr, 0);
    
    // Set Constant Buffer
    ID3D11Buffer* cbs[] = { m_constantBuffer.Get() };
    m_context->CSSetConstantBuffers(0, 1, cbs);
    
    // Set Resources (t0, t1)
    ID3D11ShaderResourceView* srvs[] = { inputSRV1, inputSRV2 };
    m_context->CSSetShaderResources(0, 2, srvs);
    
    // Process CURRENT Frame
    ID3D11UnorderedAccessView* uavs[] = { m_outputUAVs[bufferIdx].Get() };
    m_context->CSSetUnorderedAccessViews(0, 1, uavs, nullptr);
    
    // Dispatch
    m_context->Dispatch(120, 68, 1); // 1920x1080 / 16x16
    
    // Unbind UAVs
    ID3D11UnorderedAccessView* nullUAVs[] = { nullptr };
    m_context->CSSetUnorderedAccessViews(0, 1, nullUAVs, nullptr);
    
    ID3D11ShaderResourceView* nullSRVs[] = { nullptr, nullptr };
    m_context->CSSetShaderResources(0, 2, nullSRVs);

    // Copy to Staging (Schedule Copy)
    m_context->CopyResource(m_stagingOutputs[bufferIdx].Get(), m_outputTextures[bufferIdx].Get());
    
    // === READBACK (Pipelined) ===
    if (m_frameIndex >= 2) {
        int readbackIdx = (m_frameIndex - 2) % kBufferCount;
        
        // Map and Download (ARGB)
        D3D11_MAPPED_SUBRESOURCE mapped;
        hr = m_context->Map(m_stagingOutputs[readbackIdx].Get(), 0, D3D11_MAP_READ, 0, &mapped);
        
        if (SUCCEEDED(hr)) {
            uint8_t* src = (uint8_t*)mapped.pData;
            uint8_t* dst = (uint8_t*)outputBuffer;
            
            for (int y = 0; y < m_height; ++y) {
                 memcpy(dst + y * (m_width * 4), src + y * mapped.RowPitch, m_width * 4);
            }
            
            m_context->Unmap(m_stagingOutputs[readbackIdx].Get(), 0);
        } else {
            if (doLog) std::cerr << "[Shader] Failed to map staging texture: " << std::hex << hr << std::endl;
        }
    } else {
        memset(outputBuffer, 0, m_width * m_height * 4);
    }

    m_frameIndex++;
}
#else
// Helper float4 structure for CPU conversion
struct Float4 {
    float r, g, b, a;
    Float4() : r(0), g(0), b(0), a(0) {}
    Float4(float r, float g, float b, float a) : r(r), g(g), b(b), a(a) {}
    Float4 operator*(float scalar) const {
        return Float4(r * scalar, g * scalar, b * scalar, a * scalar);
    }
    Float4 operator+(const Float4& other) const {
        return Float4(r + other.r, g + other.g, b + other.b, a + other.a);
    }
    Float4 operator-(const Float4& other) const {
        return Float4(r - other.r, g - other.g, b - other.b, a - other.a);
    }
    Float4 abs() const {
        return Float4(std::abs(r), std::abs(g), std::abs(b), std::abs(a));
    }
};

static Float4 SampleFiltered(int x, int y, int width, int height, const uint8_t* p, int filterMode) {
    int idx = (y * width + x) * 4;
    Float4 center((float)p[idx+2] / 255.0f, (float)p[idx+1] / 255.0f, (float)p[idx] / 255.0f, (float)p[idx+3] / 255.0f);
    if (filterMode == 0) return center;

    int yM1 = std::max(y - 1, 0);
    int yP1 = std::min(y + 1, height - 1);
    int idxM1 = (yM1 * width + x) * 4;
    int idxP1 = (yP1 * width + x) * 4;
    Float4 above1((float)p[idxM1+2] / 255.0f, (float)p[idxM1+1] / 255.0f, (float)p[idxM1] / 255.0f, (float)p[idxM1+3] / 255.0f);
    Float4 below1((float)p[idxP1+2] / 255.0f, (float)p[idxP1+1] / 255.0f, (float)p[idxP1] / 255.0f, (float)p[idxP1+3] / 255.0f);

    if (filterMode == 1) {
        return above1 * 0.25f + center * 0.50f + below1 * 0.25f;
    }

    int yM2 = std::max(y - 2, 0);
    int yP2 = std::min(y + 2, height - 1);
    int idxM2 = (yM2 * width + x) * 4;
    int idxP2 = (yP2 * width + x) * 4;
    Float4 above2((float)p[idxM2+2] / 255.0f, (float)p[idxM2+1] / 255.0f, (float)p[idxM2] / 255.0f, (float)p[idxM2+3] / 255.0f);
    Float4 below2((float)p[idxP2+2] / 255.0f, (float)p[idxP2+1] / 255.0f, (float)p[idxP2] / 255.0f, (float)p[idxP2+3] / 255.0f);

    return above2 * 0.0625f + above1 * 0.25f + center * 0.375f + below1 * 0.25f + below2 * 0.0625f;
}

// macOS CPU color conversion pipeline
void ShaderManager::ConvertAndDownload(CefFrameResource srv1, CefFrameResource srv2, void* outputBuffer) {
    if (!srv1 || !srv2 || !outputBuffer) {
        if (outputBuffer) {
            memset(outputBuffer, 0, m_width * m_height * 4);
        }
        return;
    }

    const uint8_t* p1 = srv1->buffer.data();
    const uint8_t* p2 = srv2->buffer.data();
    uint32_t* out = static_cast<uint32_t*>(outputBuffer);

    for (int y = 0; y < m_height; ++y) {
        int rowOffset = y * m_width;
        for (int x = 0; x < m_width; ++x) {
            Float4 pixel;
            
            if (m_viewMode == 1) { // Diff Mode
                Float4 px1 = SampleFiltered(x, y, m_width, m_height, p1, m_filterMode);
                Float4 px2 = SampleFiltered(x, y, m_width, m_height, p2, m_filterMode);
                pixel = (px1 - px2).abs();
                pixel.a = 1.0f;
            }
            else if (m_viewMode == 2) { // Progressive Frame 1 (Top Field Source)
                pixel = SampleFiltered(x, y, m_width, m_height, p1, m_filterMode);
            }
            else if (m_viewMode == 3) { // Progressive Frame 2 (Bottom Field Source)
                pixel = SampleFiltered(x, y, m_width, m_height, p2, m_filterMode);
            }
            else if (m_viewMode == 4) { // 30p Blend Mode
                Float4 px1 = SampleFiltered(x, y, m_width, m_height, p1, m_filterMode);
                Float4 px2 = SampleFiltered(x, y, m_width, m_height, p2, m_filterMode);
                pixel = px1 * 0.5f + px2 * 0.5f;
            }
            else { // Interlaced Mode (0)
                if (y % 2 == 0) {
                    pixel = SampleFiltered(x, y, m_width, m_height, p1, m_filterMode);
                } else {
                    pixel = SampleFiltered(x, y, m_width, m_height, p2, m_filterMode);
                }
            }

            // Unpremultiply
            if (pixel.a > m_alphaThreshold) {
                pixel.r /= pixel.a;
                pixel.g /= pixel.a;
                pixel.b /= pixel.a;
            }

            uint8_t a = (uint8_t)(std::min(std::max(pixel.a, 0.0f), 1.0f) * 255.0f);
            uint8_t r = (uint8_t)(std::min(std::max(pixel.r, 0.0f), 1.0f) * 255.0f);
            uint8_t g = (uint8_t)(std::min(std::max(pixel.g, 0.0f), 1.0f) * 255.0f);
            uint8_t b = (uint8_t)(std::min(std::max(pixel.b, 0.0f), 1.0f) * 255.0f);

            out[rowOffset + x] = ((uint32_t)b << 24) | ((uint32_t)g << 16) | ((uint32_t)r << 8) | a;
        }
    }
}
#endif

void ShaderManager::SetAlphaThreshold(float threshold) {
    m_alphaThreshold = threshold;
}

void ShaderManager::SetViewMode(int mode) {
    m_viewMode = mode;
}


void ShaderManager::SetFilterMode(int mode) {
    m_filterMode = mode;
}
