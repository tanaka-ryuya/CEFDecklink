#pragma once
#include <include/cef_render_handler.h>
#include <mutex>
#include <vector>
#include <d3d11.h>

class CefRenderHandlerImpl : public CefRenderHandler {
public:
    CefRenderHandlerImpl(ID3D11Device* device);
    ~CefRenderHandlerImpl();

    // CefRenderHandler methods
    void GetViewRect(CefRefPtr<CefBrowser> browser, CefRect& rect) override;
    void OnPaint(CefRefPtr<CefBrowser> browser, PaintElementType type, const RectList& dirtyRects, const void* buffer, int width, int height) override;

    // Custom methods for DX11 Interop
    // Returns the SRV of the texture containing the latest web frame
    ID3D11ShaderResourceView* GetTextureSRV();
    
    // Set the expected size
    void Resize(int width, int height);

    // Call from Main Thread to upload pending data
    void SyncWithGPU();

private:
    int m_width = 1920;
    int m_height = 1080;
    ID3D11Device* m_device = nullptr;
    ID3D11Texture2D* m_texture = nullptr;
    ID3D11ShaderResourceView* m_textureSRV = nullptr;
    
    std::mutex m_mutex;
    
    // Thread safety
    std::vector<uint8_t> m_cpuBuffer;
    bool m_cpuBufferHasNewData = false;

    // Use IMPLEMENT_REFCOUNTING macro
    IMPLEMENT_REFCOUNTING(CefRenderHandlerImpl);
};
