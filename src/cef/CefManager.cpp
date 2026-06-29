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
    settings.log_severity = LOGSEVERITY_WARNING;
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

void CefManager::CreateBrowser(HWND parentHwnd, const std::string& url, ID3D11Device* device) {
    m_parentHwnd = parentHwnd;
    m_initialUrl = url;
    m_d3dDevice = device;

    // If already initialized, create immediately. 
    // Otherwise, OnContextInitialized will handle it.
    if (m_initialized) {
        ExecuteCreateBrowser();
    }
}

void CefManager::ExecuteCreateBrowser() {
    CefWindowInfo window_info;
    window_info.SetAsWindowless(m_parentHwnd); 
    window_info.external_begin_frame_enabled = true; // Driven by DeckLink

    // Create Render Handler
    m_renderHandler = new CefRenderHandlerImpl(m_d3dDevice);
    
    // Create Client
    CefRefPtr<CefClient> client = new CefClientImpl(this, m_renderHandler);
    
    // Browser Settings
    CefBrowserSettings browser_settings;
    // Set to 128 to ensure no internal throttling
    browser_settings.windowless_frame_rate = 128; 
    
    // Create Browser
    CefBrowserHost::CreateBrowser(window_info, client, m_initialUrl, browser_settings, nullptr, nullptr);
}

void CefManager::DriveExternalBeginFrame(int mode) {
    if (!m_browser) return;

    CefRefPtr<CefTaskRunner> runner = CefTaskRunner::GetForThread(TID_UI);
    if (!runner) return;

    // Frame 1: Trigger immediately
    runner->PostTask(new FunctionTask([this]{ 
        if (m_browser) {
            m_browser->GetHost()->SendExternalBeginFrame();
            m_browser->GetHost()->Invalidate(PET_VIEW);
        }
    }));

    // If Mode is not 3 (Blend Mode), we need a 59.94fps rate.
    // DeckLink calls this at 29.97Hz (every ~33.36ms).
    // So we schedule the second frame 16ms later.
    if (mode != 3) {
        runner->PostDelayedTask(new FunctionTask([this]{ 
            if (m_browser) {
                m_browser->GetHost()->SendExternalBeginFrame();
                m_browser->GetHost()->Invalidate(PET_VIEW);
            }
        }), 16);
    }
}

void CefManager::SetBrowser(CefRefPtr<CefBrowser> browser) {
    m_browser = browser;
    // We no longer start an independent pacing loop here.
    // DeckLink will drive CEF via DriveExternalBeginFrame.
    if (m_browser) {
        m_browser->GetHost()->WasHidden(false);
        m_browser->GetHost()->WasResized();
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
