#pragma once

#include <string>

#ifdef _WIN32
#include <Windows.h>
#endif

class CrashHandler {
public:
    // Initialize crash handler - call at application startup
    static void Initialize();
    
    // Get the singleton instance
    static CrashHandler& GetInstance();
    
    // Generate a crash dump manually (for testing)
    static void ForceCrashDump();

private:
    CrashHandler() = default;
    ~CrashHandler() = default;
    
    // Prevent copying
    CrashHandler(const CrashHandler&) = delete;
    CrashHandler& operator=(const CrashHandler&) = delete;
    
#ifdef _WIN32
    // Exception filter callback
    static LONG WINAPI UnhandledExceptionHandler(_EXCEPTION_POINTERS* exceptionInfo);
    
    // Create minidump file
    static bool WriteMiniDump(_EXCEPTION_POINTERS* exceptionInfo);
    
    // Generate dump file name with timestamp
    static std::wstring GenerateDumpFileName();
#endif
};
