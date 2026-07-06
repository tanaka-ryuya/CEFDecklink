#pragma once
#include <include/cef_render_handler.h>
#include <mutex>
#include <vector>
#include <atomic>
#include <chrono>
#include <queue>

#ifdef _WIN32
#include <d3d11.h>
typedef ID3D11ShaderResourceView* CefFrameResource;
#else
struct MacFrameResource {
    std::vector<uint8_t> buffer;
    int refCount = 1;
    void AddRef() { refCount++; }
    void Release() {
        if (--refCount <= 0) delete this;
    }
};
typedef MacFrameResource* CefFrameResource;
#endif

class CefRenderHandlerImpl : public CefRenderHandler {
public:
#ifdef _WIN32
    CefRenderHandlerImpl(ID3D11Device* device);
#else
    CefRenderHandlerImpl();
#endif
    ~CefRenderHandlerImpl();

    // CefRenderHandler methods
    void GetViewRect(CefRefPtr<CefBrowser> browser, CefRect& rect) override;
    void OnPaint(CefRefPtr<CefBrowser> browser, PaintElementType type, const RectList& dirtyRects, const void* buffer, int width, int height) override;

    // Custom methods for interop
    CefFrameResource GetTextureSRV();
    
    // Set the expected size
    void Resize(int width, int height);

    // Call from Main Thread to upload pending data
    void SyncWithGPU();

    // Get the two most recent distinct textures
    // Retreives a synchronized pair of frames for interlaced output, applying stutter prevention logic.
    void GetSynchronizedTextures(CefFrameResource* srvTop, CefFrameResource* srvBottom);

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
    static const int kBufferCount = 32;
    int m_width = 1920;
    int m_height = 1080;

#ifdef _WIN32
    ID3D11Device* m_device = nullptr;
    ID3D11Texture2D* m_textures[kBufferCount] = { nullptr };
    ID3D11ShaderResourceView* m_textureSRVs[kBufferCount] = { nullptr };
#else
    CefFrameResource m_textures[kBufferCount] = { nullptr };
#endif
    
    struct TextureEntry {
        CefFrameResource srv;
        std::chrono::steady_clock::time_point timestamp;
    };

    // The history of recent textures for decoupled retrieval
    std::queue<TextureEntry> m_readyTextures;
    CefFrameResource m_lastTop = nullptr;
    CefFrameResource m_lastBottom = nullptr;
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
