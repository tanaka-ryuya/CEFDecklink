#pragma once
#include <include/cef_render_handler.h>
#include <mutex>
#include <vector>
#include <atomic>
#include <d3d11.h>

class CefRenderHandlerImpl : public CefRenderHandler {
public:
    CefRenderHandlerImpl(ID3D11Device* device);
    ~CefRenderHandlerImpl();

    // CefRenderHandler methods
    void GetViewRect(CefRefPtr<CefBrowser> browser, CefRect& rect) override;
    void OnPaint(CefRefPtr<CefBrowser> browser, PaintElementType type, const RectList& dirtyRects, const void* buffer, int width, int height) override;

    // Custom methods for DX11 Interop
    // Returns the SRV of the texture that is safe to use (completely uploaded)
    ID3D11ShaderResourceView* GetTextureSRV();
    
    // Set the expected size
    void Resize(int width, int height);

    // Call from Main Thread to upload pending data
    void SyncWithGPU();

private:
    int m_width = 1920;
    int m_height = 1080;
    ID3D11Device* m_device = nullptr;
    
    // Triple Buffering (Ring Buffer) -> Now Sextuple Buffering
    static const int kBufferCount = 6;
    
    ID3D11Texture2D* m_textures[kBufferCount] = { nullptr };
    ID3D11ShaderResourceView* m_textureSRVs[kBufferCount] = { nullptr };
    
    // The SRV that is guaranteed to be fully uploaded and safe for the shader to read
    ID3D11ShaderResourceView* m_lastUploadedSRV = nullptr;

    std::mutex m_mutex;
    
    // CPU Ring Buffers
    std::vector<uint8_t> m_cpuBuffers[kBufferCount];

    // Ring Buffer Indices
    // m_writeIndex: Incremented by OnPaint (Producer)
    // m_readIndex:  Incremented by SyncWithGPU (Consumer)
    std::atomic<size_t> m_writeIndex{ 0 };
    std::atomic<size_t> m_readIndex{ 0 };

    // Use IMPLEMENT_REFCOUNTING macro
    IMPLEMENT_REFCOUNTING(CefRenderHandlerImpl);
};
