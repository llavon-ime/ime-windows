#pragma once

#include <cstdint>

#ifdef LLAVON_UI_RUNTIME_EXPORTS
#define LLAVON_UI_RUNTIME_API __declspec(dllexport)
#else
#define LLAVON_UI_RUNTIME_API __declspec(dllimport)
#endif

// One process-wide WinUI Application and STA for the service lifetime. UI clients
// acquire references; releasing the last client keeps the framework available
// until explicit terminal shutdown (WinUI control resources cannot reinitialize).
// Each acquired client must destroy its XAML/windows with invoke before release.
// invoke is synchronous and reports exceptions as HRESULTs at the DLL boundary.
extern "C" {
LLAVON_UI_RUNTIME_API std::int32_t llavon_ui_thread_acquire();
LLAVON_UI_RUNTIME_API std::int32_t llavon_ui_thread_invoke(void (*callback)(void*), void* context);
LLAVON_UI_RUNTIME_API std::int32_t llavon_ui_thread_release();
// Terminal process shutdown, after all clients have released. The caller must
// hold a module reference until this call returns. A stopped runtime cannot restart.
LLAVON_UI_RUNTIME_API std::int32_t llavon_ui_thread_shutdown();
}
