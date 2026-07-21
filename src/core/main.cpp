#ifdef _WIN32
#include <Windows.h>
#include <shellapi.h>
#include <tchar.h>
#include <d3d11.h>
#include <d3d11_1.h>
#include <d3d10.h>
#include <dxgi.h>
#include <mmsystem.h>
#pragma comment(lib, "winmm.lib")
#include <wincrypt.h>
#pragma comment(lib, "crypt32.lib")
#include <conio.h>
#include <wininet.h>
#pragma comment(lib, "wininet.lib")
#else
#include <unistd.h>
#include <sys/ioctl.h>
#include <mach-o/dyld.h>
#include <limits.h>
#include <signal.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <termios.h>
#include <fcntl.h>
#include <cstdio>
#include "include/wrapper/cef_library_loader.h"
#include "MacWindow.h"
using HWND = void*;
#endif

#include <iostream>
#include <iomanip>
#include <chrono>
#include <thread>
#include <mutex>
#include <vector>
#include <deque>
#include <fstream>
#include <sstream>
#include <ctime>
#include <atomic>

#include "DeckLinkDevice.h"
#include "CefManager.h"
#include "CefRenderHandler.h"
#include "ShaderManager.h"

#ifndef _WIN32
#include <cstdlib> // For atexit

static struct termios g_originalTermios;
static bool g_termiosSaved = false;

static void RestoreTerminal() {
    if (g_termiosSaved) {
        tcsetattr(STDIN_FILENO, TCSANOW, &g_originalTermios);
    }
    // Show cursor and switch back to main screen buffer
    std::cout << "\x1b[?1049l\x1b[?25h" << std::flush;
}

static void InitTerminalRawMode() {
    if (tcgetattr(STDIN_FILENO, &g_originalTermios) == 0) {
        g_termiosSaved = true;
        atexit(RestoreTerminal);
        
        struct termios raw = g_originalTermios;
        raw.c_lflag &= ~(ICANON | ECHO);
        tcsetattr(STDIN_FILENO, TCSANOW, &raw);
    }
}

static int _getch_nonblock() {
    int oldf = fcntl(STDIN_FILENO, F_GETFL, 0);
    fcntl(STDIN_FILENO, F_SETFL, oldf | O_NONBLOCK);
    int ch = getchar();
    fcntl(STDIN_FILENO, F_SETFL, oldf);
    return ch;
}
#endif

#ifdef _WIN32
#include "CrashHandler.h"
#include "../resource.h"
#endif

// ============================================================
// File Logger - Writes to logs/app_YYYYMMDD_HHMMSS.log
// and buffers recent logs for TUI
// ============================================================
class FileLogger {
public:
    FileLogger() {}

    bool Open(const std::string& logDir) {
#ifdef _WIN32
        CreateDirectoryA(logDir.c_str(), nullptr);
#else
        mkdir(logDir.c_str(), 0777);
#endif

        auto now = std::chrono::system_clock::now();
        std::time_t t = std::chrono::system_clock::to_time_t(now);
        struct tm tm_info;
#ifdef _WIN32
        localtime_s(&tm_info, &t);
#else
        localtime_r(&t, &tm_info);
#endif
        char buf[64];
        strftime(buf, sizeof(buf), "app_%Y%m%d_%H%M%S.log", &tm_info);

#ifdef _WIN32
        m_path = logDir + "\\" + buf;
#else
        m_path = logDir + "/" + buf;
#endif
        m_file.open(m_path, std::ios::out | std::ios::trunc);
        return m_file.is_open();
    }

    void Log(const std::string& tag, const std::string& msg) {
        auto now = std::chrono::system_clock::now();
        std::time_t t = std::chrono::system_clock::to_time_t(now);
        struct tm tm_info;
#ifdef _WIN32
        localtime_s(&tm_info, &t);
#else
        localtime_r(&t, &tm_info);
#endif
        char timebuf[32];
        strftime(timebuf, sizeof(timebuf), "%H:%M:%S", &tm_info);

        std::ostringstream oss;
        oss << "[" << timebuf << "] " << tag << " " << msg;
        std::string line = oss.str();

        std::lock_guard<std::mutex> lock(m_mutex);
        if (m_file.is_open()) {
            char fulltime[32];
            strftime(fulltime, sizeof(fulltime), "%Y-%m-%d %H:%M:%S", &tm_info);
            m_file << fulltime << " " << tag << " " << msg << "\n";
            m_file.flush();
        }

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
#ifdef _WIN32
    HANDLE hOut = GetStdHandle(STD_OUTPUT_HANDLE);
    if (hOut == INVALID_HANDLE_VALUE) return false;
    DWORD dwMode = 0;
    if (!GetConsoleMode(hOut, &dwMode)) return false;
    dwMode |= ENABLE_VIRTUAL_TERMINAL_PROCESSING;
    return SetConsoleMode(hOut, dwMode) != 0;
#else
    return true; // macOS console supports ANSI out of the box
#endif
}

// Data
#ifdef _WIN32
static ID3D11Device*            g_pd3dDevice = nullptr;
static ID3D11DeviceContext*     g_pd3dDeviceContext = nullptr;
static IDXGISwapChain*          g_pSwapChain = nullptr;
static ID3D11RenderTargetView*  g_mainRenderTargetView = nullptr;
#endif
static std::mutex               g_d3dContextMutex;
static bool                     g_appDone = false;
static std::atomic<bool>        g_isShutdownComplete(false);

// Managers & Devices (Global)
static CefManager g_cefManager;
static std::unique_ptr<ShaderManager> g_shaderManager;
static DeckLinkDevice g_deckLink;
std::atomic<int> g_viewMode(0); // 0=Interlace, 1=Diff, 2=Progressive, 3=30pBlend
static std::string g_targetUrl;
static std::string g_format = "5994i"; // "5994i" or "50i"
std::atomic<int> g_filterMode(0); // 0=None, 1=3-tap, 2=5-tap vertical LPF
static float g_alphaThreshold = 0.000f;

#include <queue>
// Shared preview queue for VSync window blitting
static std::queue<std::vector<uint8_t>> g_previewQueue;
static std::mutex                       g_previewQueueMutex;

#ifndef _WIN32
static MacWindowHandle g_macWindow = nullptr;
#endif

// Forward declarations of helper functions
#ifdef _WIN32
IDXGIAdapter* SelectBestAdapter();
bool CreateDeviceD3D(HWND hWnd);
void CleanupDeviceD3D();
void CreateRenderTarget();
void CleanupRenderTarget();
LRESULT WINAPI WndProc(HWND hWnd, UINT msg, WPARAM wParam, LPARAM lParam);
void ToggleFullscreen(HWND hWnd);
BOOL WINAPI ConsoleHandler(DWORD ctrlType);
#else
void SignalHandler(int sig) {
    g_appDone = true;
}
#endif

#ifndef _WIN32
#include <Security/Security.h>
#include <CoreFoundation/CoreFoundation.h>

static std::vector<uint8_t> Base64Decode(const std::string& in) {
    std::vector<uint8_t> out;
    std::vector<int> T(256, -1);
    for (int i = 0; i < 64; i++) {
        T["ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/"[i]] = i;
    }
    int val = 0, valb = -8;
    for (char c : in) {
        if (T[c] == -1) continue;
        val = (val << 6) + T[c];
        valb += 6;
        if (valb >= 0) {
            out.push_back((val >> valb) & 0xFF);
            valb -= 8;
        }
    }
    return out;
}
#endif

bool IsLicenseValid(const std::string& key) {
#ifdef _WIN32
    if (key.length() < 10 || key[8] != '-') return false;
    std::string dateStr = key.substr(0, 8);
    std::string sigStr = key.substr(9);

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
#else
    if (key.length() < 10 || key[8] != '-') return false;
    std::string dateStr = key.substr(0, 8);
    std::string sigStr = key.substr(9);

    std::vector<uint8_t> sigData = Base64Decode(sigStr);
    if (sigData.empty()) return false;

    uint8_t pubBlob[] = {
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

    // Construct DER (PKCS#1) public key manually
    std::vector<uint8_t> der;
    der.push_back(0x30); // SEQUENCE
    der.push_back(0x82); // 2-byte length
    size_t seqLenPos = der.size();
    der.push_back(0x00);
    der.push_back(0x00);

    // Modulus INTEGER
    der.push_back(0x02); // INTEGER
    der.push_back(0x82); // 2-byte length
    der.push_back(0x01); // 257 bytes
    der.push_back(0x01);
    der.push_back(0x00); // 0x00 padding to prevent sign bit interpretation
    
    // Copy Modulus (256 bytes) reversing from Little-Endian to Big-Endian
    for (int i = 0; i < 256; ++i) {
        der.push_back(pubBlob[20 + 255 - i]);
    }

    // Exponent INTEGER
    der.push_back(0x02); // INTEGER
    der.push_back(0x03); // 3 bytes length
    der.push_back(0x01); // 0x01
    der.push_back(0x00); // 0x00
    der.push_back(0x01); // 0x01 (65537)

    // Update SEQUENCE length
    size_t totalLen = der.size() - 4; // Excluding SEQUENCE header bytes (30 82 XX XX)
    der[seqLenPos] = (totalLen >> 8) & 0xFF;
    der[seqLenPos + 1] = totalLen & 0xFF;

    CFDataRef keyData = CFDataCreate(kCFAllocatorDefault, der.data(), der.size());
    if (!keyData) return false;

    // Attributes for SecKeyCreateWithData
    CFMutableDictionaryRef attributes = CFDictionaryCreateMutable(kCFAllocatorDefault, 0, &kCFTypeDictionaryKeyCallBacks, &kCFTypeDictionaryValueCallBacks);
    CFDictionarySetValue(attributes, kSecAttrKeyType, kSecAttrKeyTypeRSA);
    CFDictionarySetValue(attributes, kSecAttrKeyClass, kSecAttrKeyClassPublic);

    CFErrorRef error = nullptr;
    SecKeyRef publicKey = SecKeyCreateWithData(keyData, attributes, &error);
    CFRelease(keyData);
    CFRelease(attributes);

    if (!publicKey) {
        if (error) CFRelease(error);
        return false;
    }

    CFDataRef messageData = CFDataCreate(kCFAllocatorDefault, (const UInt8*)dateStr.c_str(), dateStr.length());
    CFDataRef signatureData = CFDataCreate(kCFAllocatorDefault, sigData.data(), sigData.size());

    bool isValidSig = false;
    if (messageData && signatureData) {
        isValidSig = SecKeyVerifySignature(
            publicKey,
            kSecKeyAlgorithmRSASignatureMessagePKCS1v15SHA256,
            messageData,
            signatureData,
            &error
        );
    }

    if (messageData) CFRelease(messageData);
    if (signatureData) CFRelease(signatureData);
    CFRelease(publicKey);
    if (error) CFRelease(error);

    if (!isValidSig) return false;

    time_t now = time(nullptr);
    struct tm tm_now;
    localtime_r(&now, &tm_now);

    char buf[9];
    strftime(buf, sizeof(buf), "%Y%m%d", &tm_now);
    std::string current_dateStr(buf);

    if (current_dateStr > dateStr) return false; 
    return true; 
#endif
}

#ifndef _WIN32
std::string GetExeDir() {
    char path[PATH_MAX];
    uint32_t size = sizeof(path);
    if (_NSGetExecutablePath(path, &size) == 0) {
        std::string sPath(path);
        size_t pos = sPath.find_last_of("/");
        if (pos != std::string::npos) {
            return sPath.substr(0, pos);
        }
    }
    return ".";
}
#endif

// Helper: get terminal column width (cross-platform)
static int GetTerminalCols() {
#ifdef _WIN32
    CONSOLE_SCREEN_BUFFER_INFO csbi;
    if (GetConsoleScreenBufferInfo(GetStdHandle(STD_OUTPUT_HANDLE), &csbi)) {
        return csbi.srWindow.Right - csbi.srWindow.Left + 1;
    }
    return 80;
#else
    struct winsize ws;
    if (ioctl(STDOUT_FILENO, TIOCGWINSZ, &ws) == 0 && ws.ws_col > 0) {
        return ws.ws_col;
    }
    return 80;
#endif
}

// Helper: clamp a plain-text string to maxCols visible characters
// (strips ANSI escapes for counting, returns truncated raw string)
static std::string ClampLine(const std::string& s, int maxCols) {
    // Walk the string: skip ANSI escape sequences when counting visible cols.
    int visible = 0;
    bool inEsc = false;
    std::string out;
    out.reserve(s.size());
    for (size_t i = 0; i < s.size(); ++i) {
        char c = s[i];
        if (inEsc) {
            out += c;
            if ((c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z')) {
                inEsc = false;
            }
        } else if (c == '\x1b' && i + 1 < s.size() && s[i+1] == '[') {
            out += c;
            inEsc = true;
        } else {
            if (visible >= maxCols - 1) {
                out += '>';
                break;
            }
            out += c;
            ++visible;
        }
    }
    return out;
}

// --- TUI URL Input & Validation Globals ---
static std::string g_inputUrl;
static std::atomic<bool> g_tuiChanged(false);
static std::atomic<int> g_validationState(0); // 0=Idle, 1=Checking, 2=Accepted, 3=Rejected
static std::string g_validationStatusText;
static std::mutex g_validationMutex;
static std::atomic<bool> g_isValidating(false);

struct UrlCheckResult {
    bool success = false;
    int statusCode = 0;
    std::string statusText;
};

#ifdef _WIN32
static bool CheckFileExists(const std::string& path) {
    DWORD dwAttrib = GetFileAttributesA(path.c_str());
    return (dwAttrib != INVALID_FILE_ATTRIBUTES && 
           !(dwAttrib & FILE_ATTRIBUTE_DIRECTORY));
}

static UrlCheckResult CheckUrlAlive(const std::string& url) {
    UrlCheckResult result;
    if (url.rfind("file://", 0) == 0) {
        std::string path = url.substr(7);
        while (!path.empty() && path[0] == '/') {
            path = path.substr(1);
        }
        if (path.size() >= 2 && path[1] == '|') {
            path[1] = ':';
        }
        if (CheckFileExists(path)) {
            result.success = true;
            result.statusCode = 200;
            result.statusText = "Accepted (Local File)";
        } else {
            result.success = false;
            result.statusCode = 404;
            result.statusText = "404 File Not Found";
        }
        return result;
    }

    HINTERNET hInternet = InternetOpenA("CEFDecklink-TUI", INTERNET_OPEN_TYPE_DIRECT, NULL, NULL, 0);
    if (!hInternet) {
        result.statusText = "Failed to init Internet";
        return result;
    }

    // Set connection and receive timeouts to 3 seconds
    DWORD timeout = 3000;
    InternetSetOptionA(hInternet, INTERNET_OPTION_CONNECT_TIMEOUT, &timeout, sizeof(timeout));
    InternetSetOptionA(hInternet, INTERNET_OPTION_RECEIVE_TIMEOUT, &timeout, sizeof(timeout));

    DWORD flags = INTERNET_FLAG_RELOAD | INTERNET_FLAG_NO_CACHE_WRITE | INTERNET_FLAG_NO_AUTO_REDIRECT;
    if (url.rfind("https://", 0) == 0) {
        flags |= INTERNET_FLAG_SECURE;
    }

    HINTERNET hUrl = InternetOpenUrlA(hInternet, url.c_str(), NULL, 0, flags, 0);
    if (!hUrl) {
        DWORD err = GetLastError();
        result.statusCode = 0;
        if (err == ERROR_INTERNET_TIMEOUT) {
            result.statusText = "Timeout (3s)";
        } else {
            result.statusText = "Connection failed (" + std::to_string(err) + ")";
        }
        InternetCloseHandle(hInternet);
        return result;
    }

    char statusBuffer[256] = {0};
    DWORD bufferLength = sizeof(statusBuffer);
    if (HttpQueryInfoA(hUrl, HTTP_QUERY_STATUS_CODE, statusBuffer, &bufferLength, NULL)) {
        try {
            result.statusCode = std::stoi(statusBuffer);
        } catch (...) {
            result.statusCode = 0;
        }
        
        if (result.statusCode >= 200 && result.statusCode < 400) {
            result.success = true;
            result.statusText = "Accepted";
        } else {
            char phraseBuffer[256] = {0};
            DWORD phraseLength = sizeof(phraseBuffer);
            if (HttpQueryInfoA(hUrl, HTTP_QUERY_STATUS_TEXT, phraseBuffer, &phraseLength, NULL)) {
                result.statusText = std::string(statusBuffer) + " " + phraseBuffer;
            } else {
                result.statusText = std::string(statusBuffer) + " Error";
            }
        }
    } else {
        result.statusText = "Query failed";
    }

    InternetCloseHandle(hUrl);
    InternetCloseHandle(hInternet);
    return result;
}

static bool SaveConfig(const std::string& url, float alpha, const std::string& format, int filterMode) {
    wchar_t exePath[MAX_PATH];
    GetModuleFileNameW(nullptr, exePath, MAX_PATH);
    std::wstring exeDir = exePath;
    size_t lastSlash = exeDir.find_last_of(L"\\/");
    if (lastSlash != std::wstring::npos) {
        exeDir = exeDir.substr(0, lastSlash);
    }
    
    // 1. Save to executable directory (where app runs from)
    std::wstring configPath = exeDir + L"\\config.json";
    {
        std::ofstream file(configPath, std::ios::out | std::ios::trunc);
        if (file.is_open()) {
            file << "{\n";
            file << "    \"url\": \"" << url << "\",\n";
            file << "    \"unmult_thresh\": " << std::fixed << std::setprecision(4) << alpha << ",\n";
            file << "    \"format\": \"" << format << "\",\n";
            file << "    \"il_filter_mode\": " << filterMode << "\n";
            file << "}\n";
        }
    }

    // 2. Save to project root directory (so rebuilding doesn't overwrite build folder's config with outdated root config)
    std::wstring rootConfigPath = exeDir + L"\\..\\..\\config.json";
    {
        std::ofstream file(rootConfigPath, std::ios::out | std::ios::trunc);
        if (file.is_open()) {
            file << "{\n";
            file << "    \"url\": \"" << url << "\",\n";
            file << "    \"unmult_thresh\": " << std::fixed << std::setprecision(4) << alpha << ",\n";
            file << "    \"format\": \"" << format << "\",\n";
            file << "    \"il_filter_mode\": " << filterMode << "\n";
            file << "}\n";
        }
    }
    return true;
}
#else
#include <sys/stat.h>
static bool CheckFileExists(const std::string& path) {
    struct stat buffer;
    return (stat(path.c_str(), &buffer) == 0 && S_ISREG(buffer.st_mode));
}

static UrlCheckResult CheckUrlAlive(const std::string& url) {
    UrlCheckResult result;
    if (url.rfind("file://", 0) == 0) {
        std::string path = url.substr(7);
        while (!path.empty() && path[0] == '/') {
            path = path.substr(1);
        }
        if (CheckFileExists("/" + path)) {
            result.success = true;
            result.statusCode = 200;
            result.statusText = "Accepted (Local File)";
        } else {
            result.success = false;
            result.statusCode = 404;
            result.statusText = "404 File Not Found";
        }
        return result;
    }

    // Actual connection checker for macOS/Linux using curl
    std::string safeUrl;
    for (char c : url) {
        if (std::isalnum(c) || c == ':' || c == '/' || c == '.' || c == '-' || c == '_' || c == '?' || c == '=' || c == '&' || c == '%') {
            safeUrl.push_back(c);
        }
    }
    
    std::string cmd = "curl -o /dev/null -s -w \"%{http_code}\" --max-time 3 \"" + safeUrl + "\"";
    FILE* pipe = popen(cmd.c_str(), "r");
    if (!pipe) {
        result.success = false;
        result.statusCode = 0;
        result.statusText = "Failed to run curl";
        return result;
    }
    
    char buffer[128];
    std::string output = "";
    while (!feof(pipe)) {
        if (fgets(buffer, 128, pipe) != NULL)
            output += buffer;
    }
    pclose(pipe);
    
    int code = 0;
    try {
        code = std::stoi(output);
    } catch (...) {
        code = 0;
    }
    
    result.statusCode = code;
    if (code >= 200 && code < 400) {
        result.success = true;
        result.statusText = "Accepted";
    } else {
        result.success = false;
        if (code == 0) {
            result.statusText = "Connection failed / Timeout";
        } else {
            result.statusText = "HTTP Error " + std::to_string(code);
        }
    }
    return result;
}

static bool SaveConfig(const std::string& url, float alpha, const std::string& format, int filterMode) {
    std::string exeDir = GetExeDir();
    
    // 1. Save to executable directory
    std::string configPath = exeDir + "/config.json";
    {
        std::ofstream file(configPath, std::ios::out | std::ios::trunc);
        if (file.is_open()) {
            file << "{\n";
            file << "    \"url\": \"" << url << "\",\n";
            file << "    \"unmult_thresh\": " << std::fixed << std::setprecision(4) << alpha << ",\n";
            file << "    \"format\": \"" << format << "\",\n";
            file << "    \"il_filter_mode\": " << filterMode << "\n";
            file << "}\n";
        }
    }

    // 2. Save to project root directory
    std::string rootConfigPath = exeDir + "/../../config.json";
    {
        std::ofstream file(rootConfigPath, std::ios::out | std::ios::trunc);
        if (file.is_open()) {
            file << "{\n";
            file << "    \"url\": \"" << url << "\",\n";
            file << "    \"unmult_thresh\": " << std::fixed << std::setprecision(4) << alpha << ",\n";
            file << "    \"format\": \"" << format << "\",\n";
            file << "    \"il_filter_mode\": " << filterMode << "\n";
            file << "}\n";
        }
    }
    return true;
}
#endif

static void StartUrlValidation(const std::string& url) {
    if (g_isValidating.load()) return;
    g_isValidating.store(true);
    g_validationState.store(1); // Checking
    {
        std::lock_guard<std::mutex> lock(g_validationMutex);
        g_validationStatusText = "Checking...";
    }
    g_tuiChanged = true;

    std::thread([url]() {
        UrlCheckResult res = CheckUrlAlive(url);
        if (res.success) {
            g_validationState.store(2); // Accepted
            {
                std::lock_guard<std::mutex> lock(g_validationMutex);
                g_validationStatusText = "Accepted";
            }
            g_targetUrl = url;
            g_cefManager.LoadURL(url);
            g_inputUrl.clear(); // Clear URL input box on success
            SaveConfig(g_targetUrl, g_alphaThreshold, g_format, g_filterMode.load());
            g_logger.Log("[TUI]", "URL changed and saved: " + url);
        } else {
            g_validationState.store(3); // Rejected
            {
                std::lock_guard<std::mutex> lock(g_validationMutex);
                g_validationStatusText = "Rejected: " + res.statusText;
            }
            g_logger.Log("[TUI]", "URL connection rejected: " + url + " (" + res.statusText + ")");
        }
        g_isValidating.store(false);
        g_tuiChanged = true;
    }).detach();
}

// TUI Dashboard Console Output Helper
void LogStatus(bool locked, double deckLinkFps, int cefFps, int uniqueInInterval, float alphaThreshold, uint64_t totalCefFrames, int pendingCount) {
    static bool tuiInitialized = false;
    if (!tuiInitialized) {
        EnableVTMode();
        // Switch to alternate screen buffer, clear screen, show cursor
        std::cout << "\x1b[?1049h\x1b[2J\x1b[?25h" << std::flush;
        tuiInitialized = true;
    }

    std::string modeStr;
    int vMode = g_viewMode.load();
    if (vMode == 0) modeStr = "\x1b[1m\x1b[32mInterlace (0)\x1b[0m";
    else if (vMode == 1) modeStr = "\x1b[1m\x1b[31mDiff Mode (1)\x1b[0m";
    else if (vMode == 2) modeStr = "\x1b[1m\x1b[31mProgressive (2)\x1b[0m";
    else if (vMode == 3) modeStr = "\x1b[1m\x1b[31m30p Blend (3)\x1b[0m";

    auto recentLogs = g_logger.GetRecentLogs();

    // Get terminal width for line clamping
    int cols = GetTerminalCols();

    // Clamp URL to avoid wrapping
    std::string urlDisplay = ClampLine(g_targetUrl, cols - 8); // "  URL: " prefix = 7 chars

    std::ostringstream oss;
    // Move cursor to home (0,0) without clearing screen (prevents scrollback buffer growth)
    oss << "\x1b[H";
    // Disable auto-wrap while drawing (re-enabled at the end)
    oss << "\x1b[?7l";

    oss << "\x1b[36m===============================================================================\x1b[K\x1b[0m\n";
    oss << "  \x1b[1m\x1b[37m\xF0\x9F\x8D\x8C CEFDecklink Live Status Dashboard \xF0\x9F\x8D\x8C\x1b[K\x1b[0m\n";
    oss << "  \x1b[36mURL: \x1b[0m" << urlDisplay << "\x1b[K\n";
    oss << "  \x1b[36mDevTools: \x1b[0mhttp://localhost:9222\x1b[K\n";
    oss << "\x1b[36m===============================================================================\x1b[K\x1b[0m\n";
    
    std::string dlFpsColor;
    std::string cefFpsColor;
    if (g_format == "50i") {
        dlFpsColor = (deckLinkFps >= 24.9 && deckLinkFps <= 25.1) ? "\x1b[1m\x1b[32m" : "\x1b[1m\x1b[31m";
        cefFpsColor = (cefFps >= 60) ? "\x1b[1m\x1b[31m" : "\x1b[1m\x1b[32m";
    } else {
        dlFpsColor = (deckLinkFps >= 29.9 && deckLinkFps <= 30.1) ? "\x1b[1m\x1b[32m" : "\x1b[1m\x1b[31m";
        cefFpsColor = (cefFps >= 70) ? "\x1b[1m\x1b[31m" : "\x1b[1m\x1b[32m";
    }
    std::string queueColor = (pendingCount >= 13) ? "\x1b[1m\x1b[31m" : "\x1b[1m\x1b[32m";
    std::string keyerStr = g_deckLink.GetKeyerMode() ? "\x1b[1m\x1b[32mExternal\x1b[0m" : "\x1b[1m\x1b[31mInternal\x1b[0m";
    
    oss << "  \x1b[32m[Status]\x1b[0m   DeckLink: " << dlFpsColor << std::fixed << std::setprecision(2) << deckLinkFps << " fps\x1b[0m | "
        << "CEF: " << cefFpsColor << cefFps << " fps\x1b[0m | "
        << "Queue: " << queueColor << pendingCount << "\x1b[0m | "
        << "Keyer: " << keyerStr << "\x1b[K\n";
        
    std::string formatColor = (g_format == "5994i" || g_format == "50i") ? "\x1b[1m\x1b[32m" : "\x1b[1m\x1b[31m";
    std::string threshColor = (alphaThreshold >= 0.0f && alphaThreshold <= 0.1f) ? "\x1b[1m\x1b[32m" : "\x1b[1m\x1b[31m";
    
    oss << "  \x1b[32m[Config]\x1b[0m   ViewMode: " << modeStr 
        << " | Format: " << formatColor << g_format << "\x1b[0m"
        << " | UnmultThresh: " << threshColor << std::fixed << std::setprecision(4) << alphaThreshold << "\x1b[0m";
        
    int fMode = g_filterMode.load();
    std::string filterStr;
    if (fMode == 1) filterStr = "\x1b[1m\x1b[32m3tap\x1b[0m";
    else if (fMode == 2) filterStr = "\x1b[1m\x1b[31m5tap\x1b[0m";
    else filterStr = "\x1b[1m\x1b[31mNone\x1b[0m";
    
    oss << " | Filter: " << filterStr << "\x1b[K\n";
    oss << "\x1b[36m-------------------------------------------------------------------------------\x1b[K\x1b[0m\n";
    oss << "  \x1b[33mRecent Events / Logs:\x1b[K\x1b[0m\n";
 
    for (int i = 0; i < 5; ++i) {
        if (i < (int)recentLogs.size()) {
            std::string logLine = recentLogs[i];
            size_t skipsPos = logLine.find("skips(d:");
            if (skipsPos != std::string::npos) {
                size_t dPos = skipsPos + 8; // length of "skips(d:"
                size_t uPos = logLine.find(" u:", dPos);
                if (uPos != std::string::npos) {
                    size_t uValPos = uPos + 3; // length of " u:"
                    size_t endPos = logLine.find(")", uValPos);
                    if (endPos != std::string::npos) {
                        std::string dStr = logLine.substr(dPos, uPos - dPos);
                        std::string uStr = logLine.substr(uValPos, endPos - uValPos);
                        try {
                            int dVal = std::stoi(dStr);
                            int uVal = std::stoi(uStr);
                            
                            std::string dColor = "\x1b[32m"; // green (0)
                            if (dVal == 1) dColor = "\x1b[33m"; // yellow (1)
                            else if (dVal >= 2) dColor = "\x1b[31m"; // red (2+)
                            
                            std::string uColor = "\x1b[32m"; // green (0)
                            if (uVal >= 1) uColor = "\x1b[31m"; // red (1+)
                            
                            std::string newSkips = "skips(d:" + dColor + std::to_string(dVal) + "\x1b[0m u:" + uColor + std::to_string(uVal) + "\x1b[0m)";
                            logLine.replace(skipsPos, endPos + 1 - skipsPos, newSkips);
                        } catch (...) {
                            // ignore conversion errors
                        }
                    }
                }
            }
            oss << "   " << ClampLine(logLine, cols - 4) << "\x1b[K\n";
        } else {
            oss << "\x1b[K\n";
        }
    }
 
    oss << "\x1b[36m===============================================================================\x1b[K\x1b[0m\n";
#ifdef _WIN32
    oss << "  \x1b[90mControls: Ctrl+O(output-mode) | Ctrl+F(int-filter-mode) | Ctrl+K(keyer-mode)\x1b[K\x1b[0m\n";
    oss << "            \x1b[90mCtrl+A/Z(unmult-th:0.001) | Ctrl+Up/Down(unmult-th:0.1) | Ctrl+R(reload) | Ctrl+C(exit)\x1b[K\x1b[0m\n";
#else
    oss << "  \x1b[90mControls: Ctrl+P(output-mode) | Ctrl+F(int-filter-mode) | Ctrl+K(keyer-mode)\x1b[K\x1b[0m\n";
    oss << "            \x1b[90m</>(unmult-th:0.001) | Ctrl+[/](unmult-th:0.1) | Ctrl+R(reload) | Ctrl+C(exit)\x1b[K\x1b[0m\n";
#endif
    oss << "\x1b[36m===============================================================================\x1b[K\x1b[0m\n";
 
    std::string valStatus;
    int state = g_validationState.load();
    {
        std::lock_guard<std::mutex> lock(g_validationMutex);
        if (state == 1) {
            valStatus = "\x1b[1m\x1b[33mChecking...\x1b[0m";
        } else if (state == 2) {
            valStatus = "\x1b[1m\x1b[32mAccepted\x1b[0m";
        } else if (state == 3) {
            valStatus = "\x1b[1m\x1b[31mRejected: " + g_validationStatusText + "\x1b[0m";
        } else {
            valStatus = "\x1b[90mReady\x1b[0m";
        }
    }
 
    std::string keyHint = " \x1b[90m[Ctrl+Enter to Connect]\x1b[0m";
    std::string inputDisplay = ClampLine(g_inputUrl, cols - 30);
 
    oss << "  \x1b[36m>> URL:\x1b[0m " << inputDisplay << keyHint << "\x1b[K\n";
    oss << "  \x1b[36m>> Status:\x1b[0m " << valStatus << "\x1b[K\n";
    oss << "\x1b[36m===============================================================================\x1b[K\x1b[0m\n";
 
    // Position physical console cursor right after visible URL text on line 19
    oss << "\x1b[19;" << (11 + inputDisplay.length()) << "H";
    // Show cursor
    oss << "\x1b[?25h";

    // Re-enable auto-wrap
    oss << "\x1b[?7h";

    std::cout << oss.str() << std::flush;
}

// Helper to load config.json
bool LoadConfig(std::string& url, float& alpha, std::string& format) {
#ifdef _WIN32
    wchar_t exePath[MAX_PATH];
    GetModuleFileNameW(nullptr, exePath, MAX_PATH);
    std::wstring exeDir = exePath;
    size_t lastSlash = exeDir.find_last_of(L"\\/");
    if (lastSlash != std::wstring::npos) {
        exeDir = exeDir.substr(0, lastSlash);
    }
    
    std::wstring configPath = exeDir + L"\\config.json";
    std::ifstream file(configPath);
#else
    std::string exeDir = GetExeDir(); // .app/Contents/MacOS
    // 1st: look next to the .app bundle (for ZIP distribution)
    std::string appSiblingDir = exeDir + "/../../..";
    std::string configPath = appSiblingDir + "/config.json";
    std::ifstream file(configPath);
    // 2nd: fallback to exe directory (for in-bundle / dev usage)
    if (!file.is_open()) {
        configPath = exeDir + "/config.json";
        file.open(configPath);
    }
#endif
    if (!file.is_open()) return false;

    std::string content((std::istreambuf_iterator<char>(file)), std::istreambuf_iterator<char>());
    
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
    

    
    return true;
}

// RenderFrame handles Main Thread tasks: Input processing & CEF Message Loop & Logging
void RenderFrame(HWND hWnd) {
    bool changed = false;
#ifdef _WIN32
    while (_kbhit()) {
        int ch = _getch();
        bool ctrlPressed = (GetAsyncKeyState(VK_CONTROL) & 0x8000) != 0;
        bool shiftPressed = (GetAsyncKeyState(VK_SHIFT) & 0x8000) != 0;
        bool altPressed = (GetAsyncKeyState(VK_MENU) & 0x8000) != 0;
        bool modified = ctrlPressed || shiftPressed || altPressed;

        if (ch == 0 || ch == 224) {
            if (_kbhit()) {
                int extra = _getch();
                if (ch == 224) {
                    if (extra == 141 || (extra == 72 && ctrlPressed)) { // Ctrl+Up (Unmult 0.1)
                        g_alphaThreshold += 0.1f;
                        if (g_alphaThreshold > 1.0f) g_alphaThreshold = 1.0f;
                        changed = true;
                        g_tuiChanged = true;
                    } else if (extra == 145 || (extra == 80 && ctrlPressed)) { // Ctrl+Down (Unmult 0.1)
                        g_alphaThreshold -= 0.1f;
                        if (g_alphaThreshold < 0.0f) g_alphaThreshold = 0.0f;
                        changed = true;
                        g_tuiChanged = true;
                    }
                }
            }
            continue;
        }

        if (ch == 10 || (ch == 13 && ctrlPressed)) {
            StartUrlValidation(g_inputUrl);
        } else if (ch == 8) { // Backspace
            if (!g_inputUrl.empty()) {
                g_inputUrl.pop_back();
                g_tuiChanged = true;
            }
        } else if (ch >= 32 && ch <= 126 && !altPressed) {
            g_inputUrl.push_back((char)ch);
            g_tuiChanged = true;
        } else if (ch == 15) { // Ctrl+O
            int vm = g_viewMode.load();
            g_viewMode.store((vm + 1) % 4);
            g_tuiChanged = true;
        } else if (ch == 1) { // Ctrl+A (Unmult 0.001)
            g_alphaThreshold += 0.001f;
            if (g_alphaThreshold > 1.0f) g_alphaThreshold = 1.0f;
            changed = true;
        } else if (ch == 26) { // Ctrl+Z (Unmult 0.001)
            g_alphaThreshold -= 0.001f;
            if (g_alphaThreshold < 0.0f) g_alphaThreshold = 0.0f;
            changed = true;
        } else if (ch == 6) { // Ctrl+F
            int fm = g_filterMode.load();
            g_filterMode.store((fm + 1) % 3);
            changed = true;
        } else if (ch == 18) { // Ctrl+R
            g_cefManager.ReloadIgnoreCache();
        } else if (ch == 11) { // Ctrl+K
            bool current = g_deckLink.GetKeyerMode();
            g_deckLink.SetKeyerMode(!current);
            g_logger.Log("App", std::string("Keyer mode switched to: ") + (!current ? "Internal" : "External"));
            changed = true;
        }
    }

    // Window Hotkeys
    if (GetForegroundWindow() == hWnd) {
        if (GetAsyncKeyState(VK_F11) & 0x8000) {
            static auto lastF11Time = std::chrono::steady_clock::now();
            auto nowF11 = std::chrono::steady_clock::now();
            if (std::chrono::duration_cast<std::chrono::milliseconds>(nowF11 - lastF11Time).count() > 300) {
                ToggleFullscreen(hWnd);
                lastF11Time = nowF11;
            }
        }
        
        bool ctrlPressed = (GetAsyncKeyState(VK_CONTROL) & 0x8000) != 0;
        bool shiftPressed = (GetAsyncKeyState(VK_SHIFT) & 0x8000) != 0;
        bool altPressed = (GetAsyncKeyState(VK_MENU) & 0x8000) != 0;
        bool modified = ctrlPressed || shiftPressed || altPressed;
        
        if (modified && ((GetAsyncKeyState('R') & 0x8000) || (GetAsyncKeyState(VK_F5) & 0x8000))) {
            static auto lastReloadTime = std::chrono::steady_clock::now();
            auto nowReload = std::chrono::steady_clock::now();
            if (std::chrono::duration_cast<std::chrono::milliseconds>(nowReload - lastReloadTime).count() > 500) {
                g_cefManager.ReloadIgnoreCache();
                lastReloadTime = nowReload;
            }
        }
    }
#else
    while (true) {
        int ch = _getch_nonblock();
        if (ch == EOF) break;
        if (ch == 27) { // ESC or Ctrl+[
            int ch2 = _getch_nonblock();
            if (ch2 == '[') {
                int seq[8];
                int seq_len = 0;
                while (seq_len < 8) {
                    int next_ch = _getch_nonblock();
                    if (next_ch == EOF) break;
                    seq[seq_len++] = next_ch;
                }
                // xterm / terminal.app sequences:
                // Ctrl+Up is usually ESC [ 1 ; 5 A
                // Ctrl+Down is usually ESC [ 1 ; 5 B
                if (seq_len >= 4 && seq[0] == '1' && seq[1] == ';' && seq[2] == '5') {
                    if (seq[3] == 'A') { // Ctrl+Up (Unmult 0.1)
                        g_alphaThreshold += 0.1f;
                        if (g_alphaThreshold > 1.0f) g_alphaThreshold = 1.0f;
                        changed = true;
                        g_tuiChanged = true;
                    } else if (seq[3] == 'B') { // Ctrl+Down (Unmult 0.1)
                        g_alphaThreshold -= 0.1f;
                        if (g_alphaThreshold < 0.0f) g_alphaThreshold = 0.0f;
                        changed = true;
                        g_tuiChanged = true;
                    }
                }
            } else if (ch2 == EOF) {
                // Ctrl+[ (increase by 0.1)
                g_alphaThreshold += 0.1f;
                if (g_alphaThreshold > 1.0f) g_alphaThreshold = 1.0f;
                changed = true;
                g_tuiChanged = true;
            }
            continue;
        }

        // macOS standard control codes:
        // Ctrl+Enter is typically CR (13) or LF (10). Ctrl+O is 15.
        // Backspace is typically DEL (127) or BS (8).
        if (ch == 10 || ch == 13) {
            StartUrlValidation(g_inputUrl);
        } else if (ch == 127 || ch == 8) {
            if (!g_inputUrl.empty()) {
                g_inputUrl.pop_back();
                g_tuiChanged = true;
            }
        } else if (ch == '<') { // '<' (Shift + ,) - Unmult 0.001 increase
            g_alphaThreshold += 0.001f;
            if (g_alphaThreshold > 1.0f) g_alphaThreshold = 1.0f;
            changed = true;
        } else if (ch == '>') { // '>' (Shift + .) - Unmult 0.001 decrease
            g_alphaThreshold -= 0.001f;
            if (g_alphaThreshold < 0.0f) g_alphaThreshold = 0.0f;
            changed = true;
        } else if (ch >= 32 && ch <= 126) {
            g_inputUrl.push_back((char)ch);
            g_tuiChanged = true;
        } else if (ch == 16) { // Ctrl+P (behaves like Ctrl+O)
            int vm = g_viewMode.load();
            g_viewMode.store((vm + 1) % 4);
            g_tuiChanged = true;
        } else if (ch == 29) { // Ctrl+] (behaves like Ctrl+Down, decrease by 0.1)
            g_alphaThreshold -= 0.1f;
            if (g_alphaThreshold < 0.0f) g_alphaThreshold = 0.0f;
            changed = true;
            g_tuiChanged = true;
        } else if (ch == 6) { // Ctrl+F
            int fm = g_filterMode.load();
            g_filterMode.store((fm + 1) % 3);
            changed = true;
        } else if (ch == 18) { // Ctrl+R
            g_cefManager.ReloadIgnoreCache();
        } else if (ch == 11) { // Ctrl+K
            bool current = g_deckLink.GetKeyerMode();
            g_deckLink.SetKeyerMode(!current);
            g_logger.Log("App", std::string("Keyer mode switched to: ") + (!current ? "Internal" : "External"));
            changed = true;
        }
    }
#endif
    
    if (changed) {
        if (g_shaderManager) {
            std::lock_guard<std::mutex> lock(g_d3dContextMutex);
            g_shaderManager->SetAlphaThreshold(g_alphaThreshold);
            g_shaderManager->SetFilterMode(g_filterMode.load());
        }
        SaveConfig(g_targetUrl, g_alphaThreshold, g_format, g_filterMode.load());
    }

    // CEF Message Loop
    g_cefManager.DoMessageLoopWork();

    // --- Wait for DeckLink callback ---
    bool deckLinkReady = g_deckLink.WaitForNextFrame(0);
    
    static int frameCount = 0;
    static auto lastLogTime = std::chrono::steady_clock::now();
    static auto lastHeartbeatTime = std::chrono::steady_clock::now();
    static uint64_t lastCefTotal = 0;
    
    if (deckLinkReady) {
        frameCount++;
    }

    auto now = std::chrono::steady_clock::now();
    uint64_t elapsedMs = std::chrono::duration_cast<std::chrono::milliseconds>(now - lastLogTime).count();
    bool tuiChanged = g_tuiChanged.exchange(false);

    static double s_deckLinkFps = 0.0;
    static int s_cefFps = 0;
    static int s_normalizedUnique = 0;
    static uint64_t s_totalCef = 0;
    static int s_pendingCef = 0;

    if (elapsedMs >= 1000) {
        static uint64_t lastUniqueTotal = 0;
        double deckLinkFps = (double)frameCount * 1000.0 / elapsedMs;
        
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

        s_deckLinkFps = deckLinkFps;
        s_cefFps = (int)(cefFps + 0.5);
        s_normalizedUnique = (int)(normalizedUnique + 0.5);
        s_totalCef = totalCef;
        s_pendingCef = pendingCef;

        lastCefTotal = totalCef;

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
        tuiChanged = true;
    }

    if (tuiChanged) {
        LogStatus(true, s_deckLinkFps, s_cefFps, s_normalizedUnique, g_alphaThreshold, s_totalCef, s_pendingCef);
    }

    uint64_t hbElapsedSec = std::chrono::duration_cast<std::chrono::seconds>(now - lastHeartbeatTime).count();
    if (hbElapsedSec >= 60) {
        lastHeartbeatTime = now;
        auto handler = g_cefManager.GetRenderHandler();
        uint64_t totalCef = handler ? handler->GetTotalFrameCount() : 0;
        std::ostringstream oss;
        oss << "alive cefTotal=" << totalCef;
        g_logger.Log("[HEARTBEAT]", oss.str());


    }
#ifdef _WIN32
    if (g_deckLink.IsSimulated() && g_pd3dDeviceContext && g_pSwapChain && g_mainRenderTargetView && g_shaderManager) {
        static std::vector<uint8_t> localBuffer(1920 * 1080 * 4, 0);
        static int s_fieldIndex = 0; // 0 = Top, 1 = Bottom
        bool hasFrame = false;

        // Fetch a new frame from the queue only when we are starting a new field cycle
        if (s_fieldIndex == 0) {
            std::lock_guard<std::mutex> lock(g_previewQueueMutex);
            
            // Catch up logic: if the queue has built up too much delay, drain older frames.
            while (g_previewQueue.size() > 3) {
                g_previewQueue.pop();
            }

            if (!g_previewQueue.empty()) {
                g_previewQueue.front().swap(localBuffer);
                g_previewQueue.pop();
            }
        }

        if (!localBuffer.empty()) {
            hasFrame = true;
        }
        
        if (hasFrame) {
            RECT rc;
            GetClientRect(hWnd, &rc);
            int winW = rc.right - rc.left;
            int winH = rc.bottom - rc.top;

            std::lock_guard<std::mutex> lock(g_d3dContextMutex);
            
            g_shaderManager->PresentPreview(localBuffer.data(), g_mainRenderTargetView, s_fieldIndex, winW, winH);
            g_pSwapChain->Present(1, 0); // VSync enabled
            
            s_fieldIndex = (s_fieldIndex + 1) % 2;
        }
    }
#else
    if (g_macWindow) {
        ProcessMacWindowEvents();
    }
#endif
}

// Main code
int main(int argc, char** argv)
{
    // 0. Configuration Logic
    g_targetUrl = "https://telophub.duckdns.org/graphics/preview.html?machineId=8efb67b2-2fac-418a-9bd9-0284852ccd86";
    
    LoadConfig(g_targetUrl, g_alphaThreshold, g_format);
    
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
    g_inputUrl = "";
    
#ifdef _WIN32
    CefMainArgs main_args(GetModuleHandle(nullptr));
#else
    CefScopedLibraryLoader library_loader;
    if (!library_loader.LoadInMain()) {
        std::cerr << "Failed to load CEF library." << std::endl;
        return 1;
    }
    CefMainArgs main_args(argc, argv);
#endif
    int exit_code = CefExecuteProcess(main_args, nullptr, nullptr);
    if (exit_code >= 0) {
#ifdef _WIN32
        SetPriorityClass(GetCurrentProcess(), HIGH_PRIORITY_CLASS);
#endif
        return exit_code;
    }
    
#ifdef _WIN32
    std::cout << "--- DeckLink + CEF Application [build:" << GIT_COMMIT_HASH << "] ---" << std::endl;
    SetConsoleOutputCP(CP_UTF8);
#else
    std::cout << "--- DeckLink + CEF Application [macOS] ---" << std::endl;
#endif
    std::cout << "Initializing..." << std::endl;
    std::cout << "[Config] URL: " << g_targetUrl << std::endl;
    std::cout << "[Config] Format: " << g_format << std::endl;
    std::cout << "[Config] UnmultThresh: " << g_alphaThreshold << std::endl;
    std::cout << "[Config] FilterMode: " << g_filterMode.load() << std::endl;

    // Open file logger
    {
#ifdef _WIN32
        wchar_t exePath[MAX_PATH];
        GetModuleFileNameW(nullptr, exePath, MAX_PATH);
        std::wstring exeDir = exePath;
        size_t lastSlash = exeDir.find_last_of(L"\\/");
        if (lastSlash != std::wstring::npos) exeDir = exeDir.substr(0, lastSlash);
        std::string logDir;
        logDir.reserve(exeDir.size());
        for (wchar_t wc : exeDir) {
            logDir.push_back(static_cast<char>(wc));
        }
        logDir += "\\logs";
#else
        std::string logDir = GetExeDir() + "/logs";
#endif
        if (g_logger.Open(logDir)) {
            g_logger.Log("[INFO]", "Application started. Log file: " + g_logger.GetPath());
            g_logger.Log("[INFO]", "URL: " + g_targetUrl);
        } else {
            std::cerr << "[Logger] Failed to open log file in: " << logDir << std::endl;
        }
    }

#ifdef _WIN32
    CrashHandler::Initialize();
    SetConsoleCtrlHandler(ConsoleHandler, TRUE);

    HWND hwndConsole = GetConsoleWindow();
    if (hwndConsole) {
        HICON hIcon = LoadIcon(GetModuleHandle(NULL), MAKEINTRESOURCE(IDI_APPICON));
        if (hIcon) {
            SendMessage(hwndConsole, WM_SETICON, ICON_SMALL, (LPARAM)hIcon);
            SendMessage(hwndConsole, WM_SETICON, ICON_BIG, (LPARAM)hIcon);
        }
    }

    timeBeginPeriod(1);
    SetPriorityClass(GetCurrentProcess(), HIGH_PRIORITY_CLASS);
#else
    signal(SIGINT, SignalHandler);
    signal(SIGTERM, SignalHandler);
#endif

    // Initialize CEF
#ifdef _WIN32
    if (!g_cefManager.Initialize(GetModuleHandle(nullptr))) {
#else
    if (!g_cefManager.Initialize(main_args)) {
#endif
        std::cerr << "Failed to initialize CEF." << std::endl;
#ifdef _WIN32
        timeEndPeriod(1);
#endif
        return 1;
    }

#ifdef _WIN32
    WNDCLASSEXW wc = { sizeof(wc), CS_CLASSDC, WndProc, 0L, 0L, GetModuleHandle(nullptr), nullptr, nullptr, nullptr, nullptr, L"DeckLinkApp", nullptr };
    wc.hIcon = LoadIcon(GetModuleHandle(NULL), MAKEINTRESOURCE(IDI_APPICON));
    wc.hIconSm = LoadIcon(GetModuleHandle(NULL), MAKEINTRESOURCE(IDI_APPICON));
    ::RegisterClassExW(&wc);
    HWND hwnd = ::CreateWindowW(wc.lpszClassName, L"Native DeckLink + CEF", WS_OVERLAPPEDWINDOW, 100, 100, 1280, 800, nullptr, nullptr, wc.hInstance, nullptr);

    if (!CreateDeviceD3D(hwnd))
    {
        CleanupDeviceD3D();
        ::UnregisterClassW(wc.lpszClassName, wc.hInstance);
        return 1;
    }

    g_shaderManager = std::make_unique<ShaderManager>(g_pd3dDevice, g_pd3dDeviceContext);
#else
    HWND hwnd = nullptr;
    g_shaderManager = std::make_unique<ShaderManager>();
#endif
    g_shaderManager->SetAlphaThreshold(g_alphaThreshold);
    g_shaderManager->SetFilterMode(g_filterMode.load());
    if (!g_shaderManager->Initialize(1920, 1080)) {
        std::cerr << "Failed to initialize Shader Manager." << std::endl;
    }
    
    {
        std::lock_guard<std::mutex> lock(g_d3dContextMutex);
        g_shaderManager->SetAlphaThreshold(g_alphaThreshold);
    }

    if (g_deckLink.Initialize(g_format))
    {
        std::cout << "DeckLink Initialized." << std::endl;

        g_deckLink.SetRenderCallback([](void* pBuffer) {
            int currentMode = g_viewMode.load();

            g_cefManager.DriveExternalBeginFrame(currentMode);

            auto BlitToWindow = [&](void* buffer) {
#ifdef _WIN32
                 if (g_deckLink.IsSimulated() && buffer) {
                     std::vector<uint8_t> tempWriteBuffer(1920 * 1080 * 4);
                     memcpy(tempWriteBuffer.data(), buffer, 1920 * 1080 * 4);

                     std::lock_guard<std::mutex> lock(g_previewQueueMutex);
                     g_previewQueue.push(std::move(tempWriteBuffer));
                     
                     // Hard threshold drop: keep the queue size minimal (under 5) if something backs up
                     while (g_previewQueue.size() > 5) {
                         g_previewQueue.pop();
                     }
                 }
#else
                 if (g_deckLink.IsSimulated() && buffer && g_macWindow) {
                     BlitToMacWindow(g_macWindow, buffer, 1920, 1080);
                 }
#endif
            };
            
            CefFrameResource srvTop = nullptr;
            CefFrameResource srvBottom = nullptr;

            auto renderHandler = g_cefManager.GetRenderHandler();
            
            if (renderHandler) {
                std::lock_guard<std::mutex> lock(g_d3dContextMutex);
                while (renderHandler->HasPendingFrames(1)) {
                    renderHandler->SyncWithGPU();
                }

                renderHandler->GetSynchronizedTextures(&srvTop, &srvBottom);
            }

            if (srvTop && !srvBottom) { srvBottom = srvTop; srvBottom->AddRef(); }
            if (!srvTop && srvBottom) { srvTop = srvBottom; srvTop->AddRef(); }

            if (pBuffer) {
                if (srvTop && srvBottom && g_shaderManager) {
                    std::lock_guard<std::mutex> lock(g_d3dContextMutex);
                    int shaderMode = currentMode;
                    if (currentMode == 3) shaderMode = 4; // Map TUI Mode 3 to HLSL Mode 4
                    g_shaderManager->SetViewMode(shaderMode);
                    g_shaderManager->ConvertAndDownload(srvTop, srvBottom, pBuffer);
                    BlitToWindow(pBuffer);
                } else if (g_shaderManager) {
                    memset(pBuffer, 0, 1920 * 1080 * 4);
                }
            }
            
            if (srvTop) srvTop->Release();
            if (srvBottom) srvBottom->Release();
        });

        g_deckLink.StartOutput();

#ifdef _WIN32
        if (g_deckLink.IsSimulated()) {
            SetWindowTextW(hwnd, L"Native DeckLink + CEF [SIMULATOR MODE] - Press F11 to toggle Fullscreen");
            ::ShowWindow(hwnd, SW_SHOW);
            ::UpdateWindow(hwnd);
        } else {
            ::ShowWindow(hwnd, SW_HIDE);
        }
#else
        if (g_deckLink.IsSimulated()) {
            g_macWindow = CreateMacWindow("Native DeckLink + CEF [SIMULATOR MODE] - Press F11 to toggle Fullscreen", 1280, 800);
            ShowMacWindow(g_macWindow, true);
        }
#endif
    }
    else
    {
        std::cerr << "Failed to initialize DeckLink!" << std::endl; 
    }

#ifdef _WIN32
    g_cefManager.CreateBrowser(hwnd, g_targetUrl, g_pd3dDevice, g_format);
    
    g_cefManager.SetOnFullscreenCallback([hwnd](bool fullscreen) {
        ToggleFullscreen(hwnd);
    });
#else
    g_cefManager.CreateBrowser(hwnd, g_targetUrl, g_format);
#endif

    g_logger.Log("[INFO]", "Starting Main Loop...");
    std::cout << "Starting Main Loop..." << std::endl;

    try {
#ifdef _WIN32
        while (!g_appDone)
        {
            MSG msg;
            while (::PeekMessage(&msg, nullptr, 0U, 0U, PM_REMOVE))
            {
                ::TranslateMessage(&msg);
                ::DispatchMessage(&msg);
                if (msg.message == WM_QUIT)
                    g_appDone = true;
            }
            RenderFrame(hwnd);
            std::this_thread::sleep_for(std::chrono::milliseconds(1));
        }
#else
        InitTerminalRawMode();
        while (!g_appDone)
        {
            RenderFrame(hwnd);
            std::this_thread::sleep_for(std::chrono::milliseconds(2));
        }
#endif
    }
    catch (const std::exception& e) {
        std::cerr << "\n[EXCEPTION] Caught C++ exception in main loop: " << e.what() << std::endl;
#ifdef _WIN32
        CrashHandler::ForceCrashDump();
#endif
    }
    catch (...) {
        std::cerr << "\n[EXCEPTION] Caught unknown exception in main loop" << std::endl;
#ifdef _WIN32
        CrashHandler::ForceCrashDump();
#endif
    }
    
    g_deckLink.StopOutput();
    g_cefManager.Shutdown();

#ifdef _WIN32
    CleanupDeviceD3D();
    ::DestroyWindow(hwnd);
    ::UnregisterClassW(wc.lpszClassName, wc.hInstance);
    timeEndPeriod(1);
#else
    if (g_macWindow) {
        DestroyMacWindow(g_macWindow);
        g_macWindow = nullptr;
    }
#endif
    
    std::cout << "\x1b[?1049l\x1b[?25h\nExiting." << std::endl;
    g_isShutdownComplete = true;
    return 0;
}

#ifdef _WIN32
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

        char descStr[128];
        wcstombs_s(nullptr, descStr, sizeof(descStr), desc.Description, _TRUNCATE);

        std::cout << "  [" << i << "] " << descStr
                  << " (Vendor: 0x" << std::hex << desc.VendorId << std::dec
                  << ", VRAM: " << (desc.DedicatedVideoMemory / 1024 / 1024) << " MB)" << std::endl;

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
    D3D_FEATURE_LEVEL featureLevel;
    const D3D_FEATURE_LEVEL featureLevelArray[2] = { D3D_FEATURE_LEVEL_11_0, D3D_FEATURE_LEVEL_10_0, };
    
    IDXGIAdapter* adapter = SelectBestAdapter();
    D3D_DRIVER_TYPE driverType = adapter ? D3D_DRIVER_TYPE_UNKNOWN : D3D_DRIVER_TYPE_HARDWARE;
    
    HRESULT res = D3D11CreateDeviceAndSwapChain(adapter, driverType, nullptr, createDeviceFlags, featureLevelArray, 2, D3D11_SDK_VERSION, &sd, &g_pSwapChain, &g_pd3dDevice, &featureLevel, &g_pd3dDeviceContext);
    
    if (res == DXGI_ERROR_UNSUPPORTED) { 
        if (adapter) adapter->Release();
        adapter = nullptr;
        res = D3D11CreateDeviceAndSwapChain(nullptr, D3D_DRIVER_TYPE_WARP, nullptr, createDeviceFlags, featureLevelArray, 2, D3D11_SDK_VERSION, &sd, &g_pSwapChain, &g_pd3dDevice, &featureLevel, &g_pd3dDeviceContext);
    }
    
    if (adapter) adapter->Release();
    if (res != S_OK) return false;

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
        }
        return 0;

    case WM_CLOSE:
        g_appDone = true;
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
        
        // Wait for the main thread to complete shutdown (up to 5 seconds by OS policy)
        while (!g_isShutdownComplete) {
            Sleep(10);
        }
        
        return TRUE;
    }
    return FALSE;
}

void ToggleFullscreen(HWND hWnd) {
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
#endif
