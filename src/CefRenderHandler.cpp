#include "CefRenderHandler.h"
#include <iostream>

CefRenderHandlerImpl::CefRenderHandlerImpl(ID3D11Device* device) : m_device(device) {
    // Initial Resize to allocate texture
    Resize(1920, 1080);
}

CefRenderHandlerImpl::~CefRenderHandlerImpl() {
    if (m_textureSRV) m_textureSRV->Release();
    if (m_texture) m_texture->Release();
}

void CefRenderHandlerImpl::GetViewRect(CefRefPtr<CefBrowser> browser, CefRect& rect) {
    rect.x = 0;
    rect.y = 0;
    rect.width = m_width;
    rect.height = m_height;
}

void CefRenderHandlerImpl::OnPaint(CefRefPtr<CefBrowser> browser, PaintElementType type, const RectList& dirtyRects, const void* buffer, int width, int height) {
    // This runs on CEF UI/Render thread.
    // CRITICAL: Do NOT use ID3D11DeviceContext here. It is not thread safe.
    // Instead, copy to a CPU buffer.
    
    std::lock_guard<std::mutex> lock(m_mutex);
    
    if (width != m_width || height != m_height) return;

    // Allocate if needed (or if size changed, though Resize handles that)
    size_t size = width * height * 4;
    if (m_cpuBuffer.size() != size) {
        m_cpuBuffer.resize(size);
    }

    // Copy buffer
    memcpy(m_cpuBuffer.data(), buffer, size);
    m_cpuBufferHasNewData = true;
}

void CefRenderHandlerImpl::Resize(int width, int height) {
    std::lock_guard<std::mutex> lock(m_mutex);
    m_width = width;
    m_height = height;
    m_cpuBuffer.resize(width * height * 4);
    
    if (m_textureSRV) { m_textureSRV->Release(); m_textureSRV = nullptr; }
    if (m_texture) { m_texture->Release(); m_texture = nullptr; }

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

    m_device->CreateTexture2D(&desc, nullptr, &m_texture);

    if (m_texture) {
        m_device->CreateShaderResourceView(m_texture, nullptr, &m_textureSRV);
    }
}

// Call this from Main Thread
void CefRenderHandlerImpl::SyncWithGPU() {
    std::lock_guard<std::mutex> lock(m_mutex);
    if (!m_cpuBufferHasNewData || !m_texture || !m_device) return;

    ID3D11DeviceContext* context = nullptr;
    m_device->GetImmediateContext(&context);
    
    if (context) {
        D3D11_MAPPED_SUBRESOURCE mapped;
        if (SUCCEEDED(context->Map(m_texture, 0, D3D11_MAP_WRITE_DISCARD, 0, &mapped))) {
            const uint8_t* src = m_cpuBuffer.data();
            uint8_t* dst = (uint8_t*)mapped.pData;
            
            for (int y = 0; y < m_height; ++y) {
                memcpy(dst + y * mapped.RowPitch, src + y * m_width * 4, m_width * 4);
            }
            context->Unmap(m_texture, 0);
        }
        context->Release();
    }
    m_cpuBufferHasNewData = false;
}

ID3D11ShaderResourceView* CefRenderHandlerImpl::GetTextureSRV() {
    std::lock_guard<std::mutex> lock(m_mutex);
    if (m_textureSRV) {
        m_textureSRV->AddRef();
        return m_textureSRV; 
    }
    return nullptr;
}

