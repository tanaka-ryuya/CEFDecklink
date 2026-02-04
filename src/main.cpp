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

// Forward declarations of helper functions
IDXGIAdapter* SelectBestAdapter();
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

// RenderFrame now only handles Main Thread tasks: Input processing & CEF Message Loop & Logging
void RenderFrame() {
    // --- Keyboard Input for Alpha Threshold Adjustment ---
    static float alphaThreshold = 0.01f; // Start value
    static auto lastKeyTime = std::chrono::steady_clock::now();
    auto now = std::chrono::steady_clock::now();
    auto timeSinceLastKey = std::chrono::duration_cast<std::chrono::milliseconds>(now - lastKeyTime).count();
    
    // Allow adjustment every 100ms when key is held
    if (timeSinceLastKey > 100) {
        bool changed = false;
        if (GetAsyncKeyState(VK_OEM_PLUS) & 0x8000 || GetAsyncKeyState(0xBB) & 0x8000) { // + key
            alphaThreshold += 0.001f;
            if (alphaThreshold > 1.0f) alphaThreshold = 1.0f;
            changed = true;
            lastKeyTime = now;
        }
        if (GetAsyncKeyState(VK_OEM_MINUS) & 0x8000 || GetAsyncKeyState(0xBD) & 0x8000) { // - key
            alphaThreshold -= 0.001f;
            if (alphaThreshold < 0.0f) alphaThreshold = 0.0f;
            changed = true;
            lastKeyTime = now;
        }
        
        if (changed && g_shaderManager) {
            // NOTE: ShaderManager Access from Main Thread while DeckLink accesses it from Callback Thread!
            // ShaderManager should use D3D context locking if not done already.
            g_shaderManager->SetAlphaThreshold(alphaThreshold);
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
        LogStatus(true, fps, alphaThreshold);
        
        frameCount = 0;
        lastLogTime = now;
    }
}

// Main code
int main(int, char**)
{
    // 1. CEF Sub-process check (MUST be the absolute first thing)
    CefMainArgs main_args(GetModuleHandle(nullptr));
    int exit_code = CefExecuteProcess(main_args, nullptr, nullptr);
    if (exit_code >= 0) {
        return exit_code;
    }

    std::cout << "--- DeckLink + CEF CUI Application ---" << std::endl;
    std::cout << "Initializing..." << std::endl;

    // Initialize Crash Handler for debugging
    CrashHandler::Initialize();

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

        // --- Register Render Callback (Reference Pattern) ---
        // This runs INSIDE the DeckLink thread/callback
        g_deckLink.SetRenderCallback([](void* pBuffer) {
            // New Logic for 60p -> 60i Conversion (Double Fetch)
            // We need to fetch TWO source frames for every ONE output frame.
            // Frame T   -> Top Field
            // Frame T+1 -> Bottom Field

            ID3D11ShaderResourceView* srvTop = nullptr;
            ID3D11ShaderResourceView* srvBottom = nullptr;

            auto renderHandler = g_cefManager.GetRenderHandler();
            
            if (renderHandler) {
                // 1. Fetch Top Field Source (Frame T)
                // This advances the read pointer by 1
                renderHandler->SyncWithGPU(); 
                srvTop = renderHandler->GetTextureSRV();
                
                // 2. Fetch Bottom Field Source (Frame T+1)
                // This advances the read pointer by 1 again
                renderHandler->SyncWithGPU();
                srvBottom = renderHandler->GetTextureSRV();
            }

            // Fallback: If we only got one frame (e.g. startup or starvation), duplicate it.
            // This effectively creates a progressive frame (Field 0 == Field 1)
            // Note: GetTextureSRV already AddRef'd srvTop, so if we copy pointer we must AddRef again.
            if (srvTop && !srvBottom) {
                srvBottom = srvTop;
                srvBottom->AddRef();
            }
            // Sanity check (inverse case unlikely via ring buffer but safe to handle)
            if (!srvTop && srvBottom) {
                srvTop = srvBottom;
                srvTop->AddRef();
            }

            // 3. Dispatch Shader (Interlace Combine)
            if (pBuffer) {
                if (srvTop && srvBottom && g_shaderManager) {
                    g_shaderManager->ConvertAndDownload(srvTop, srvBottom, pBuffer); 
                } else if (g_shaderManager) {
                     // No source frames available - Output Black
                     // Assuming ARGB 4 bytes per pixel for now based on previous context
                     memset(pBuffer, 0, 1920 * 1080 * 4);
                }
            }
            
            // Release References
            if (srvTop) srvTop->Release();
            if (srvBottom) srvBottom->Release();
        });

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
            RenderFrame(); // This now just handles input and logging
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

    // Shutdown CEF cleanly before D3D cleanup
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

    // --- Enable Multithread Protection ---
    // Critical for Callback-Driven Architecture where DeckLink thread uses the Context
    /*
    ID3D11Multithread* pMultithread = nullptr;
    if (SUCCEEDED(g_pd3dDevice->QueryInterface(IID_PPV_ARGS(&pMultithread)))) {
        pMultithread->SetMultithreadProtected(TRUE);
        pMultithread->Release();
        std::cout << "[D3D11] Multithread protection enabled." << std::endl;
    } else {
        std::cerr << "[D3D11] Failed to enable multithread protection!" << std::endl;
    }
    */
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
