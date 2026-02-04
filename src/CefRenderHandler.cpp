#include "CefRenderHandler.h"
#include <iostream>

CefRenderHandlerImpl::CefRenderHandlerImpl(ID3D11Device* device) : m_device(device) {
    // Initial Resize to allocate textures
    Resize(1920, 1080);
}

CefRenderHandlerImpl::~CefRenderHandlerImpl() {
    for (int i = 0; i < kBufferCount; ++i) {
        if (m_textureSRVs[i]) m_textureSRVs[i]->Release();
        if (m_textures[i]) m_textures[i]->Release();
    }
}

void CefRenderHandlerImpl::GetViewRect(CefRefPtr<CefBrowser> browser, CefRect& rect) {
    rect.x = 0;
    rect.y = 0;
    rect.width = m_width;
    rect.height = m_height;
}

void CefRenderHandlerImpl::OnPaint(CefRefPtr<CefBrowser> browser, PaintElementType type, const RectList& dirtyRects, const void* buffer, int width, int height) {
    // This runs on CEF UI/Render thread.
    std::lock_guard<std::mutex> lock(m_mutex);
    
    if (width != m_width || height != m_height) return;

    size_t writeIdx = m_writeIndex.load();
    size_t readIdx = m_readIndex.load();

    // Check Overlap: If next write would overtake read by full buffer size, drop frame.
    // Allow effectively (kBufferCount - 1) frames ahead.
    if (writeIdx - readIdx >= kBufferCount) {
        // Drop frame to prevent overwriting data currently being uploaded
        return; 
    }

    // Allocate if needed
    size_t size = width * height * 4;
    int bufferIdx = writeIdx % kBufferCount;
    
    if (m_cpuBuffers[bufferIdx].size() != size) {
        m_cpuBuffers[bufferIdx].resize(size);
    }

    // Copy buffer
    memcpy(m_cpuBuffers[bufferIdx].data(), buffer, size);
    
    // Commit
    m_writeIndex++;
    m_frameCount++;
}

void CefRenderHandlerImpl::Resize(int width, int height) {
    std::lock_guard<std::mutex> lock(m_mutex);
    m_width = width;
    m_height = height;
    
    for (int i = 0; i < kBufferCount; ++i) {
        m_cpuBuffers[i].resize(width * height * 4);
        
        if (m_textureSRVs[i]) { m_textureSRVs[i]->Release(); m_textureSRVs[i] = nullptr; }
        if (m_textures[i]) { m_textures[i]->Release(); m_textures[i] = nullptr; }
    }
    m_lastUploadedSRV = nullptr; // Reset safe SRV

    if (!m_device) return;

    D3D11_TEXTURE2D_DESC desc;
    ZeroMemory(&desc, sizeof(desc));
    desc.Width = width;
    desc.Height = height;
    desc.MipLevels = 1;
    desc.ArraySize = 1;
    desc.Format = DXGI_FORMAT_B8G8R8A8_UNORM; 
    desc.SampleDesc.Count = 1;
    desc.Usage = D3D11_USAGE_DYNAMIC;
    desc.BindFlags = D3D11_BIND_SHADER_RESOURCE;
    desc.CPUAccessFlags = D3D11_CPU_ACCESS_WRITE;

    for (int i = 0; i < kBufferCount; ++i) {
        m_device->CreateTexture2D(&desc, nullptr, &m_textures[i]);
        if (m_textures[i]) {
            m_device->CreateShaderResourceView(m_textures[i], nullptr, &m_textureSRVs[i]);
        }
    }
}

// Call this from Main Thread / DeckLink Thread
void CefRenderHandlerImpl::SyncWithGPU() {
    std::lock_guard<std::mutex> lock(m_mutex);
    
    size_t writeIdx = m_writeIndex.load();
    size_t readIdx = m_readIndex.load();

    if (!m_device) return;

    // Process all pending frames up to writeIndex (Resulting in "Catch Up")
    // OR just process the next one. 
    // Optimization: Just process the *latest* completable one to reduce latency
    // BUT since we are on the DeckLink thread which drives output, we should consume one per call if possible,
    // or skip if we are way behind.
    // For now, let's just consume the NEXT sequential frame to ensure smooth animation.
    
    if (writeIdx > readIdx) {
        int bufferIdx = readIdx % kBufferCount;
        
        ID3D11Texture2D* tex = m_textures[bufferIdx];
        if (!tex) return;

        ID3D11DeviceContext* context = nullptr;
        m_device->GetImmediateContext(&context);
        
        if (context) {
            D3D11_MAPPED_SUBRESOURCE mapped;
            if (SUCCEEDED(context->Map(tex, 0, D3D11_MAP_WRITE_DISCARD, 0, &mapped))) {
                const uint8_t* src = m_cpuBuffers[bufferIdx].data();
                uint8_t* dst = (uint8_t*)mapped.pData;
                
                for (int y = 0; y < m_height; ++y) {
                    memcpy(dst + y * mapped.RowPitch, src + y * m_width * 4, m_width * 4);
                }
                context->Unmap(tex, 0);
                
                // CRITICAL: Now that upload is complete, update the Safe SRV
                m_lastUploadedSRV = m_textureSRVs[bufferIdx];
            }
            context->Release();
        }
        
        m_readIndex++;
    }
}

ID3D11ShaderResourceView* CefRenderHandlerImpl::GetTextureSRV() {
    std::lock_guard<std::mutex> lock(m_mutex);
    if (m_lastUploadedSRV) {
        m_lastUploadedSRV->AddRef();
        return m_lastUploadedSRV; 
    }
    return nullptr;
}

