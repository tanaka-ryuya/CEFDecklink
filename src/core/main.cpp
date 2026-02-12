#include <Windows.h>
#include <shellapi.h>
#include <tchar.h>
#include <d3d11.h>
#include <d3d11_1.h>
#include <dxgi.h>
#include <vector>
#include <iostream>
#include <iomanip>
#include <chrono>
#include <thread>
#include <mmsystem.h>
#pragma comment(lib, "winmm.lib")

#include "DeckLinkDevice.h"

#include "CefManager.h"
#include "CefRenderHandler.h"
#include "ShaderManager.h"
#include "CrashHandler.h"

// Data
static ID3D11Device*            g_pd3dDevice = nullptr;
static ID3D11DeviceContext*     g_pd3dDeviceContext = nullptr;
static IDXGISwapChain*          g_pSwapChain = nullptr;
static ID3D11RenderTargetView*  g_mainRenderTargetView = nullptr;
static bool                     g_appDone = false;

// Managers & Devices (Global)
static CefManager g_cefManager;
static std::unique_ptr<ShaderManager> g_shaderManager;
static DeckLinkDevice g_deckLink;
static std::atomic<int> g_viewMode(0); // 0=Interlace, 1=Diff, 2=Progressive

// Forward declarations of helper functions
IDXGIAdapter* SelectBestAdapter();
bool CreateDeviceD3D(HWND hWnd);
void CleanupDeviceD3D();
void CreateRenderTarget();
void CleanupRenderTarget();
LRESULT WINAPI WndProc(HWND hWnd, UINT msg, WPARAM wParam, LPARAM lParam);
void ToggleFullscreen(HWND hWnd);
BOOL WINAPI ConsoleHandler(DWORD ctrlType);

// Console Output Helper
void LogStatus(bool locked, double deckLinkFps, int cefFps, float alphaThreshold, uint64_t totalCefFrames) {
    static auto lastTime = std::chrono::steady_clock::now();
    static uint64_t lastTotal = 0;
    auto now = std::chrono::steady_clock::now();
    
    // Log every 1 second
    if (std::chrono::duration_cast<std::chrono::milliseconds>(now - lastTime).count() > 1000) {
        uint64_t diff = totalCefFrames - lastTotal;
        lastTotal = totalCefFrames;

        std::cout << "\r" // Carriage return to overwrite line
                  << "[Status] Sync: " << (locked ? "LOCKED" : "SEARCHING")
                  << " | DL: " << std::fixed << std::setprecision(2) << deckLinkFps << " fps"
                  << " | CEF: " << cefFps << " fps (" << diff << " unique)"
                  << " | Mode: " << g_viewMode.load()
                  << " | Alpha: " << std::fixed << std::setprecision(4) << alphaThreshold
                  << " | Total: " << totalCefFrames;
        
        // Extended Sync info
        std::cout << " | Ctrl+C to Exit.   " // Extra spaces to clear line
                  << std::flush;
        lastTime = now;
    }
}

#include <iostream>
#include <fstream>
#include <string>
#include <algorithm>

// ... (existing includes)

// Global Configuration
static float g_alphaThreshold = 0.01f;

// Helper to load config.json
bool LoadConfig(std::string& url, float& alpha) {
    wchar_t exePath[MAX_PATH];
    GetModuleFileNameW(nullptr, exePath, MAX_PATH);
    std::wstring exeDir = exePath;
    size_t lastSlash = exeDir.find_last_of(L"\\/");
    if (lastSlash != std::wstring::npos) {
        exeDir = exeDir.substr(0, lastSlash);
    }
    
    std::wstring configPath = exeDir + L"\\config.json";
    std::ifstream file(configPath);
    if (!file.is_open()) return false;

    std::string content((std::istreambuf_iterator<char>(file)), std::istreambuf_iterator<char>());
    
    // Simple JSON parsing logic
    auto ParseString = [&](const std::string& key) -> std::string {
        size_t keyPos = content.find("\"" + key + "\"");
        if (keyPos == std::string::npos) return "";
        
        size_t colonPos = content.find(":", keyPos);
        if (colonPos == std::string::npos) return "";

        size_t startQuote = content.find("\"", colonPos);
        if (startQuote == std::string::npos) return "";
        
        size_t endQuote = content.find("\"", startQuote + 1);
        if (endQuote == std::string::npos) return "";

        return content.substr(startQuote + 1, endQuote - startQuote - 1);
    };

    auto ParseFloat = [&](const std::string& key) -> float {
        size_t keyPos = content.find("\"" + key + "\"");
        if (keyPos == std::string::npos) return -1.0f;

        size_t colonPos = content.find(":", keyPos);
        if (colonPos == std::string::npos) return -1.0f;

        size_t valueStart = content.find_first_not_of(" \t\n\r", colonPos + 1);
        if (valueStart == std::string::npos) return -1.0f;
        
        size_t valueEnd = content.find_first_of(",}", valueStart);
        if (valueEnd == std::string::npos) return -1.0f;

        std::string valStr = content.substr(valueStart, valueEnd - valueStart);
        try {
            return std::stof(valStr);
        } catch (...) {
            return -1.0f;
        }
    };

    // Load URL
    std::string parsedUrl = ParseString("url");
    if (!parsedUrl.empty()) {
        url = parsedUrl;
    }

    // Load Alpha
    float parsedAlpha = ParseFloat("alpha");
    if (parsedAlpha >= 0.0f) {
        alpha = parsedAlpha;
    }
    
    return true;
}

// ... (rest of main)

// RenderFrame now only handles Main Thread tasks: Input processing & CEF Message Loop & Logging
void RenderFrame(HWND hWnd) {
    // --- Keyboard Input for Alpha Threshold Adjustment & Fullscreen ---
    // static float alphaThreshold = 0.01f; // REMOVED: Using Global g_alphaThreshold
    static auto lastKeyTime = std::chrono::steady_clock::now();
    auto now = std::chrono::steady_clock::now();
    auto timeSinceLastKey = std::chrono::duration_cast<std::chrono::milliseconds>(now - lastKeyTime).count();
    
    // Allow adjustment every 200ms when key is held to prevent bouncing
    if (timeSinceLastKey > 200) {
        bool changed = false;
        
        if (GetAsyncKeyState(VK_F11) & 0x8000) {
            ToggleFullscreen(hWnd);
            changed = true;
            lastKeyTime = now;
        }
        if (GetAsyncKeyState('D') & 0x8000) {
            g_viewMode.store(1); // Diff Mode
            changed = true;
            lastKeyTime = now;
        }
        if (GetAsyncKeyState('P') & 0x8000) {
            g_viewMode.store(2); // Progressive Mode
            changed = true;
            lastKeyTime = now;
        }
        if (GetAsyncKeyState('I') & 0x8000) {
            g_viewMode.store(0); // Interlace Mode
            changed = true;
            lastKeyTime = now;
        }

        if (GetAsyncKeyState(VK_OEM_PLUS) & 0x8000 || GetAsyncKeyState(0xBB) & 0x8000) { // + key
            g_alphaThreshold += 0.001f;
            if (g_alphaThreshold > 1.0f) g_alphaThreshold = 1.0f;
            changed = true;
            lastKeyTime = now;
        }
        if (GetAsyncKeyState(VK_OEM_MINUS) & 0x8000 || GetAsyncKeyState(0xBD) & 0x8000) { // - key
            g_alphaThreshold -= 0.001f;
            if (g_alphaThreshold < 0.0f) g_alphaThreshold = 0.0f;
            changed = true;
            lastKeyTime = now;
        }
        
        if (changed && g_shaderManager) {
            // NOTE: ShaderManager Access from Main Thread while DeckLink accesses it from Callback Thread!
            // ShaderManager should use D3D context locking if not done already.
            g_shaderManager->SetAlphaThreshold(g_alphaThreshold);
        }
    }

    // CEF Message Loop
    g_cefManager.DoMessageLoopWork();

    // --- Synchronization - Wait for DeckLink callback just for tracking FPS ---
    bool deckLinkReady = g_deckLink.WaitForNextFrame(0);
    
    // Console Logging (Actual FPS calculation)
    static int frameCount = 0;
    static auto lastLogTime = std::chrono::steady_clock::now();
    
    if (deckLinkReady) {
        frameCount++;
    }

    now = std::chrono::steady_clock::now();
    if (std::chrono::duration_cast<std::chrono::milliseconds>(now - lastLogTime).count() > 1000) {
        double fps = (double)frameCount; 
        
        // Get CEF Rate
        int cefFps = 0;
        auto handler = g_cefManager.GetRenderHandler();
        uint64_t totalCef = 0;
        if (handler) {
            cefFps = handler->GetAndResetFrameCount();
            totalCef = handler->GetTotalFrameCount();
        }

        LogStatus(true, fps, cefFps, g_alphaThreshold, totalCef);
        
        frameCount = 0;
        lastLogTime = now;
    }
}

// Main code
int main(int argc, char** argv)
{
    // 0. Configuration Logic
    std::string targetUrl = "http://localhost:9090/graphics/on_air.html";
    
    // Priority 3: Default (Set above)
    
    // Priority 2: config.json
    LoadConfig(targetUrl, g_alphaThreshold);
    
    // Priority 1: CLI Args
    for (int i = 1; i < argc; ++i) {
        std::string arg = argv[i];
        if (arg == "--url" && i + 1 < argc) {
            targetUrl = argv[++i];
        } else if (arg == "--alpha" && i + 1 < argc) {
            try {
                g_alphaThreshold = std::stof(argv[++i]);
            } catch (...) {}
        }
    }
    
    std::cout << "[Config] URL: " << targetUrl << std::endl;
    std::cout << "[Config] Alpha: " << g_alphaThreshold << std::endl;

    // 1. CEF Sub-process check (MUST be the absolute first thing)
    CefMainArgs main_args(GetModuleHandle(nullptr));
    int exit_code = CefExecuteProcess(main_args, nullptr, nullptr);
    if (exit_code >= 0) {
        return exit_code;
    }
    
    // ... (Continue initialization)

    std::cout << "--- DeckLink + CEF CUI Application ---" << std::endl;
    std::cout << "Initializing..." << std::endl;

    // Initialize Crash Handler for debugging
    CrashHandler::Initialize();

    // Register Console Control Handler for clean exit on Ctrl+C
    SetConsoleCtrlHandler(ConsoleHandler, TRUE);

    // High Resolution Timer for accurate 60fps pacing
    timeBeginPeriod(1);

    // Boost Priority to Real-Time-ish (High) to avoid scheduler starvation
    SetPriorityClass(GetCurrentProcess(), HIGH_PRIORITY_CLASS);

    // Initialize CEF early
    if (!g_cefManager.Initialize(GetModuleHandle(nullptr))) {
        std::cerr << "Failed to initialize CEF." << std::endl;
        return 1;
    }

    // Create application window (Hidden)
    WNDCLASSEXW wc = { sizeof(wc), CS_CLASSDC, WndProc, 0L, 0L, GetModuleHandle(nullptr), nullptr, nullptr, nullptr, nullptr, L"DeckLinkApp", nullptr };
    ::RegisterClassExW(&wc);
    // Use SW_HIDE behavior by simply NOT showing it, or strictly SW_HIDE. 
    // We create it as OVERLAPPEDWINDOW but standard size.
    HWND hwnd = ::CreateWindowW(wc.lpszClassName, L"Native DeckLink + CEF", WS_OVERLAPPEDWINDOW, 100, 100, 1280, 800, nullptr, nullptr, wc.hInstance, nullptr);

    // Initialize Direct3D
    if (!CreateDeviceD3D(hwnd))
    {
        CleanupDeviceD3D();
        ::UnregisterClassW(wc.lpszClassName, wc.hInstance);
        return 1;
    }

    // Initialize Shader Manager
    g_shaderManager = std::make_unique<ShaderManager>(g_pd3dDevice, g_pd3dDeviceContext);
    if (!g_shaderManager->Initialize(1920, 1080)) {
        std::cerr << "Failed to initialize Shader Manager." << std::endl;
    }
    
    // Apply initial configuration
    g_shaderManager->SetAlphaThreshold(g_alphaThreshold);

    // Initialize DeckLink
    if (g_deckLink.Initialize())
    {
        std::cout << "DeckLink Initialized." << std::endl;

        // --- Register Render Callback (Reference Pattern) ---
        // This runs INSIDE the DeckLink thread/callback
        g_deckLink.SetRenderCallback([](void* pBuffer) {
            // Helper Lambda for Blitting to Window
            auto BlitToWindow = [&](void* buffer) {
                 if (g_deckLink.IsSimulated() && buffer) {
                     HWND previewHwnd = FindWindowW(L"DeckLinkApp", L"Native DeckLink + CEF");
                     if (previewHwnd) {
                         HDC hdc = GetDC(previewHwnd);
                         if (hdc) {
                             RECT rcClient;
                             GetClientRect(previewHwnd, &rcClient);
                             int winW = rcClient.right - rcClient.left;
                             int winH = rcClient.bottom - rcClient.top;

                             BITMAPINFO bmi = {0};
                             bmi.bmiHeader.biSize = sizeof(BITMAPINFOHEADER);
                             bmi.bmiHeader.biWidth = 1920;
                             bmi.bmiHeader.biHeight = -1080; // Top-down
                             bmi.bmiHeader.biPlanes = 1;
                             bmi.bmiHeader.biBitCount = 32;
                             bmi.bmiHeader.biCompression = BI_RGB;
                             
                             SetStretchBltMode(hdc, COLORONCOLOR);
                             StretchDIBits(hdc, 
                                 0, 0, winW, winH,          // Destination
                                 0, 0, 1920, 1080,          // Source
                                 buffer, &bmi, DIB_RGB_COLORS, SRCCOPY);
                                 
                             ReleaseDC(previewHwnd, hdc);
                         }
                     }
                 }
            };
            
            static uint64_t totalConsumedFrames = 0;
            ID3D11ShaderResourceView* srvTop = nullptr;
            ID3D11ShaderResourceView* srvBottom = nullptr;

            auto renderHandler = g_cefManager.GetRenderHandler();
            
            if (renderHandler) {
                // 1. Drain all pending frames from the CEF queue to GPU textures
                while (renderHandler->HasPendingFrames(1)) {
                    renderHandler->SyncWithGPU();
                }

                // 2. Fetch the two most recent distinct textures for synthesis
                renderHandler->GetLatestTextures(&srvTop, &srvBottom);
            }

            // Fallback: If we only got one frame, duplicate it.
            if (srvTop && !srvBottom) { srvBottom = srvTop; srvBottom->AddRef(); }
            if (!srvTop && srvBottom) { srvTop = srvBottom; srvTop->AddRef(); }

            // --- Rendering Logic ---
            int currentMode = g_viewMode.load();

            if (currentMode == 2 && g_deckLink.IsSimulated()) {
                // === Mode 2: Progressive Double-Pump (59.94p Window Output - DEBUG ONLY) ===
                if (pBuffer && g_shaderManager && srvTop && srvBottom) {
                    // Pass 1: Render Frame 1 (Top Field Source)
                    g_shaderManager->SetViewMode(2); 
                    g_shaderManager->ConvertAndDownload(srvTop, srvBottom, pBuffer);
                    BlitToWindow(pBuffer); // Show Frame 1

                    // Wait ~16.6ms to simulate 60fps pacing
                    std::this_thread::sleep_for(std::chrono::milliseconds(16));

                    // Pass 2: Render Frame 2 (Bottom Field Source)
                    g_shaderManager->SetViewMode(3);
                    g_shaderManager->ConvertAndDownload(srvTop, srvBottom, pBuffer);
                    BlitToWindow(pBuffer); // Show Frame 2
                }
            } else {
                // === Mode 0/1/2/3: Standard Logic ===
                if (pBuffer) {
                    if (srvTop && srvBottom && g_shaderManager) {
                        g_shaderManager->SetViewMode(currentMode);
                        g_shaderManager->ConvertAndDownload(srvTop, srvBottom, pBuffer);
                        BlitToWindow(pBuffer); // Blit once
                    } else if (g_shaderManager) {
                         memset(pBuffer, 0, 1920 * 1080 * 4);
                    }
                }
            }
            
            // Release References
            if (srvTop) srvTop->Release();
            if (srvBottom) srvBottom->Release();
        });

        g_deckLink.StartOutput();

        if (g_deckLink.IsSimulated()) {
            // Show window for Preview in Simulator Mode
            ::ShowWindow(hwnd, SW_SHOW);
            ::UpdateWindow(hwnd);
        } else {
            ::ShowWindow(hwnd, SW_HIDE);
        }
    }
    else
    {
        std::cerr << "Failed to initialize DeckLink!" << std::endl; 
    }

    // Create CEF Browser
    // Note: URL from Config/CLI/Default
    g_cefManager.CreateBrowser(hwnd, targetUrl, g_pd3dDevice);

    // Register Fullscreen Callback
    g_cefManager.SetOnFullscreenCallback([hwnd](bool fullscreen) {
        ToggleFullscreen(hwnd);
    });

    std::cout << "Starting Main Loop..." << std::endl;

    // Main loop
    try {
        while (!g_appDone)
        {
            // Poll Windows Messages
            MSG msg;
            while (::PeekMessage(&msg, nullptr, 0U, 0U, PM_REMOVE))
            {
                ::TranslateMessage(&msg);
                ::DispatchMessage(&msg);
                if (msg.message == WM_QUIT)
                    g_appDone = true;
            }
            if (g_appDone) break;

            // Update user input (Main Thread)
            RenderFrame(hwnd); // This now just handles input and logging
        }
    }
    catch (const std::exception& e) {
        std::cerr << "\n[EXCEPTION] Caught C++ exception in main loop: " << e.what() << std::endl;
        CrashHandler::ForceCrashDump();
    }
    catch (...) {
        std::cerr << "\n[EXCEPTION] Caught unknown exception in main loop" << std::endl;
        CrashHandler::ForceCrashDump();
    }
    
    // Shutdown
    g_deckLink.StopOutput(); // Explicit stop
    
    g_cefManager.Shutdown();

    CleanupDeviceD3D();
    ::DestroyWindow(hwnd);
    ::UnregisterClassW(wc.lpszClassName, wc.hInstance);
    
    std::cout << "\nExiting." << std::endl;

    return 0;
}

// Helper functions for D3D implementation...

// Select the best GPU adapter (prioritize NVIDIA or high VRAM)
IDXGIAdapter* SelectBestAdapter()
{
    IDXGIFactory* factory = nullptr;
    HRESULT hr = CreateDXGIFactory(__uuidof(IDXGIFactory), (void**)&factory);
    if (FAILED(hr)) {
        std::cerr << "[GPU] Failed to create DXGI Factory" << std::endl;
        return nullptr;
    }

    IDXGIAdapter* bestAdapter = nullptr;
    SIZE_T maxDedicatedMem = 0;
    UINT adapterIndex = 0;

    std::cout << "[GPU] Enumerating available adapters:" << std::endl;

    for (UINT i = 0; ; ++i) {
        IDXGIAdapter* adapter = nullptr;
        if (factory->EnumAdapters(i, &adapter) == DXGI_ERROR_NOT_FOUND)
            break;

        DXGI_ADAPTER_DESC desc;
        adapter->GetDesc(&desc);

        // Convert wide string to narrow for printing
        char descStr[128];
        wcstombs_s(nullptr, descStr, sizeof(descStr), desc.Description, _TRUNCATE);

        std::cout << "  [" << i << "] " << descStr
                  << " (Vendor: 0x" << std::hex << desc.VendorId << std::dec
                  << ", VRAM: " << (desc.DedicatedVideoMemory / 1024 / 1024) << " MB)" << std::endl;

        // Prioritize NVIDIA (0x10DE) or highest VRAM
        bool isNvidia = (desc.VendorId == 0x10DE);
        bool hasMoreVRAM = (desc.DedicatedVideoMemory > maxDedicatedMem);

        if (isNvidia || (!bestAdapter && hasMoreVRAM)) {
            if (bestAdapter) bestAdapter->Release();
            bestAdapter = adapter;
            maxDedicatedMem = desc.DedicatedVideoMemory;
            adapterIndex = i;

            if (isNvidia) {
                std::cout << "  -> NVIDIA GPU detected, selecting this adapter" << std::endl;
            }
        } else {
            adapter->Release();
        }
    }

    factory->Release();

    if (bestAdapter) {
        DXGI_ADAPTER_DESC desc;
        bestAdapter->GetDesc(&desc);
        char descStr[128];
        wcstombs_s(nullptr, descStr, sizeof(descStr), desc.Description, _TRUNCATE);
        std::cout << "[GPU] Selected adapter [" << adapterIndex << "]: " << descStr << std::endl;
    } else {
        std::cout << "[GPU] No suitable adapter found, using default" << std::endl;
    }

    return bestAdapter;
}

bool CreateDeviceD3D(HWND hWnd)
{
    // Setup swap chain
    DXGI_SWAP_CHAIN_DESC sd;
    ZeroMemory(&sd, sizeof(sd));
    sd.BufferCount = 2;
    sd.BufferDesc.Width = 0;
    sd.BufferDesc.Height = 0;
    sd.BufferDesc.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
    sd.BufferDesc.RefreshRate.Numerator = 60;
    sd.BufferDesc.RefreshRate.Denominator = 1;
    sd.Flags = DXGI_SWAP_CHAIN_FLAG_ALLOW_MODE_SWITCH;
    sd.BufferUsage = DXGI_USAGE_RENDER_TARGET_OUTPUT;
    sd.OutputWindow = hWnd;
    sd.SampleDesc.Count = 1;
    sd.SampleDesc.Quality = 0;
    sd.Windowed = TRUE;
    sd.SwapEffect = DXGI_SWAP_EFFECT_DISCARD;

    UINT createDeviceFlags = 0;
    //createDeviceFlags |= D3D11_CREATE_DEVICE_DEBUG;
    D3D_FEATURE_LEVEL featureLevel;
    const D3D_FEATURE_LEVEL featureLevelArray[2] = { D3D_FEATURE_LEVEL_11_0, D3D_FEATURE_LEVEL_10_0, };
    
    // Select best GPU adapter (NVIDIA or highest VRAM)
    IDXGIAdapter* adapter = SelectBestAdapter();
    D3D_DRIVER_TYPE driverType = adapter ? D3D_DRIVER_TYPE_UNKNOWN : D3D_DRIVER_TYPE_HARDWARE;
    
    HRESULT res = D3D11CreateDeviceAndSwapChain(adapter, driverType, nullptr, createDeviceFlags, featureLevelArray, 2, D3D11_SDK_VERSION, &sd, &g_pSwapChain, &g_pd3dDevice, &featureLevel, &g_pd3dDeviceContext);
    
    if (res == DXGI_ERROR_UNSUPPORTED) { // Try high-performance WARP software driver if hardware is not available.
        if (adapter) adapter->Release();
        adapter = nullptr;
        res = D3D11CreateDeviceAndSwapChain(nullptr, D3D_DRIVER_TYPE_WARP, nullptr, createDeviceFlags, featureLevelArray, 2, D3D11_SDK_VERSION, &sd, &g_pSwapChain, &g_pd3dDevice, &featureLevel, &g_pd3dDeviceContext);
    }
    
    // Release adapter after device creation
    if (adapter) adapter->Release();
    
    if (res != S_OK)
        return false;

    CreateRenderTarget();
    return true;
}

void CleanupDeviceD3D()
{
    CleanupRenderTarget();
    if (g_pSwapChain) { g_pSwapChain->Release(); g_pSwapChain = nullptr; }
    if (g_pd3dDeviceContext) { g_pd3dDeviceContext->Release(); g_pd3dDeviceContext = nullptr; }
    if (g_pd3dDevice) { g_pd3dDevice->Release(); g_pd3dDevice = nullptr; }
}

void CreateRenderTarget()
{
    ID3D11Texture2D* pBackBuffer;
    g_pSwapChain->GetBuffer(0, IID_PPV_ARGS(&pBackBuffer));
    g_pd3dDevice->CreateRenderTargetView(pBackBuffer, nullptr, &g_mainRenderTargetView);
    pBackBuffer->Release();
}

void CleanupRenderTarget()
{
    if (g_mainRenderTargetView) { g_mainRenderTargetView->Release(); g_mainRenderTargetView = nullptr; }
}

LRESULT WINAPI WndProc(HWND hWnd, UINT msg, WPARAM wParam, LPARAM lParam)
{
    switch (msg)
    {
    case WM_SIZE:
        if (g_pd3dDevice != nullptr && wParam != SIZE_MINIMIZED)
        {
            int width = (UINT)LOWORD(lParam);
            int height = (UINT)HIWORD(lParam);

            CleanupRenderTarget();
            g_pSwapChain->ResizeBuffers(0, width, height, DXGI_FORMAT_UNKNOWN, 0);
            CreateRenderTarget();

            // Note: We DO NOT resize CEF here. 
            // The broadcast output MUST remain 1920x1080.
            // The window resizing only affects the Preview scaling (handled in RenderCallback).
        }

        return 0;

    case WM_CLOSE:
        ::DestroyWindow(hWnd);
        return 0;

    case WM_DESTROY:
        ::PostQuitMessage(0);
        g_appDone = true;
        return 0;
    }
    return ::DefWindowProcW(hWnd, msg, wParam, lParam);
}

BOOL WINAPI ConsoleHandler(DWORD ctrlType) {
    if (ctrlType == CTRL_C_EVENT || ctrlType == CTRL_CLOSE_EVENT || ctrlType == CTRL_BREAK_EVENT) {
        g_appDone = true;
        return TRUE;
    }
    return FALSE;
}

// Helper to toggle borderless fullscreen
void ToggleFullscreen(HWND hWnd) {
    // static variables to save window state
    static WINDOWPLACEMENT g_wpPrev = { sizeof(g_wpPrev) };

    DWORD dwStyle = GetWindowLong(hWnd, GWL_STYLE);
    if (dwStyle & WS_OVERLAPPEDWINDOW) {
        MONITORINFO mi = { sizeof(mi) };
        if (GetWindowPlacement(hWnd, &g_wpPrev) &&
            GetMonitorInfo(MonitorFromWindow(hWnd, MONITOR_DEFAULTTOPRIMARY), &mi)) {
            SetWindowLong(hWnd, GWL_STYLE, dwStyle & ~WS_OVERLAPPEDWINDOW);
            SetWindowPos(hWnd, HWND_TOP,
                mi.rcMonitor.left, mi.rcMonitor.top,
                mi.rcMonitor.right - mi.rcMonitor.left,
                mi.rcMonitor.bottom - mi.rcMonitor.top,
                SWP_NOOWNERZORDER | SWP_FRAMECHANGED);
        }
    } else {
        SetWindowLong(hWnd, GWL_STYLE, dwStyle | WS_OVERLAPPEDWINDOW);
        SetWindowPlacement(hWnd, &g_wpPrev);
        SetWindowPos(hWnd, NULL, 0, 0, 0, 0,
            SWP_NOMOVE | SWP_NOSIZE | SWP_NOZORDER | SWP_NOOWNERZORDER | SWP_FRAMECHANGED);
    }
}
