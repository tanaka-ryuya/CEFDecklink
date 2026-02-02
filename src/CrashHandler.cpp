#include "CrashHandler.h"
#include <DbgHelp.h>
#include <iostream>
#include <sstream>
#include <iomanip>
#include <chrono>

#pragma comment(lib, "dbghelp.lib")

void CrashHandler::Initialize() {
    SetUnhandledExceptionFilter(UnhandledExceptionHandler);
    std::cout << "[CrashHandler] Initialized. Crash dumps will be written to current directory." << std::endl;
}

CrashHandler& CrashHandler::GetInstance() {
    static CrashHandler instance;
    return instance;
}

void CrashHandler::ForceCrashDump() {
    std::cout << "[CrashHandler] Forcing crash dump generation for testing..." << std::endl;
    __try {
        // Intentional access violation
        int* p = nullptr;
        *p = 42;
    }
    __except(WriteMiniDump(GetExceptionInformation()) ? EXCEPTION_EXECUTE_HANDLER : EXCEPTION_CONTINUE_SEARCH) {
        std::cout << "[CrashHandler] Test crash dump generated." << std::endl;
    }
}

LONG WINAPI CrashHandler::UnhandledExceptionHandler(_EXCEPTION_POINTERS* exceptionInfo) {
    std::cerr << "\n========================================" << std::endl;
    std::cerr << "UNHANDLED EXCEPTION DETECTED!" << std::endl;
    std::cerr << "========================================" << std::endl;
    
    if (exceptionInfo && exceptionInfo->ExceptionRecord) {
        DWORD code = exceptionInfo->ExceptionRecord->ExceptionCode;
        void* address = exceptionInfo->ExceptionRecord->ExceptionAddress;
        
        std::cerr << "Exception Code: 0x" << std::hex << std::uppercase << code << std::dec << std::endl;
        std::cerr << "Exception Address: 0x" << std::hex << std::uppercase << (uintptr_t)address << std::dec << std::endl;
        
        // Common exception codes
        switch (code) {
            case EXCEPTION_ACCESS_VIOLATION:
                std::cerr << "Type: Access Violation" << std::endl;
                if (exceptionInfo->ExceptionRecord->NumberParameters >= 2) {
                    ULONG_PTR isWrite = exceptionInfo->ExceptionRecord->ExceptionInformation[0];
                    ULONG_PTR faultAddress = exceptionInfo->ExceptionRecord->ExceptionInformation[1];
                    std::cerr << "Fault: " << (isWrite ? "Write" : "Read") << " at 0x" 
                              << std::hex << std::uppercase << faultAddress << std::dec << std::endl;
                }
                break;
            case EXCEPTION_ARRAY_BOUNDS_EXCEEDED:
                std::cerr << "Type: Array Bounds Exceeded" << std::endl;
                break;
            case EXCEPTION_STACK_OVERFLOW:
                std::cerr << "Type: Stack Overflow" << std::endl;
                break;
            case EXCEPTION_ILLEGAL_INSTRUCTION:
                std::cerr << "Type: Illegal Instruction" << std::endl;
                break;
            case EXCEPTION_INT_DIVIDE_BY_ZERO:
                std::cerr << "Type: Integer Divide by Zero" << std::endl;
                break;
            default:
                std::cerr << "Type: Unknown Exception" << std::endl;
                break;
        }
    }
    
    std::cerr << "========================================" << std::endl;
    std::cerr << "Creating crash dump..." << std::endl;
    
    bool dumpCreated = WriteMiniDump(exceptionInfo);
    
    if (dumpCreated) {
        std::cerr << "Crash dump created successfully!" << std::endl;
        std::cerr << "Please analyze the .dmp file with Visual Studio or WinDbg." << std::endl;
    } else {
        std::cerr << "Failed to create crash dump." << std::endl;
    }
    
    std::cerr << "========================================" << std::endl;
    
    // Return EXCEPTION_EXECUTE_HANDLER to terminate gracefully
    return EXCEPTION_EXECUTE_HANDLER;
}

bool CrashHandler::WriteMiniDump(_EXCEPTION_POINTERS* exceptionInfo) {
    std::wstring dumpFileName = GenerateDumpFileName();
    
    HANDLE hFile = CreateFileW(
        dumpFileName.c_str(),
        GENERIC_WRITE,
        0,
        nullptr,
        CREATE_ALWAYS,
        FILE_ATTRIBUTE_NORMAL,
        nullptr
    );
    
    if (hFile == INVALID_HANDLE_VALUE) {
        std::wcerr << L"Failed to create dump file: " << dumpFileName << std::endl;
        return false;
    }
    
    MINIDUMP_EXCEPTION_INFORMATION mdei;
    mdei.ThreadId = GetCurrentThreadId();
    mdei.ExceptionPointers = exceptionInfo;
    mdei.ClientPointers = FALSE;
    
    // MiniDumpWithFullMemory includes full memory dump (larger file but more info)
    // MiniDumpNormal is smaller but sufficient for most debugging
    MINIDUMP_TYPE dumpType = static_cast<MINIDUMP_TYPE>(
        MiniDumpWithDataSegs |
        MiniDumpWithHandleData |
        MiniDumpWithThreadInfo |
        MiniDumpWithUnloadedModules
    );
    
    BOOL success = MiniDumpWriteDump(
        GetCurrentProcess(),
        GetCurrentProcessId(),
        hFile,
        dumpType,
        exceptionInfo ? &mdei : nullptr,
        nullptr,
        nullptr
    );
    
    CloseHandle(hFile);
    
    if (success) {
        std::wcout << L"Crash dump saved to: " << dumpFileName << std::endl;
        return true;
    } else {
        std::wcerr << L"MiniDumpWriteDump failed. Error: " << GetLastError() << std::endl;
        return false;
    }
}

std::wstring CrashHandler::GenerateDumpFileName() {
    // Get current time
    auto now = std::chrono::system_clock::now();
    auto time_t_now = std::chrono::system_clock::to_time_t(now);
    
    struct tm timeinfo;
    localtime_s(&timeinfo, &time_t_now);
    
    // Format: crash_YYYYMMDD_HHMMSS.dmp
    std::wostringstream oss;
    oss << L"crash_"
        << std::setfill(L'0')
        << std::setw(4) << (timeinfo.tm_year + 1900)
        << std::setw(2) << (timeinfo.tm_mon + 1)
        << std::setw(2) << timeinfo.tm_mday
        << L"_"
        << std::setw(2) << timeinfo.tm_hour
        << std::setw(2) << timeinfo.tm_min
        << std::setw(2) << timeinfo.tm_sec
        << L".dmp";
    
    return oss.str();
}
