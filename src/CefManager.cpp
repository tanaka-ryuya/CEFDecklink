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

// Minimal Client with LifeSpanHandler
class CefClientImpl : public CefClient, public CefLifeSpanHandler {
public:
    CefClientImpl(CefManager* manager, CefRefPtr<CefRenderHandler> renderHandler) 
        : m_manager(manager), m_renderHandler(renderHandler) {}
    
    CefRefPtr<CefRenderHandler> GetRenderHandler() override { return m_renderHandler; }
    CefRefPtr<CefLifeSpanHandler> GetLifeSpanHandler() override { return this; }
    
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
    // Set to 120 to prevent CEF from throttling 59.94 internal loops logic (safeguard)
    browser_settings.windowless_frame_rate = 120; 
    
    // Create Browser
    CefBrowserHost::CreateBrowser(window_info, client, m_initialUrl, browser_settings, nullptr, nullptr);
}

void CefManager::ScheduleFrames() {
    if (!m_initialized) return;

    // Trigger 1st Frame (Immediate)
    // Execute on UI Thread
    CefRefPtr<CefTaskRunner> runner = CefTaskRunner::GetForThread(TID_UI);
    if (!runner) return;

    // Use a simple static helper or lambda if wrapper supports it. 
    // CEF C++ wrapper CefTask is usually an interface. 
    // We can use CefCreateClosureTask with std::function/lambda.
    
    runner->PostTask(new FunctionTask([this]{ TriggerBeginFrame(); }));
    
    // Trigger 2nd Frame (Target interval for 59.94p is ~16.68ms)
    // We use 14ms to ensure we don't overshoot if system is slightly busy.
    // Timer resolution is improved by timeBeginPeriod(1).
    runner->PostDelayedTask(new FunctionTask([this]{ TriggerBeginFrame(); }), 14);
}

void CefManager::TriggerBeginFrame() {
    if (m_browser) {
        m_browser->GetHost()->SendExternalBeginFrame();
        // Force redraw even if content thinks it is static (important for video sync)
        m_browser->GetHost()->Invalidate(PET_VIEW);
    }
}

void CefManager::SetBrowser(CefRefPtr<CefBrowser> browser) {
    m_browser = browser;
}


void CefAppImpl::OnContextInitialized() {
    if (m_manager) {
        m_manager->ExecuteCreateBrowser();
    }
}
