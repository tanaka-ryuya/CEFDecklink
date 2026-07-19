#pragma once
#include <string>

// Opaque pointer representing the macOS window
typedef void* MacWindowHandle;

#ifdef __cplusplus
extern "C" {
#endif

MacWindowHandle CreateMacWindow(const std::string& title, int width, int height);
void ShowMacWindow(MacWindowHandle window, bool show);
void BlitToMacWindow(MacWindowHandle window, const void* buffer, int width, int height);
void DestroyMacWindow(MacWindowHandle window);
void ProcessMacWindowEvents();

#ifdef __cplusplus
}
#endif
