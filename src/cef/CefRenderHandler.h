#pragma once
#include <include/cef_render_handler.h>
#include <mutex>
#include <vector>
#include <atomic>
#include <d3d11.h>
#include <chrono>
#include <queue>

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
    // Retreives a synchronized pair of frames for interlaced output, applying stutter prevention logic.
    void GetSynchronizedTextures(ID3D11ShaderResourceView** srvTop, ID3D11ShaderResourceView** srvBottom);

    // Diagnostics
    int GetAndResetFrameCount() { return m_frameCount.exchange(0); }
    uint64_t GetTotalFrameCount() const { return m_totalFrameCount.load(); }
    bool HasPendingFrames(size_t count) const { return (m_writeIndex.load() - m_readIndex.load()) >= count; }
    int GetPendingFrameCount() {
        std::lock_guard<std::mutex> lock(m_mutex);
        return (int)m_readyTextures.size();
    }
    int GetAndResetDroppedFrames() { return m_droppedFrames.exchange(0); }
    int GetAndResetDuplicatedFrames() { return m_duplicatedFrames.exchange(0); }

private:
    int m_width = 1920;
    int m_height = 1080;
    ID3D11Device* m_device = nullptr;
    
    // Triple Buffering (Ring Buffer) -> Now 32 to prevent aliasing with large preroll queues
    static const int kBufferCount = 32;
    
    ID3D11Texture2D* m_textures[kBufferCount] = { nullptr };
    ID3D11ShaderResourceView* m_textureSRVs[kBufferCount] = { nullptr };
    
    struct TextureEntry {
        ID3D11ShaderResourceView* srv;
        std::chrono::steady_clock::time_point timestamp;
    };

    // The history of recent textures for decoupled retrieval
    std::queue<TextureEntry> m_readyTextures;
    ID3D11ShaderResourceView* m_lastTop = nullptr;
    ID3D11ShaderResourceView* m_lastBottom = nullptr;
    bool m_isConsuming = false;
    bool m_hadStarvation = false; // Tracks if we hit size==1 recently
    int m_prerollDelay = 3; // DeckLink cycles to wait before consuming
    
    std::atomic<int> m_droppedFrames{0};
    std::atomic<int> m_duplicatedFrames{0};

    std::mutex m_mutex;
    
    // CPU Ring Buffers
    std::vector<uint8_t> m_cpuBuffers[kBufferCount];
    std::chrono::steady_clock::time_point m_timestamps[kBufferCount];

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
