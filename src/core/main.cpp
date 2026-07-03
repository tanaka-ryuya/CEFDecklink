#include <Windows.h>
#include <shellapi.h>
#include <tchar.h>
#include <d3d11.h>
#include <d3d11_1.h>
#include <d3d10.h>
#include <dxgi.h>
#include <iostream>
#include <iomanip>
#include <chrono>
#include <thread>
#include <mutex>
#include <mmsystem.h>
#pragma comment(lib, "winmm.lib")

#include <wincrypt.h>
#pragma comment(lib, "crypt32.lib")

#include "DeckLinkDevice.h"

#include "CefManager.h"
#include "CefRenderHandler.h"
#include "ShaderManager.h"
#include "CrashHandler.h"
#include "../resource.h"

#include <fstream>
#include <sstream>
#include <ctime>
#include <atomic>
#include <conio.h>

// ============================================================
// File Logger - Writes to logs/app_YYYYMMDD_HHMMSS.log
// ============================================================
#include <vector>
#include <deque>

// ============================================================
// File Logger - Writes to logs/app_YYYYMMDD_HHMMSS.log
// and buffers recent logs for TUI
// ============================================================
class FileLogger {
public:
    FileLogger() {}

    bool Open(const std::string& logDir) {
        CreateDirectoryA(logDir.c_str(), nullptr);

        auto now = std::chrono::system_clock::now();
        std::time_t t = std::chrono::system_clock::to_time_t(now);
        struct tm tm_info;
        localtime_s(&tm_info, &t);
        char buf[64];
        strftime(buf, sizeof(buf), "app_%Y%m%d_%H%M%S.log", &tm_info);

        m_path = logDir + "\\" + buf;
        m_file.open(m_path, std::ios::out | std::ios::trunc);
        return m_file.is_open();
    }

    void Log(const std::string& tag, const std::string& msg) {
        auto now = std::chrono::system_clock::now();
        std::time_t t = std::chrono::system_clock::to_time_t(now);
        struct tm tm_info;
        localtime_s(&tm_info, &t);
        char timebuf[32];
        strftime(timebuf, sizeof(timebuf), "%H:%M:%S", &tm_info);

        std::ostringstream oss;
        oss << "[" << timebuf << "] " << tag << " " << msg;
        std::string line = oss.str();

        std::lock_guard<std::mutex> lock(m_mutex);
        // Full timestamp for file
        if (m_file.is_open()) {
            char fulltime[32];
            strftime(fulltime, sizeof(fulltime), "%Y-%m-%d %H:%M:%S", &tm_info);
            m_file << fulltime << " " << tag << " " << msg << "\n";
            m_file.flush();
        }

        // Buffer recent logs for TUI display
        m_recentLogs.push_back(line);
        if (m_recentLogs.size() > 5) {
            m_recentLogs.pop_front();
        }
    }

    std::vector<std::string> GetRecentLogs() {
        std::lock_guard<std::mutex> lock(m_mutex);
        return std::vector<std::string>(m_recentLogs.begin(), m_recentLogs.end());
    }

    const std::string& GetPath() const { return m_path; }

private:
    std::ofstream m_file;
    std::mutex m_mutex;
    std::string m_path;
    std::deque<std::string> m_recentLogs;
};

static FileLogger g_logger;

// Enable Virtual Terminal Processing for ANSI Escape Codes
static bool EnableVTMode() {
    HANDLE hOut = GetStdHandle(STD_OUTPUT_HANDLE);
    if (hOut == INVALID_HANDLE_VALUE) return false;
    DWORD dwMode = 0;
    if (!GetConsoleMode(hOut, &dwMode)) return false;
    dwMode |= ENABLE_VIRTUAL_TERMINAL_PROCESSING;
    return SetConsoleMode(hOut, dwMode) != 0;
}

// Data
static ID3D11Device*            g_pd3dDevice = nullptr;
static ID3D11DeviceContext*     g_pd3dDeviceContext = nullptr;
static std::mutex               g_d3dContextMutex;
static IDXGISwapChain*          g_pSwapChain = nullptr;
static ID3D11RenderTargetView*  g_mainRenderTargetView = nullptr;
static bool                     g_appDone = false;

// Managers & Devices (Global)
static CefManager g_cefManager;
static std::unique_ptr<ShaderManager> g_shaderManager;
static DeckLinkDevice g_deckLink;
std::atomic<int> g_viewMode(0); // 0=Interlace, 1=Diff, 2=Progressive, 3=30pBlend
static std::string g_targetUrl;
static std::string g_format = "5994i"; // "5994i" or "50i"
static std::string g_licenseKey = "";
static bool g_isLicensed = false;
std::atomic<int> g_filterMode(0); // 0=None, 1=3-tap, 2=5-tap vertical LPF

// Forward declarations of helper functions
IDXGIAdapter* SelectBestAdapter();
bool CreateDeviceD3D(HWND hWnd);
void CleanupDeviceD3D();
void CreateRenderTarget();
void CleanupRenderTarget();
bool IsLicenseValid(const std::string& key) {
    if (key.length() < 10 || key[8] != '-') return false;
    std::string dateStr = key.substr(0, 8);
    std::string sigStr = key.substr(9);

    // Hardcoded RSA-2048 Public Key BLOB
    BYTE pubBlob[] = {
        0x06, 0x02, 0x00, 0x00, 0x00, 0xA4, 0x00, 0x00, 0x52, 0x53, 0x41, 0x31, 0x00, 0x08, 0x00, 0x00,
        0x01, 0x00, 0x01, 0x00, 0x95, 0x36, 0x2D, 0x18, 0x47, 0x82, 0x3D, 0xF5, 0x5C, 0x28, 0x08, 0xD1,
        0x9D, 0x2B, 0xAD, 0x8E, 0x41, 0xEC, 0xC7, 0x46, 0x07, 0x43, 0x3E, 0x90, 0x4F, 0x48, 0x98, 0x8A,
        0x0A, 0x33, 0x43, 0x94, 0xA9, 0xF0, 0x24, 0x24, 0xBC, 0x18, 0xDD, 0xA3, 0xE2, 0x39, 0x48, 0x83,
        0x4F, 0xBC, 0x84, 0x0C, 0x32, 0x0C, 0xDA, 0x9E, 0x3B, 0xD4, 0x12, 0xA0, 0x21, 0x32, 0x19, 0xD0,
        0x30, 0xD8, 0xD4, 0xE8, 0x36, 0x15, 0xDF, 0xEA, 0x16, 0xA8, 0x3C, 0x1E, 0x95, 0x85, 0x84, 0x8C,
        0xE3, 0xA5, 0x96, 0xD2, 0x1F, 0x5D, 0xB0, 0xC8, 0x22, 0x66, 0xB2, 0xA6, 0x4C, 0x34, 0x93, 0x9B,
        0x3F, 0x3C, 0x3B, 0xED, 0xF0, 0x2E, 0x19, 0xDB, 0x62, 0xC5, 0xCE, 0x94, 0x39, 0x39, 0x0F, 0x85,
        0x9D, 0x81, 0x83, 0x33, 0x12, 0xBF, 0x56, 0x1B, 0x42, 0x9B, 0x76, 0xB0, 0xE3, 0x01, 0xD6, 0x20,
        0xED, 0xEF, 0x0B, 0x59, 0x1B, 0x07, 0x21, 0x98, 0x1F, 0xE3, 0x70, 0x77, 0x54, 0x2D, 0x1E, 0x97,
        0x9D, 0xA7, 0xA1, 0xCC, 0x56, 0x59, 0x79, 0x9B, 0x02, 0x3D, 0xE7, 0x5A, 0x28, 0xB2, 0xD7, 0x6E,
        0x22, 0x3B, 0x27, 0xF9, 0x38, 0x0C, 0xEC, 0x54, 0xE5, 0x94, 0xD0, 0x29, 0x17, 0xB3, 0xA3, 0x96,
        0x49, 0x47, 0x94, 0xA7, 0x50, 0xFE, 0x8A, 0x69, 0x6D, 0x81, 0xAF, 0x23, 0x0A, 0xFD, 0xD3, 0xCA,
        0x36, 0xE2, 0xE9, 0x1B, 0xD1, 0xD0, 0x96, 0x5B, 0x14, 0xF3, 0xE0, 0x54, 0xF5, 0x8B, 0xA5, 0xFB,
        0x46, 0xCB, 0x7F, 0x8E, 0x7E, 0xB3, 0x41, 0x92, 0x47, 0xB2, 0xCA, 0xE1, 0x90, 0xCC, 0x7C, 0xFF,
        0x2B, 0xED, 0x04, 0xF1, 0xC6, 0x08, 0x95, 0x64, 0x38, 0xB9, 0x0F, 0x3B, 0x9A, 0x26, 0xF3, 0x58,
        0x97, 0x84, 0x1B, 0xC9, 0xAA, 0xD5, 0x98, 0x83, 0x0C, 0xAB, 0xCE, 0xEA, 0xD5, 0x94, 0x4E, 0x00,
        0xAE, 0x36, 0x8A, 0xF7
    };

    DWORD sigLen = 0;
    if (!CryptStringToBinaryA(sigStr.c_str(), 0, CRYPT_STRING_BASE64, NULL, &sigLen, NULL, NULL)) return false;
    std::vector<BYTE> sigData(sigLen);
    if (!CryptStringToBinaryA(sigStr.c_str(), 0, CRYPT_STRING_BASE64, sigData.data(), &sigLen, NULL, NULL)) return false;

    // Windows CryptoAPI expects Little Endian for signatures, but Base64 encodes Big Endian from .NET.
    // Reverse the signature bytes
    for (size_t i = 0; i < sigLen / 2; ++i) {
        std::swap(sigData[i], sigData[sigLen - 1 - i]);
    }

    HCRYPTPROV hProv = 0;
    if (!CryptAcquireContext(&hProv, NULL, MS_ENH_RSA_AES_PROV, PROV_RSA_AES, CRYPT_VERIFYCONTEXT)) return false;

    HCRYPTKEY hKey = 0;
    if (!CryptImportKey(hProv, pubBlob, sizeof(pubBlob), 0, 0, &hKey)) {
        CryptReleaseContext(hProv, 0);
        return false;
    }

    HCRYPTHASH hHash = 0;
    if (!CryptCreateHash(hProv, CALG_SHA_256, 0, 0, &hHash)) {
        CryptDestroyKey(hKey);
        CryptReleaseContext(hProv, 0);
        return false;
    }

    if (!CryptHashData(hHash, (const BYTE*)dateStr.c_str(), (DWORD)dateStr.length(), 0)) {
        CryptDestroyHash(hHash);
        CryptDestroyKey(hKey);
        CryptReleaseContext(hProv, 0);
        return false;
    }

    bool isValidSig = CryptVerifySignatureA(hHash, sigData.data(), sigLen, hKey, NULL, 0) == TRUE;
    
    CryptDestroyHash(hHash);
    CryptDestroyKey(hKey);
    CryptReleaseContext(hProv, 0);

    if (!isValidSig) return false;

    time_t now = time(nullptr);
    struct tm tm_now;
    localtime_s(&tm_now, &now);
    char buf[9];
    strftime(buf, sizeof(buf), "%Y%m%d", &tm_now);
    std::string current_dateStr(buf);

    if (current_dateStr > dateStr) return false; 
    return true; 
}
LRESULT WINAPI WndProc(HWND hWnd, UINT msg, WPARAM wParam, LPARAM lParam);
void ToggleFullscreen(HWND hWnd);
BOOL WINAPI ConsoleHandler(DWORD ctrlType);

// TUI Dashboard Console Output Helper
void LogStatus(bool locked, double deckLinkFps, int cefFps, int uniqueInInterval, float alphaThreshold, uint64_t totalCefFrames, int pendingCount) {
    static bool tuiInitialized = false;
    if (!tuiInitialized) {
        EnableVTMode();
        // Clear screen once on start
        std::cout << "\x1b[2J" << std::flush;
        tuiInitialized = true;
    }

    const char* modeStr = "Interlace (0)";
    int vMode = g_viewMode.load();
    if (vMode == 1) modeStr = "\x1b[33mDiff Mode (1)\x1b[0m"; // Yellow warning for Diff
    else if (vMode == 2) modeStr = "Progressive (2)";
    else if (vMode == 3) modeStr = "\x1b[35m30p Blend (3)\x1b[0m"; // Magenta for Blend

    auto recentLogs = g_logger.GetRecentLogs();

    std::ostringstream oss;
    // Move cursor to home (0,0)
    oss << "\x1b[H";
    oss << "\x1b[36m===============================================================================\x1b[K\x1b[0m\n";
    oss << "  \x1b[1m\x1b[37m\xF0\x9F\x8D\x8C CEFDecklink Live Status Dashboard \xF0\x9F\x8D\x8C\x1b[K\x1b[0m\n";
    oss << "  \x1b[36mURL: \x1b[0m" << g_targetUrl << "\x1b[K\n";
    oss << "\x1b[36m===============================================================================\x1b[K\x1b[0m\n";
    oss << "  \x1b[32m[Status]\x1b[0m   DeckLink: \x1b[1m" << std::fixed << std::setprecision(2) << deckLinkFps << " fps\x1b[0m | "
        << "CEF: \x1b[1m" << cefFps << " fps\x1b[0m | "
        << "Queue: \x1b[1m" << pendingCount << "\x1b[0m\x1b[K\n";
    oss << "  \x1b[32m[Config]\x1b[0m   ViewMode: " << modeStr << " | Format: " << g_format << " | UnmultThresh: " << std::fixed << std::setprecision(4) << alphaThreshold;
    // Filter Mode display
    int fMode = g_filterMode.load();
    const char* filterStr = "None";
    if (fMode == 1) filterStr = "3tap";
    else if (fMode == 2) filterStr = "5tap";
    oss << " | Filter: " << filterStr << "\x1b[K\n";
    oss << "\x1b[36m-------------------------------------------------------------------------------\x1b[K\x1b[0m\n";
    oss << "  \x1b[33mRecent Events / Logs:\x1b[K\x1b[0m\n";
    
    for (int i = 0; i < 5; ++i) {
        if (i < (int)recentLogs.size()) {
            oss << "   " << recentLogs[i] << "\x1b[K\n";
        } else {
            oss << "\x1b[K\n";
        }
    }

    oss << "\x1b[36m===============================================================================\x1b[K\x1b[0m\n";
    oss << "  \x1b[90mControls: Ctrl+I(Interlace) | Ctrl+D(Diff) | Ctrl+P(Prog) | Ctrl+F(Filter)\x1b[K\x1b[0m\n";
    oss << "            \x1b[90mCtrl+A/Z(UnmultThresh) | Ctrl+C(Exit)\x1b[K\x1b[0m\n";
    oss << "\x1b[36m===============================================================================\x1b[K\x1b[0m\x1b[J";

    std::cout << oss.str() << std::flush;
}

// ============================================================
// Includes
// ============================================================

#include <iostream>
#include <fstream>
#include <string>

// ... (existing includes)

// Global Configuration
static float g_alphaThreshold = 0.000f;

// Helper to load config.json
bool LoadConfig(std::string& url, float& alpha, std::string& format) {
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

    // Load Unmult Threshold (formerly alpha)
    float parsedUnmultThresh = ParseFloat("unmult_thresh");
    if (parsedUnmultThresh >= 0.0f) {
        alpha = parsedUnmultThresh;
    }

    // Load Filter Mode
    float parsedFilterMode = ParseFloat("il_filter_mode");
    if (parsedFilterMode >= 0.0f) {
        g_filterMode.store((int)parsedFilterMode);
    }
    
    // Load Format
    std::string parsedFormat = ParseString("format");
    if (!parsedFormat.empty()) {
        format = parsedFormat;
    }
    
    // Load License from separate file
    std::wstring licensePath = exeDir + L"\\licensekey.json";
    std::ifstream licFile(licensePath);
    if (licFile.is_open()) {
        std::string licContent((std::istreambuf_iterator<char>(licFile)), std::istreambuf_iterator<char>());
        size_t keyPos = licContent.find("\"license_key\"");
        if (keyPos != std::string::npos) {
            size_t colonPos = licContent.find(":", keyPos);
            if (colonPos != std::string::npos) {
                size_t startQuote = licContent.find("\"", colonPos);
                if (startQuote != std::string::npos) {
                    size_t endQuote = licContent.find("\"", startQuote + 1);
                    if (endQuote != std::string::npos) {
                        g_licenseKey = licContent.substr(startQuote + 1, endQuote - startQuote - 1);
                    }
                }
            }
        }
    }
    
    return true;
}

// ... (rest of main)

// RenderFrame now only handles Main Thread tasks: Input processing & CEF Message Loop & Logging
void RenderFrame(HWND hWnd) {
    // --- Console TUI Keyboard Input (_kbhit / _getch) ---
    bool changed = false;
    while (_kbhit()) {
        int ch = _getch();
        bool ctrlPressed = (GetAsyncKeyState(VK_CONTROL) & 0x8000) != 0;
        bool shiftPressed = (GetAsyncKeyState(VK_SHIFT) & 0x8000) != 0;
        bool altPressed = (GetAsyncKeyState(VK_MENU) & 0x8000) != 0;
        bool modified = ctrlPressed || shiftPressed || altPressed;

        // Requires modifier key (Ctrl/Shift/Alt or ASCII control codes) to prevent accidental triggers
        if (ch == 4 || (modified && (ch == 'd' || ch == 'D'))) {
            g_viewMode.store(1); // Diff Mode
            changed = true;
        } else if (ch == 16 || (modified && (ch == 'p' || ch == 'P'))) {
            g_viewMode.store(2); // Progressive Mode
            changed = true;
        } else if ((ch == 9 && ctrlPressed) || (modified && (ch == 'i' || ch == 'I'))) {
            g_viewMode.store(0); // Interlace Mode
            changed = true;
        } else if (ch == 2 || (modified && (ch == 'b' || ch == 'B'))) {
            g_viewMode.store(3); // 30p Blend Mode
            changed = true;
        } else if (ch == 1 || (modified && (ch == 'a' || ch == 'A'))) {
            g_alphaThreshold += 0.001f;
            if (g_alphaThreshold > 1.0f) g_alphaThreshold = 1.0f;
            changed = true;
        } else if (ch == 26 || (modified && (ch == 'z' || ch == 'Z'))) {
            g_alphaThreshold -= 0.001f;
            if (g_alphaThreshold < 0.0f) g_alphaThreshold = 0.0f;
            changed = true;
        } else if (ch == 6 || (modified && (ch == 'f' || ch == 'F'))) {
            // Cycle vertical LPF: None(0) -> 3-tap(1) -> 5-tap(2) -> None(0)
            int fm = g_filterMode.load();
            g_filterMode.store((fm + 1) % 3);
            changed = true;
        }
    }

    // Window Fullscreen hotkey (F11)
    if (GetForegroundWindow() == hWnd && (GetAsyncKeyState(VK_F11) & 0x8000)) {
        static auto lastF11Time = std::chrono::steady_clock::now();
        auto nowF11 = std::chrono::steady_clock::now();
        if (std::chrono::duration_cast<std::chrono::milliseconds>(nowF11 - lastF11Time).count() > 300) {
            ToggleFullscreen(hWnd);
            lastF11Time = nowF11;
        }
    }
    
    if (changed && g_shaderManager) {
        std::lock_guard<std::mutex> lock(g_d3dContextMutex);
        g_shaderManager->SetAlphaThreshold(g_alphaThreshold);
        g_shaderManager->SetFilterMode(g_filterMode.load());
    }

    // CEF Message Loop
    g_cefManager.DoMessageLoopWork();

    // --- Synchronization - Wait for DeckLink callback just for tracking FPS ---
    bool deckLinkReady = g_deckLink.WaitForNextFrame(0);
    
    // Console Logging (Actual FPS calculation)
    static int frameCount = 0;
    static auto lastLogTime = std::chrono::steady_clock::now();
    static auto lastHeartbeatTime = std::chrono::steady_clock::now();
    static uint64_t lastCefTotal = 0;
    static int cefZeroCount = 0; // consecutive 1-sec intervals with 0 CEF fps
    
    if (deckLinkReady) {
        frameCount++;
    }

    auto now = std::chrono::steady_clock::now();
    uint64_t elapsedMs = std::chrono::duration_cast<std::chrono::milliseconds>(now - lastLogTime).count();
    if (elapsedMs >= 1000) {
        static uint64_t lastUniqueTotal = 0;
        double deckLinkFps = (double)frameCount * 1000.0 / elapsedMs;
        
        // Get CEF Rate
        int cefTotalInInterval = 0;
        auto handler = g_cefManager.GetRenderHandler();
        uint64_t totalCef = 0;
        int pendingCef = 0;
        if (handler) {
            cefTotalInInterval = handler->GetAndResetFrameCount();
            totalCef = handler->GetTotalFrameCount();
            pendingCef = handler->GetPendingFrameCount();
        }

        uint64_t diffUnique = (totalCef >= lastUniqueTotal) ? (totalCef - lastUniqueTotal) : 0;
        lastUniqueTotal = totalCef;

        double cefFps = (double)cefTotalInInterval * 1000.0 / elapsedMs;
        double normalizedUnique = (double)diffUnique * 1000.0 / elapsedMs;

        LogStatus(true, deckLinkFps, (int)(cefFps + 0.5), (int)(normalizedUnique + 0.5), g_alphaThreshold, totalCef, pendingCef);

        lastCefTotal = totalCef;

        // ---- Log status to file every 10 seconds ----
        static int statusLogCounter = 0;
        if (++statusLogCounter >= 10) {
            statusLogCounter = 0;
            int drops = 0;
            int dups = 0;
            if (handler) {
                drops = handler->GetAndResetDroppedFrames();
                dups = handler->GetAndResetDuplicatedFrames();
            }

            std::ostringstream oss;
            oss << "DL=" << std::fixed << std::setprecision(1) << deckLinkFps 
                << " CEF=" << (int)(cefFps + 0.5) 
                << " Q=" << pendingCef
                << " skips(d:" << drops << " u:" << dups << ")";
            g_logger.Log("[STATUS]", oss.str());
        }
        
        frameCount = 0;
        lastLogTime = now;
    }

    // ---- Heartbeat every 60 seconds ----
    uint64_t hbElapsedSec = std::chrono::duration_cast<std::chrono::seconds>(now - lastHeartbeatTime).count();
    if (hbElapsedSec >= 60) {
        lastHeartbeatTime = now;
        auto handler = g_cefManager.GetRenderHandler();
        uint64_t totalCef = handler ? handler->GetTotalFrameCount() : 0;
        std::ostringstream oss;
        oss << "alive cefTotal=" << totalCef;
        g_logger.Log("[HEARTBEAT]", oss.str());

        // Periodic License Check
        bool newLicenseStatus = IsLicenseValid(g_licenseKey);
        if (newLicenseStatus != g_isLicensed) {
            g_isLicensed = newLicenseStatus;
            if (g_shaderManager) {
                std::lock_guard<std::mutex> lock(g_d3dContextMutex);
                g_shaderManager->SetLicensed(g_isLicensed);
            }
            g_cefManager.SetLicensed(g_isLicensed);
        }
    }
}

// Main code
int main(int argc, char** argv)
{
    // 0. Configuration Logic
    g_targetUrl = "https://telophub.duckdns.org/graphics/preview.html?machineId=8efb67b2-2fac-418a-9bd9-0284852ccd86";
    
    // Priority 3: Default (Set above)
    
    // Priority 2: config.json
    LoadConfig(g_targetUrl, g_alphaThreshold, g_format);
    g_isLicensed = IsLicenseValid(g_licenseKey);
    g_cefManager.SetLicensed(g_isLicensed);
    
    // Priority 1: CLI Args
    for (int i = 1; i < argc; ++i) {
        std::string arg = argv[i];
        if (arg == "--url" && i + 1 < argc) {
            g_targetUrl = argv[++i];
        } else if (arg == "--unmult_thresh" && i + 1 < argc) {
            try {
                g_alphaThreshold = std::stof(argv[++i]);
            } catch (...) {}
        } else if (arg == "--il_filter_mode" && i + 1 < argc) {
            try {
                g_filterMode.store(std::stoi(argv[++i]));
            } catch (...) {}
        } else if (arg == "--format" && i + 1 < argc) {
            g_format = argv[++i];
        }
    }
    

    // 1. CEF Sub-process check (MUST be the absolute first thing)
    CefMainArgs main_args(GetModuleHandle(nullptr));
    int exit_code = CefExecuteProcess(main_args, nullptr, nullptr);
    if (exit_code >= 0) {
        // Boost CEF child process priority (Renderer, GPU, Network, etc.)
        // The parent's HIGH_PRIORITY_CLASS is NOT inherited by child processes on Windows.
        // Setting it here ensures the renderer runs at high priority, reducing animation jitter.
        SetPriorityClass(GetCurrentProcess(), HIGH_PRIORITY_CLASS);
        return exit_code;
    }
    
    // ... (Continue initialization)

    std::cout << "--- DeckLink + CEF Application [build:" << GIT_COMMIT_HASH << "] ---" << std::endl;
    SetConsoleOutputCP(CP_UTF8);
    std::cout << "Initializing..." << std::endl;
    std::cout << "[Config] URL: " << g_targetUrl << std::endl;
    std::cout << "[Config] Format: " << g_format << std::endl;
    std::cout << "[Config] UnmultThresh: " << g_alphaThreshold << std::endl;
    std::cout << "[Config] FilterMode: " << g_filterMode.load() << std::endl;

    // Open file logger
    {
        wchar_t exePath[MAX_PATH];
        GetModuleFileNameW(nullptr, exePath, MAX_PATH);
        std::wstring exeDir = exePath;
        size_t lastSlash = exeDir.find_last_of(L"\\/");
        if (lastSlash != std::wstring::npos) exeDir = exeDir.substr(0, lastSlash);
        std::string logDir(exeDir.begin(), exeDir.end());
        logDir += "\\logs";
        if (g_logger.Open(logDir)) {
            g_logger.Log("[INFO]", "Application started. Log file: " + g_logger.GetPath());
            g_logger.Log("[INFO]", "URL: " + g_targetUrl);
        } else {
            std::cerr << "[Logger] Failed to open log file in: " << logDir << std::endl;
        }
    }

    // Initialize Crash Handler for debugging
    CrashHandler::Initialize();

    // Register Console Control Handler for clean exit on Ctrl+C
    SetConsoleCtrlHandler(ConsoleHandler, TRUE);

    // Set Console Icon
    HWND hwndConsole = GetConsoleWindow();
    if (hwndConsole) {
        HICON hIcon = LoadIcon(GetModuleHandle(NULL), MAKEINTRESOURCE(IDI_APPICON));
        if (hIcon) {
            SendMessage(hwndConsole, WM_SETICON, ICON_SMALL, (LPARAM)hIcon);
            SendMessage(hwndConsole, WM_SETICON, ICON_BIG, (LPARAM)hIcon);
        }
    }

    // High Resolution Timer for accurate 60fps pacing
    timeBeginPeriod(1);

    // Boost Priority to Real-Time-ish (High) to avoid scheduler starvation
    SetPriorityClass(GetCurrentProcess(), HIGH_PRIORITY_CLASS);

    // Initialize CEF early
    if (!g_cefManager.Initialize(GetModuleHandle(nullptr))) {
        std::cerr << "Failed to initialize CEF." << std::endl;
        timeEndPeriod(1);
        return 1;
    }

    // Create application window (Hidden)
    WNDCLASSEXW wc = { sizeof(wc), CS_CLASSDC, WndProc, 0L, 0L, GetModuleHandle(nullptr), nullptr, nullptr, nullptr, nullptr, L"DeckLinkApp", nullptr };
    wc.hIcon = LoadIcon(GetModuleHandle(NULL), MAKEINTRESOURCE(IDI_APPICON));
    wc.hIconSm = LoadIcon(GetModuleHandle(NULL), MAKEINTRESOURCE(IDI_APPICON));
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
    g_shaderManager->SetAlphaThreshold(g_alphaThreshold);
    g_shaderManager->SetLicensed(g_isLicensed);
    if (!g_shaderManager->Initialize(1920, 1080)) {
        std::cerr << "Failed to initialize Shader Manager." << std::endl;
    }
    
    // Apply initial configuration
    {
        std::lock_guard<std::mutex> lock(g_d3dContextMutex);
        g_shaderManager->SetAlphaThreshold(g_alphaThreshold);
    }

    // Initialize DeckLink
    if (g_deckLink.Initialize(g_format))
    {
        std::cout << "DeckLink Initialized." << std::endl;

        // --- Register Render Callback (Reference Pattern) ---
        // This runs INSIDE the DeckLink thread/callback
        g_deckLink.SetRenderCallback([](void* pBuffer) {
            int currentMode = g_viewMode.load();

            // Genlock CEF to hardware clock
            g_cefManager.DriveExternalBeginFrame(currentMode);

            // Helper Lambda for Blitting to Window
            auto BlitToWindow = [&](void* buffer) {
                 if (g_deckLink.IsSimulated() && buffer) {
                     HWND previewHwnd = FindWindowW(L"DeckLinkApp", nullptr);
                     if (previewHwnd) {
                         HDC hdc = GetDC(previewHwnd);
                         if (hdc) {
                             RECT rcClient;
                             GetClientRect(previewHwnd, &rcClient);
                             int winW = rcClient.right - rcClient.left;
                             int winH = rcClient.bottom - rcClient.top;

                             // --- Simulator Color Conversion (DeckLink ARGB -> GDI BGRA) ---
                             // DeckLink format: A, R, G, B in memory
                             // GDI format: B, G, R, 255 in memory
                             uint32_t* p32 = static_cast<uint32_t*>(buffer);
                             for (int i = 0; i < 1920 * 1080; ++i) {
                                 uint32_t p = p32[i];
                                 uint32_t b = (p >> 24) & 0xFF;
                                 uint32_t g = (p >> 16) & 0xFF;
                                 uint32_t r = (p >> 8)  & 0xFF;
                                 p32[i] = b | (g << 8) | (r << 16) | 0xFF000000;
                             }

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
                // 1. Drain all pending frames from the CEF queue to GPU textures (Thread Safe)
                std::lock_guard<std::mutex> lock(g_d3dContextMutex);
                while (renderHandler->HasPendingFrames(1)) {
                    renderHandler->SyncWithGPU();
                }

                // 2. Fetch the two most recent distinct textures for synthesis
                renderHandler->GetSynchronizedTextures(&srvTop, &srvBottom);
            }

            // Fallback: If we only got one frame, duplicate it.
            if (srvTop && !srvBottom) { srvBottom = srvTop; srvBottom->AddRef(); }
            if (!srvTop && srvBottom) { srvTop = srvBottom; srvTop->AddRef(); }

            // Diagnostic: Warn if fields are identical (results in static image fields)
            if (srvTop && srvBottom && srvTop == srvBottom) {
                // static int dupLogCount = 0;
                // if (dupLogCount++ % 60 == 0) std::cerr << "[Warning] Synthesizing identical fields (srvTop == srvBottom)" << std::endl;
            }

            // --- Rendering Logic ---
            // currentMode is already loaded above

            if (currentMode == 2 && g_deckLink.IsSimulated()) {
                // === Mode 2: Progressive Double-Pump (59.94p Window Output - DEBUG ONLY) ===
                if (pBuffer && g_shaderManager && srvTop && srvBottom) {
                    std::lock_guard<std::mutex> lock(g_d3dContextMutex);
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
                        static int lastSameFieldLog = 0;
                        if (srvTop == srvBottom && lastSameFieldLog++ % 60 == 0) {
                             // std::cerr << "[Warning] Identical Fields synthesized (Double Image)" << std::endl;
                        }

                        // (No blackout recovery logs)

                        std::lock_guard<std::mutex> lock(g_d3dContextMutex);
                        int shaderMode = currentMode;
                        if (currentMode == 3) shaderMode = 4; // Map TUI Mode 3 to HLSL Mode 4
                        g_shaderManager->SetViewMode(shaderMode);
                        g_shaderManager->ConvertAndDownload(srvTop, srvBottom, pBuffer);
                        BlitToWindow(pBuffer); // Blit once
                    } else if (g_shaderManager) {
                        // ---- Startup / Empty state ----
                        // At startup before first CEF frame arrives, just output black
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
            SetWindowTextW(hwnd, L"Native DeckLink + CEF [SIMULATOR MODE] - Press F11 to toggle Fullscreen");
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
    g_cefManager.CreateBrowser(hwnd, g_targetUrl, g_pd3dDevice, g_format);

    // Register Fullscreen Callback
    g_cefManager.SetOnFullscreenCallback([hwnd](bool fullscreen) {
        ToggleFullscreen(hwnd);
    });

    g_logger.Log("[INFO]", "Starting Main Loop...");
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

    timeEndPeriod(1);
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

    // Enable D3D11 Multithread protection for safe access from DeckLink callback threads
    ID3D10Multithread* pMultithread = nullptr;
    if (SUCCEEDED(g_pd3dDevice->QueryInterface(IID_PPV_ARGS(&pMultithread)))) {
        pMultithread->SetMultithreadProtected(TRUE);
        pMultithread->Release();
    }

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
        g_deckLink.StopOutput();
        ExitProcess(0);
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
