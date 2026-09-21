#pragma once

#include <stdint.h>
#include <stddef.h>

#if defined(_WIN32)
#if defined(LLAVON_SETTINGS_UI_EXPORTS)
#define LLAVON_SETTINGS_UI_API __declspec(dllexport)
#else
#define LLAVON_SETTINGS_UI_API __declspec(dllimport)
#endif
#else
#define LLAVON_SETTINGS_UI_API
#endif

#ifdef __cplusplus
using llavon_char16_t = char16_t;
extern "C" {
#else
typedef uint16_t llavon_char16_t;
#endif

enum llavon_settings_inference_backend {
    LLAVON_SETTINGS_BACKEND_AUTO = 0,
    LLAVON_SETTINGS_BACKEND_CPU = 1,
    LLAVON_SETTINGS_BACKEND_CUDA = 2,
    LLAVON_SETTINGS_BACKEND_VULKAN = 3,
};

enum llavon_settings_inference_device_type {
    LLAVON_SETTINGS_DEVICE_CPU = 0,
    LLAVON_SETTINGS_DEVICE_GPU = 1,
    LLAVON_SETTINGS_DEVICE_INTEGRATED_GPU = 2,
};

struct llavon_settings_inference_device {
    int32_t backend;
    int32_t device_type;
    const llavon_char16_t* device_id;
    const llavon_char16_t* name;
    const llavon_char16_t* description;
    uint64_t memory_total;
};

struct llavon_settings_custom_name {
    const llavon_char16_t* name;
    const llavon_char16_t* const* readings;
    size_t reading_count;
};

typedef int32_t (*llavon_settings_save_inference_callback)(
    void* context, int32_t backend, const llavon_char16_t* device_id);
typedef int32_t (*llavon_settings_save_model_path_callback)(
    void* context, const llavon_char16_t* model_path);
typedef int32_t (*llavon_settings_save_custom_names_callback)(
    void* context, const struct llavon_settings_custom_name* custom_names,
    size_t custom_name_count);
typedef int32_t (*llavon_settings_save_width_toggle_callback)(
    void* context, int32_t enabled);

// Supplies a snapshot of devices and the setting used for the current service
// process. Strings and the device array are copied before this call returns.
// This must be called before llavon_settings_ui_start.
LLAVON_SETTINGS_UI_API int32_t llavon_settings_ui_configure(
    const struct llavon_settings_inference_device* devices,
    size_t device_count,
    int32_t selected_backend,
    const llavon_char16_t* selected_device_id,
    const struct llavon_settings_inference_device* active_device,
    int32_t gpu_offload,
    int32_t fell_back_to_cpu,
    llavon_settings_save_inference_callback save_callback,
    void* save_context,
    const llavon_char16_t* model_path,
    llavon_settings_save_model_path_callback save_model_path_callback,
    void* save_model_path_context,
    const struct llavon_settings_custom_name* custom_names,
    size_t custom_name_count,
    llavon_settings_save_custom_names_callback save_custom_names_callback,
    void* save_custom_names_context,
    int32_t shift_space_width_toggle_enabled,
    llavon_settings_save_width_toggle_callback save_width_toggle_callback,
    void* save_width_toggle_context);

// Starts the settings UI's dedicated STA thread. Calling this function more
// than once is safe. Returns zero on success.
LLAVON_SETTINGS_UI_API int32_t llavon_settings_ui_start(void);

// These operations only enqueue work for the settings UI thread and return
// immediately. They never execute XAML code on the caller's thread.
LLAVON_SETTINGS_UI_API void llavon_settings_ui_show(void);
LLAVON_SETTINGS_UI_API void llavon_settings_ui_hide(void);

// Shows the input-mode context menu at a screen-coordinate anchor.
LLAVON_SETTINGS_UI_API void llavon_settings_ui_show_context_menu(
    int32_t screen_x, int32_t screen_y);

// Stops and joins the dedicated UI thread. Returns zero when the thread has
// completed its XAML shutdown sequence.
LLAVON_SETTINGS_UI_API int32_t llavon_settings_ui_stop(void);

#ifdef __cplusplus
}
#endif
