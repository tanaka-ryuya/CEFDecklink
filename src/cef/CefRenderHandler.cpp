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
    if (m_lastTop) m_lastTop->Release();
    if (m_lastBottom) m_lastBottom->Release();
    while(!m_readyTextures.empty()) {
        if (m_readyTextures.front().srv) m_readyTextures.front().srv->Release();
        m_readyTextures.pop();
    }
}

void CefRenderHandlerImpl::GetViewRect(CefRefPtr<CefBrowser> browser, CefRect& rect) {
    rect.x = 0;
    rect.y = 0;
    rect.width = m_width;
    rect.height = m_height;
}

void CefRenderHandlerImpl::OnPaint(CefRefPtr<CefBrowser> browser, PaintElementType type, const RectList& dirtyRects, const void* buffer, int width, int height) {
    // Phase 1: Reserve Buffer (Short Lock)
    int bufferIdx = -1;
    size_t size = 0;
    {
        std::lock_guard<std::mutex> lock(m_mutex);
        if (width != m_width || height != m_height) return;

        size_t writeIdx = m_writeIndex.load();
        size_t readIdx = m_readIndex.load();

        if (writeIdx - readIdx >= kBufferCount) {
             // Drop frame
             return;
        }

        bufferIdx = writeIdx % kBufferCount;
        size = width * height * 4;
        
        // Ensure allocation (vector resize is fast if capacity exists, but safer to do under lock or pre-alloc)
        if (m_cpuBuffers[bufferIdx].size() != size) {
            m_cpuBuffers[bufferIdx].resize(size);
        }
    } // Unlock

    // Phase 2: Copy (No Lock)
    if (bufferIdx >= 0) {
        memcpy(m_cpuBuffers[bufferIdx].data(), buffer, size);
        m_timestamps[bufferIdx] = std::chrono::steady_clock::now();
        
        // Phase 3: Commit (Short Lock needed? Or just atomic write?)
        // m_writeIndex needs to be monotonic and observed by consumer.
        // SyncWithGPU relies on m_writeIndex.
        // We can just atomic increment.
        m_writeIndex++; 
        m_frameCount++;
        m_totalFrameCount++;
    }
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
    if (m_lastTop) { m_lastTop->Release(); m_lastTop = nullptr; }
    if (m_lastBottom) { m_lastBottom->Release(); m_lastBottom = nullptr; }
    while(!m_readyTextures.empty()) {
        if (m_readyTextures.front().srv) m_readyTextures.front().srv->Release();
        m_readyTextures.pop();
    }

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
    int bufferIdx = -1;
    ID3D11Texture2D* tex = nullptr;
    
    // Phase 1: Check for pending frames (Short Lock)
    {
        std::lock_guard<std::mutex> lock(m_mutex);
        
        size_t writeIdx = m_writeIndex.load();
        size_t readIdx = m_readIndex.load();

        if (writeIdx > readIdx && m_device) {
             bufferIdx = readIdx % kBufferCount;
             tex = m_textures[bufferIdx];
             if (tex) tex->AddRef(); // Keep alive while unlocked
        }
    } // Unlock

    // Phase 2: Upload (No Lock)
    // We own "readIdx" slot implicitly because OnPaint checks (write - read < k).
    // As long as we don't increment readIdx, OnPaint won't overwrite this slot.
    bool uploadSuccess = false;
    if (tex) {
        if (bufferIdx >= 0) {
            ID3D11DeviceContext* context = nullptr;
            m_device->GetImmediateContext(&context);
            if (context) {
                D3D11_MAPPED_SUBRESOURCE mapped;
                if (SUCCEEDED(context->Map(tex, 0, D3D11_MAP_WRITE_DISCARD, 0, &mapped))) {
                    // Safe to read cpuBuffer[bufferIdx] because OnPaint won't touch it until readIdx increments
                    const uint8_t* src = m_cpuBuffers[bufferIdx].data(); 
                    uint8_t* dst = (uint8_t*)mapped.pData;
                    
                    for (int y = 0; y < m_height; ++y) {
                        memcpy(dst + y * mapped.RowPitch, src + y * m_width * 4, m_width * 4);
                    }
                    context->Unmap(tex, 0);
                    uploadSuccess = true;
                }
                context->Release();
            }
        }
    }

    // Phase 3: Commit (Short Lock)
    if (tex) {
        std::lock_guard<std::mutex> lock(m_mutex);
        if (uploadSuccess) {
            auto srv = m_textureSRVs[bufferIdx];
            if (srv) {
                srv->AddRef();
                TextureEntry entry;
                entry.srv = srv;
                entry.timestamp = m_timestamps[bufferIdx];
                m_readyTextures.push(entry);
                
                // Limit queue size to avoid memory leaks if consumer stops
                while(m_readyTextures.size() > 8) {
                    m_readyTextures.front().srv->Release();
                    m_readyTextures.pop();
                }
            }
        }
        
        // Advance Consumer
        m_readIndex++;
        
        tex->Release();
    }
}

void CefRenderHandlerImpl::GetSynchronizedTextures(ID3D11ShaderResourceView** srvTop, ID3D11ShaderResourceView** srvBottom) {
    std::lock_guard<std::mutex> lock(m_mutex);

    bool poppedPair = false;
    bool poppedSingle = false;

    if (m_readyTextures.size() >= 2) {
        if (m_lastTop) m_lastTop->Release();
        m_lastTop = m_readyTextures.front().srv;
        m_readyTextures.pop();
        
        if (m_lastBottom) m_lastBottom->Release();
        m_lastBottom = m_readyTextures.front().srv;
        m_readyTextures.pop();
        
        poppedPair = true;
    } else if (m_readyTextures.size() == 1) {
        auto age = std::chrono::steady_clock::now() - m_readyTextures.front().timestamp;
        if (age > std::chrono::milliseconds(50)) {
            // Too old, animation likely stopped. Use it for both.
            if (m_lastTop) m_lastTop->Release();
            m_lastTop = m_readyTextures.front().srv;
            m_lastTop->AddRef(); 
            
            if (m_lastBottom) m_lastBottom->Release();
            m_lastBottom = m_readyTextures.front().srv;
            
            m_readyTextures.pop();
            poppedSingle = true;
        }
    }

    if (!poppedPair && !poppedSingle) {
        // We are waiting for frames. To prevent vibration (time-reversal jitter) 
        // from outputting an old pair [A, B] again, we freeze on the very last available frame [B, B].
        if (m_lastTop != m_lastBottom) {
            if (m_lastTop) m_lastTop->Release();
            m_lastTop = m_lastBottom;
            if (m_lastTop) m_lastTop->AddRef();
        }
    }

    if (srvTop) {
        *srvTop = m_lastTop;
        if (*srvTop) (*srvTop)->AddRef();
    }
    if (srvBottom) {
        *srvBottom = m_lastBottom;
        if (*srvBottom) (*srvBottom)->AddRef();
    }
}

ID3D11ShaderResourceView* CefRenderHandlerImpl::GetTextureSRV() {
    std::lock_guard<std::mutex> lock(m_mutex);
    if (m_lastBottom) {
        m_lastBottom->AddRef();
        return m_lastBottom; 
    }
    return nullptr;
}

