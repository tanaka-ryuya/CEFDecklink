#include <Windows.h>
#include <shellapi.h>
#include <tchar.h>
#include <d3d11.h>
#include <vector>
#include <iostream>
#include <iomanip>
#include <chrono>

#include "DeckLinkDevice.h"

#include "CefManager.h"
#include "CefRenderHandler.h"
#include "ShaderManager.h"

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

// Forward declarations of helper functions
bool CreateDeviceD3D(HWND hWnd);
void CleanupDeviceD3D();
void CreateRenderTarget();
void CleanupRenderTarget();
LRESULT WINAPI WndProc(HWND hWnd, UINT msg, WPARAM wParam, LPARAM lParam);

// Console Output Helper
void LogStatus(bool locked, double fps, float alphaThreshold) {
    static auto lastTime = std::chrono::steady_clock::now();
    auto now = std::chrono::steady_clock::now();
    
    // Log every 1 second
    if (std::chrono::duration_cast<std::chrono::milliseconds>(now - lastTime).count() > 1000) {
        std::cout << "\r" // Carriage return to overwrite line
                  << "[Status] Sync: " << (locked ? "LOCKED (59.94i)" : "SEARCHING...")
                  << " | Rate: " << std::fixed << std::setprecision(2) << fps << " fps (" << fps * 2.0 << " fields)"
                  << " | Alpha Threshold: " << std::setprecision(4) << alphaThreshold
                  << " | +/- to adjust | Ctrl+C to Exit."
                  << std::flush;
        lastTime = now;
    }
}

// Render a single frame (Logic extracted from main loop)
void RenderFrame() {
    // --- Keyboard Input for Alpha Threshold Adjustment ---
    static float alphaThreshold = 0.01f; // Start value
    static auto lastKeyTime = std::chrono::steady_clock::now();
    auto now = std::chrono::steady_clock::now();
    auto timeSinceLastKey = std::chrono::duration_cast<std::chrono::milliseconds>(now - lastKeyTime).count();
    
    // Allow adjustment every 100ms when key is held
    if (timeSinceLastKey > 100) {
        if (GetAsyncKeyState(VK_OEM_PLUS) & 0x8000 || GetAsyncKeyState(0xBB) & 0x8000) { // + key
            alphaThreshold += 0.001f;
            if (alphaThreshold > 1.0f) alphaThreshold = 1.0f;
            if (g_shaderManager) g_shaderManager->SetAlphaThreshold(alphaThreshold);
            lastKeyTime = now;
        }
        if (GetAsyncKeyState(VK_OEM_MINUS) & 0x8000 || GetAsyncKeyState(0xBD) & 0x8000) { // - key
            alphaThreshold -= 0.001f;
            if (alphaThreshold < 0.0f) alphaThreshold = 0.0f;
            if (g_shaderManager) g_shaderManager->SetAlphaThreshold(alphaThreshold);
            lastKeyTime = now;
        }
    }

    // CEF Message Loop
    g_cefManager.DoMessageLoopWork();

    // --- Synchronization ---
    bool deckLinkReady = g_deckLink.WaitForNextFrame(35); 
    
    // --- Process Pipeline ---
    // Persistent History Frame
    static ID3D11ShaderResourceView* prevSRV = nullptr;
    static ID3D11ShaderResourceView* currentSRV = nullptr;

    if (deckLinkReady) {
        // 1. Get CEF Texture (Latest)
        auto renderHandler = g_cefManager.GetRenderHandler();

        if (renderHandler) {
            renderHandler->SyncWithGPU();
            
            // Shift History
             if (prevSRV) prevSRV->Release();
             prevSRV = currentSRV; 
             
             // Get NEW Current
             currentSRV = renderHandler->GetTextureSRV();
        }

        // Handle startup
        if (!prevSRV && currentSRV) {
            currentSRV->AddRef();
            prevSRV = currentSRV;
        }
        if (!currentSRV && prevSRV) {
             prevSRV->AddRef();
             currentSRV = prevSRV;
        }

        // 2. Get DeckLink Buffer
        void* pBuffer = nullptr;
        void* pKeyBuffer = nullptr;
        
        if (g_deckLink.GetFrameBuffer(&pBuffer, &pKeyBuffer)) {
            // 3. Convert BGRA -> YUV (GPU) with Frame Mixing
            if (prevSRV && currentSRV && g_shaderManager) {
                g_shaderManager->ConvertAndDownload(currentSRV, prevSRV, pBuffer, pKeyBuffer); 
            } else if (currentSRV && g_shaderManager) {
                 g_shaderManager->ConvertAndDownload(currentSRV, currentSRV, pBuffer, pKeyBuffer);
            }
            
            // 4. Schedule Frame
            g_deckLink.ScheduleNextFrame();
        }
    }

    // Console Logging (Actual FPS calculation)
    static int frameCount = 0;
    static auto lastLogTime = std::chrono::steady_clock::now();
    
    if (deckLinkReady) {
        frameCount++;
    }

    now = std::chrono::steady_clock::now();
    if (std::chrono::duration_cast<std::chrono::milliseconds>(now - lastLogTime).count() > 1000) {
        double fps = (double)frameCount; 
        LogStatus(deckLinkReady, fps, alphaThreshold);
        
        frameCount = 0;
        lastLogTime = now;
    }
}

// Main code
int main(int, char**)
{
    // Ensure Console Handling
    // If built as Console Subsystem, stdout works. 
    // If user runs from existing terminal, it attaches.
    
    std::cout << "--- DeckLink + CEF CUI Application ---" << std::endl;
    std::cout << "Initializing..." << std::endl;

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

    // Don't show window!
    // ::ShowWindow(hwnd, SW_SHOWDEFAULT); 
    ::ShowWindow(hwnd, SW_HIDE);
    ::UpdateWindow(hwnd);

    // Initialize DeckLink
    if (g_deckLink.Initialize())
    {
        std::cout << "DeckLink Initialized." << std::endl;
        g_deckLink.StartOutput();
    }
    else
    {
        std::cerr << "Failed to initialize DeckLink!" << std::endl; 
    }

    // Create CEF Browser
    // Note: URL from user request
    g_cefManager.CreateBrowser(hwnd, "http://localhost:9090/graphics/on_air.html", g_pd3dDevice);

    std::cout << "Starting Main Loop..." << std::endl;

    // Main loop
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

        RenderFrame();
    }
    
    // Shutdown
    g_deckLink.StopOutput(); // Explicit stop
    
    g_cefManager.Shutdown();

    // Shutdown CEF cleanly before D3D cleanup
    g_cefManager.Shutdown();

    CleanupDeviceD3D();
    ::DestroyWindow(hwnd);
    ::UnregisterClassW(wc.lpszClassName, wc.hInstance);
    
    std::cout << "\nExiting." << std::endl;

    return 0;
}

// Helper functions for D3D implementation...
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
    HRESULT res = D3D11CreateDeviceAndSwapChain(nullptr, D3D_DRIVER_TYPE_HARDWARE, nullptr, createDeviceFlags, featureLevelArray, 2, D3D11_SDK_VERSION, &sd, &g_pSwapChain, &g_pd3dDevice, &featureLevel, &g_pd3dDeviceContext);
    if (res == DXGI_ERROR_UNSUPPORTED) // Try high-performance WARP software driver if hardware is not available.
        res = D3D11CreateDeviceAndSwapChain(nullptr, D3D_DRIVER_TYPE_WARP, nullptr, createDeviceFlags, featureLevelArray, 2, D3D11_SDK_VERSION, &sd, &g_pSwapChain, &g_pd3dDevice, &featureLevel, &g_pd3dDeviceContext);
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
            CleanupRenderTarget();
            g_pSwapChain->ResizeBuffers(0, (UINT)LOWORD(lParam), (UINT)HIWORD(lParam), DXGI_FORMAT_UNKNOWN, 0);
            CreateRenderTarget();
        }
        return 0;
    case WM_DESTROY:
        ::PostQuitMessage(0);
        return 0;
    }
    return ::DefWindowProcW(hWnd, msg, wParam, lParam);
}
