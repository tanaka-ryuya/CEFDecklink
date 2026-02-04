#include "CefManager.h"
#include "CefRenderHandler.h"
#include <iostream>

// Minimal Client
class CefClientImpl : public CefClient {
public:
    CefClientImpl(CefRefPtr<CefRenderHandler> renderHandler) : m_renderHandler(renderHandler) {}
    
    CefRefPtr<CefRenderHandler> GetRenderHandler() override {
        return m_renderHandler;
    }

private:
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
    CefString(&settings.cache_path).FromASCII("cache");
    CefString(&settings.root_cache_path).FromASCII("cache");
    
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
    
    // Create Render Handler
    m_renderHandler = new CefRenderHandlerImpl(m_d3dDevice);
    
    // Create Client
    CefRefPtr<CefClient> client = new CefClientImpl(m_renderHandler);
    
    // Browser Settings
    CefBrowserSettings browser_settings;
    browser_settings.windowless_frame_rate = 60; // Sync with DeckLink 59.94i/60p
    
    // Create Browser
    CefBrowserHost::CreateBrowser(window_info, client, m_initialUrl, browser_settings, nullptr, nullptr);
}

void CefAppImpl::OnContextInitialized() {
    if (m_manager) {
        m_manager->ExecuteCreateBrowser();
    }
}
