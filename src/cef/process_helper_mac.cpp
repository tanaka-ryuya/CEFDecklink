#include <include/cef_app.h>

// CEF Helper sub-process entry point for macOS
int main(int argc, char* argv[]) {
    CefMainArgs main_args(argc, argv);
    return CefExecuteProcess(main_args, nullptr, nullptr);
}
