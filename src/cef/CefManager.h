#pragma once
#include <include/cef_app.h>
#include <include/cef_client.h>
#include <include/cef_render_handler.h>
#include <functional>

// Forward declaration
class CefRenderHandlerImpl;
struct ID3D11Device;

using FullscreenCallback = std::function<void(bool)>;

class CefManager {
public:
    CefManager();
    ~CefManager();

    // Initialize CEF with windowless rendering enabled
    bool Initialize(HINSTANCE hInstance);

    // Call this in the main loop
    void DoMessageLoopWork();

    // Shutdown CEF
    void Shutdown();

    // Create a browser
    void CreateBrowser(HWND parentHwnd, const std::string& url, ID3D11Device* device, const std::string& format = "5994i");
    void ExecuteCreateBrowser();

    void SetBrowser(CefRefPtr<CefBrowser> browser);

    // Callbacks
    void SetOnFullscreenCallback(FullscreenCallback callback);
    void TriggerFullscreen(bool fullscreen);
    
    // Resize
    void Resize(int width, int height);

    // Access to render handler for texture access
    CefRefPtr<CefRenderHandlerImpl> GetRenderHandler() const { return m_renderHandler; }

    // Externally drive CEF frame generation from DeckLink thread
    void DriveExternalBeginFrame(int mode);

private:
    CefRefPtr<CefRenderHandlerImpl> m_renderHandler;
    CefRefPtr<CefBrowser> m_browser;
    bool m_initialized = false;

    // Deferred Creation Data
    HWND m_parentHwnd = nullptr;
    std::string m_initialUrl;
    std::string m_format;
    ID3D11Device* m_d3dDevice = nullptr;
    FullscreenCallback m_fullscreenCallback;
};
