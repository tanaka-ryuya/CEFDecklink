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

    // Get the two most recent distinct textures
    void GetLatestTextures(ID3D11ShaderResourceView** srv1, ID3D11ShaderResourceView** srv2);

    // Diagnostics
    int GetAndResetFrameCount() { return m_frameCount.exchange(0); }
    uint64_t GetTotalFrameCount() const { return m_totalFrameCount.load(); }
    bool HasPendingFrames(size_t count) const { return (m_writeIndex.load() - m_readIndex.load()) >= count; }

private:
    int m_width = 1920;
    int m_height = 1080;
    ID3D11Device* m_device = nullptr;
    
    // Triple Buffering (Ring Buffer) -> Now Sextuple Buffering
    static const int kBufferCount = 6;
    
    ID3D11Texture2D* m_textures[kBufferCount] = { nullptr };
    ID3D11ShaderResourceView* m_textureSRVs[kBufferCount] = { nullptr };
    
    // The history of recent textures for decoupled retrieval
    ID3D11ShaderResourceView* m_historySRVs[2] = { nullptr };

    std::mutex m_mutex;
    
    // CPU Ring Buffers
    std::vector<uint8_t> m_cpuBuffers[kBufferCount];

    // Ring Buffer Indices
    // m_writeIndex: Incremented by OnPaint (Producer)
    // m_readIndex:  Incremented by SyncWithGPU (Consumer)
    std::atomic<size_t> m_writeIndex{ 0 };
    std::atomic<size_t> m_readIndex{ 0 };
    
    std::atomic<int> m_frameCount{ 0 };
    std::atomic<uint64_t> m_totalFrameCount{ 0 };

    // Use IMPLEMENT_REFCOUNTING macro
    IMPLEMENT_REFCOUNTING(CefRenderHandlerImpl);
};
