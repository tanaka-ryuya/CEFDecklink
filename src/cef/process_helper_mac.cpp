#include <include/cef_app.h>
#include <include/wrapper/cef_library_loader.h>

// CEF Helper sub-process entry point for macOS
int main(int argc, char* argv[]) {
    CefScopedLibraryLoader library_loader;
    if (!library_loader.LoadInHelper())
        return 1;

    CefMainArgs main_args(argc, argv);
    return CefExecuteProcess(main_args, nullptr, nullptr);
}
