#include "CefManager.h"
#include "CefRenderHandler.h"
#include <iostream>
#include <include/cef_task.h> // For CefPostTask

// Helper for CefPostTask
class FunctionTask : public CefTask {
public:
    using Callback = std::function<void()>;
    FunctionTask(Callback callback) : m_callback(callback) {}
    void Execute() override { m_callback(); }
    IMPLEMENT_REFCOUNTING(FunctionTask);
private:
    Callback m_callback;
};

// Minimal Client with LifeSpanHandler & CefDisplayHandler
class CefClientImpl : public CefClient,
                      public CefLifeSpanHandler,
                      public CefDisplayHandler {
public:
    CefClientImpl(CefManager* manager, CefRefPtr<CefRenderHandler> renderHandler) 
        : m_manager(manager), m_renderHandler(renderHandler) {}
    
    CefRefPtr<CefRenderHandler> GetRenderHandler() override { return m_renderHandler; }
    CefRefPtr<CefLifeSpanHandler> GetLifeSpanHandler() override { return this; }
    CefRefPtr<CefDisplayHandler> GetDisplayHandler() override { return this; }
    
    // CefDisplayHandler methods
    void OnFullscreenModeChange(CefRefPtr<CefBrowser> browser, bool fullscreen) override {
        if (m_manager) {
            m_manager->TriggerFullscreen(fullscreen);
        }
    }
    
    void OnAfterCreated(CefRefPtr<CefBrowser> browser) override {
        if (m_manager) m_manager->SetBrowser(browser);
    }

private:
    CefManager* m_manager;
    CefRefPtr<CefRenderHandler> m_renderHandler;
    IMPLEMENT_REFCOUNTING(CefClientImpl);
};

// --- Custom Scheme Handler ---
// For now, we'll just define the factory scaffolding. 
// Implementing a full file reader for 'telop://' requires CefResourceHandler implementation.
// We will register standard file scheme or just http/https for now as a placeholder unless specifically asked to fully impl.
// The user asked to "Start with it", so we add the registrations.

// Application Handler
class CefAppImpl : public CefApp, public CefBrowserProcessHandler {
public:
    CefAppImpl(CefManager* manager) : m_manager(manager) {}
    
    CefRefPtr<CefBrowserProcessHandler> GetBrowserProcessHandler() override {
        return this;
    }
    
    void OnContextInitialized() override;

    void OnBeforeCommandLineProcessing(const CefString& process_type, CefRefPtr<CefCommandLine> command_line) override {
        // Disable VSync to allow unlimited frame rate (controlled by our external trigger)
        command_line->AppendSwitch("disable-gpu-vsync");
        command_line->AppendSwitch("disable-frame-rate-limit");
        
        // Prevent throttling when window is hidden/background
        command_line->AppendSwitch("disable-renderer-backgrounding");
        command_line->AppendSwitch("disable-background-timer-throttling");
        command_line->AppendSwitch("disable-backgrounding-occluded-windows");
        command_line->AppendSwitchWithValue("disable-features", "CalculateNativeWinOcclusion,IntensiveWakeUpThrottling,WebBluetooth,HardwareMediaKeyHandling");

        // Autoplay and audio context policies
        command_line->AppendSwitchWithValue("autoplay-policy", "no-user-gesture-required");
        command_line->AppendSwitch("no-user-gesture-required");

        // Local file access and security relaxation for overlays
        command_line->AppendSwitch("disable-web-security");
        command_line->AppendSwitch("allow-file-access-from-files");

        // UI cleaning and optimization
        command_line->AppendSwitch("hide-scrollbars");
        command_line->AppendSwitch("disable-overlay-scrollbar");
        command_line->AppendSwitch("disable-extensions");

        // GPU rasterization: force hardware rasterization path.
        // Avoids software (CPU) rasterization which causes higher first-frame latency
        // and is the root cause of animation start jitter in CEF windowless mode.
        command_line->AppendSwitch("enable-gpu-rasterization");
        command_line->AppendSwitch("ignore-gpu-blocklist");
    }

private:
    CefManager* m_manager;
    IMPLEMENT_REFCOUNTING(CefAppImpl);
};

CefManager::CefManager() {
}

CefManager::~CefManager() {
    Shutdown();
}

bool CefManager::Initialize(HINSTANCE hInstance) {
    CefMainArgs main_args(hInstance);
    
    // CEF App
    CefRefPtr<CefApp> app = new CefAppImpl(this);

    // Execute the sub-process logic. This checks the command line to see if we are a sub-process.
    // If not, it returns -1 and we proceed to initialize the main browser process.
    int exit_code = CefExecuteProcess(main_args, app, nullptr);
    if (exit_code >= 0) {
        return false; // Suppress further initialization in sub-process
    }

    CefSettings settings;
    settings.no_sandbox = true;
    settings.windowless_rendering_enabled = true;
    settings.log_severity = LOGSEVERITY_FATAL; // Suppress GCM DEPRECATED_ENDPOINT and other harmless Chromium errors
    settings.remote_debugging_port = 9222;

    // Set cache path for persistent features and DevTools stability
    // Must be absolute path
    char exePath[MAX_PATH];
    if (GetModuleFileNameA(nullptr, exePath, MAX_PATH) > 0) {
        std::string sPath(exePath);
        size_t pos = sPath.find_last_of("\\/");
        if (pos != std::string::npos) {
            sPath = sPath.substr(0, pos);
        }
        std::string cachePath = sPath + "\\cache";
        CefString(&settings.cache_path).FromString(cachePath);
        CefString(&settings.root_cache_path).FromString(cachePath);
    }
    
    // Important for transparent background
    settings.background_color = 0x00000000; 

    // Reverted: multi_threaded_message_loop triggered AppLocker block.
    // settings.multi_threaded_message_loop = true;

    if (!CefInitialize(main_args, settings, app, nullptr)) {
        return false;
    }
    
    // Register Scheme (Placeholder for telop://)
    // CefRegisterSchemeHandlerFactory("telop", "editor", new MySchemeHandlerFactory());

    m_initialized = true;
    return true;
}

void CefManager::DoMessageLoopWork() {
    CefDoMessageLoopWork();
}

void CefManager::Shutdown() {
    if (m_initialized) {
        CefShutdown();
        m_initialized = false;
    }
}

void CefManager::CreateBrowser(HWND parentHwnd, const std::string& url, ID3D11Device* device, const std::string& format) {
    m_parentHwnd = parentHwnd;
    m_initialUrl = url;
    m_d3dDevice = device;
    m_format = format;

    // If already initialized, create immediately. 
    // Otherwise, OnContextInitialized will handle it.
    if (m_initialized) {
        ExecuteCreateBrowser();
    }
}

void CefManager::ExecuteCreateBrowser() {
    CefWindowInfo window_info;
    window_info.SetAsWindowless(m_parentHwnd); 
    window_info.external_begin_frame_enabled = false; // Free-running, like CasparCG

    // Create Render Handler
    m_renderHandler = new CefRenderHandlerImpl(m_d3dDevice);
    
    // Create Client
    CefRefPtr<CefClient> client = new CefClientImpl(this, m_renderHandler);
    
    // Browser Settings
    CefBrowserSettings browser_settings;
    
    int fps = 60;
    if (m_format == "50i") fps = 50;
    
    // Set transparent background and windowless frame rate (e.g. 60 or 50)
    browser_settings.background_color = 0x00000000;
    browser_settings.windowless_frame_rate = fps; 
    
    // Create Browser
    CefBrowserHost::CreateBrowser(window_info, client, m_initialUrl, browser_settings, nullptr, nullptr);
}

void CefManager::DriveExternalBeginFrame(int mode) {
    // No longer driven externally. 
    // CEF is now free-running via its internal timer at 60fps.
}

void CefManager::SetBrowser(CefRefPtr<CefBrowser> browser) {
    m_browser = browser;
    // We no longer start an independent pacing loop here.
    // DeckLink will drive CEF via DriveExternalBeginFrame.
    if (m_browser) {
        m_browser->GetHost()->WasHidden(false);
        m_browser->GetHost()->WasResized();

        // Compositor warm-up: inject a persistent rAF loop into the page.
        // In CEF windowless mode, Chromium's compositor thread goes idle when
        // content is static. When an animation starts, the compositor needs
        // 1-3 frames (~16-50ms) to "wake up", causing visible jitter at the
        // start of animations. Keeping a continuous rAF loop running prevents
        // the compositor from ever going idle, so animations start instantly.
        // Trade-off: slight increase in GPU/CPU usage while page is static.
        m_browser->GetMainFrame()->ExecuteJavaScript(
            "(function keepCompositorWarm() {"
            "  requestAnimationFrame(keepCompositorWarm);"
            "})();"

            // GPU layer retention: prevent compositor layer teardown at animation end.
            // When a CSS animation/transition ends, Chromium destroys the promoted
            // GPU compositor layer for that element, causing a repaint that manifests
            // as a 1-frame jitter at the end of animations (visible even with linear easing).
            // Setting will-change after animationend/transitionend keeps the layer alive,
            // eliminating the teardown repaint entirely.
            "document.addEventListener('animationend', function(e) {"
            "  e.target.style.willChange = 'transform, opacity';"
            "}, true);"
            "document.addEventListener('transitionend', function(e) {"
            "  e.target.style.willChange = 'transform, opacity';"
            "}, true);",
            "about:blank", 0
        );
    }
}

void CefManager::SetOnFullscreenCallback(FullscreenCallback callback) {
    m_fullscreenCallback = callback;
}

void CefManager::TriggerFullscreen(bool fullscreen) {
    if (m_fullscreenCallback) {
        m_fullscreenCallback(fullscreen);
    }
}

void CefManager::Resize(int width, int height) {
    if (m_renderHandler) {
        m_renderHandler->Resize(width, height);
    }
    if (m_browser) {
        m_browser->GetHost()->WasResized();
    }
}


void CefAppImpl::OnContextInitialized() {
    if (m_manager) {
        m_manager->ExecuteCreateBrowser();
    }
}
