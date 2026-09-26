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

enum llavon_settings_lora_device {
    LLAVON_SETTINGS_LORA_DEVICE_AUTO = 0,
    LLAVON_SETTINGS_LORA_DEVICE_CPU = 1,
    LLAVON_SETTINGS_LORA_DEVICE_CUDA = 2,
};

enum llavon_settings_lora_stage {
    LLAVON_SETTINGS_LORA_IDLE = 0,
    LLAVON_SETTINGS_LORA_CHECKING_MODEL = 1,
    LLAVON_SETTINGS_LORA_DOWNLOADING_MODEL = 2,
    LLAVON_SETTINGS_LORA_MODEL_READY = 3,
    LLAVON_SETTINGS_LORA_PREPARING_DATA = 4,
    LLAVON_SETTINGS_LORA_TRAINING = 5,
    LLAVON_SETTINGS_LORA_EXPORTING_MODEL = 6,
    LLAVON_SETTINGS_LORA_COMPLETED = 7,
    LLAVON_SETTINGS_LORA_FAILED = 8,
    LLAVON_SETTINGS_LORA_CANCELLED = 9,
    LLAVON_SETTINGS_LORA_CHECKING_TRAINER = 10,
    LLAVON_SETTINGS_LORA_INSTALLING_TRAINER = 11,
};

enum llavon_settings_lora_action {
    LLAVON_LORA_CHECK_MODEL = 0,
    LLAVON_LORA_DOWNLOAD_MODEL = 1,
    LLAVON_LORA_CHECK_TRAINER = 2,
    LLAVON_LORA_INSTALL_CPU = 3,
    LLAVON_LORA_INSTALL_CUDA = 4,
    LLAVON_LORA_INSTALL_ROCM = 5,
};

struct llavon_settings_lora_status {
    int32_t stage;
    double progress;
    int32_t model_available;
    int32_t model_update_available;
    const llavon_char16_t* message;
    const llavon_char16_t* model_revision;
    const llavon_char16_t* output_model_path;
    int32_t trainer_available;
    int32_t trainer_assets; // bit 0 CPU, bit 1 CUDA, bit 2 ROCm
    const llavon_char16_t* trainer_version;
    const llavon_char16_t* trainer_backend;
    const llavon_char16_t* trainer_release_version;
    const llavon_char16_t* trainer_message;
};

struct llavon_settings_training_item {
    const llavon_char16_t* event_id;
    const llavon_char16_t* context;
    const llavon_char16_t* answer;
    const llavon_char16_t* reading;
    int32_t revice;
};

struct llavon_settings_lora_history_item {
    int64_t id;
    int64_t parent_id;
    const llavon_char16_t* completed_at_utc;
    const llavon_char16_t* output_model_path;
    size_t record_count;
    size_t cumulative_record_count;
    int64_t optimizer_steps;
    int32_t rank;
    double alpha;
    double dropout;
    const llavon_char16_t* target_modules;
};

struct llavon_settings_lora_options {
    int32_t rank;
    double alpha;
    double dropout;
    int32_t batch_size;
    int32_t gradient_accumulation;
    int32_t epochs;
    int32_t max_steps;
    double learning_rate;
    double weight_decay;
    int32_t warmup_steps;
    double max_gradient_norm;
    int32_t save_every;
    int32_t device;
    int32_t seed;
    int32_t shuffle;
    int32_t max_sequence_length;
    const llavon_char16_t* dtype;
    const llavon_char16_t* target_modules;
    int32_t strength;
    int32_t only_manually_selected;
    int64_t base_run_id;
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
typedef int32_t (*llavon_settings_save_update_notifications_callback)(
    void* context, int32_t enabled);
typedef int32_t (*llavon_settings_start_lora_training_callback)(
    void* context, const llavon_char16_t* const* selected_event_ids,
    size_t selected_event_id_count,
    const struct llavon_settings_lora_options* options, const llavon_char16_t* password);
typedef int32_t (*llavon_settings_refresh_training_items_callback)(
    void* context, struct llavon_settings_training_item* items,
    size_t item_capacity, size_t* item_count, const llavon_char16_t* password,
    int64_t base_run_id);
typedef int32_t (*llavon_settings_delete_training_item_callback)(
    void* context, const llavon_char16_t* event_id);
typedef int32_t (*llavon_settings_get_lora_history_callback)(
    void* context, struct llavon_settings_lora_history_item* items,
    size_t item_capacity, size_t* item_count);
typedef int32_t (*llavon_settings_get_lora_status_callback)(
    void* context, struct llavon_settings_lora_status* status);
typedef int32_t (*llavon_settings_lora_model_action_callback)(
    void* context, int32_t action);
typedef void (*llavon_settings_cancel_lora_callback)(void* context);

enum llavon_settings_protection_action {
    LLAVON_PROTECTION_STATUS = 0,
    LLAVON_PROTECTION_SETUP = 1,
    LLAVON_PROTECTION_ENABLE = 2,
    LLAVON_PROTECTION_DISABLE = 3,
    LLAVON_PROTECTION_CLEANUP = 4,
    LLAVON_PROTECTION_CLEAR_VIEW = 5,
    LLAVON_PROTECTION_RESET = 6,
};
// STATUS: bit 0 = configured, bit 1 = enabled. CLEANUP: deleted file count.
typedef int32_t (*llavon_settings_protection_callback)(
    void* context, int32_t action, const llavon_char16_t* password, size_t* result);

// Supplies a snapshot of devices and the setting used for the current service
// process. Strings and the device array are copied before this call returns.
// This must be called before llavon_settings_ui_start.
LLAVON_SETTINGS_UI_API int32_t llavon_settings_ui_configure_v4(
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
    void* save_width_toggle_context,
    const struct llavon_settings_training_item* training_items,
    size_t training_item_count,
    llavon_settings_refresh_training_items_callback refresh_training_items_callback,
    void* refresh_training_items_context,
    llavon_settings_delete_training_item_callback delete_training_item_callback,
    void* delete_training_item_context,
    llavon_settings_get_lora_history_callback get_lora_history_callback,
    void* get_lora_history_context,
    llavon_settings_start_lora_training_callback start_lora_training_callback,
    void* start_lora_training_context,
    llavon_settings_get_lora_status_callback get_lora_status_callback,
    void* get_lora_status_context,
    llavon_settings_lora_model_action_callback lora_model_action_callback,
    void* lora_model_action_context,
    llavon_settings_cancel_lora_callback cancel_lora_callback,
    void* cancel_lora_context,
    llavon_settings_protection_callback protection_callback, void* protection_context);

// Configures the major update notification setting before the UI thread starts.
LLAVON_SETTINGS_UI_API int32_t llavon_settings_ui_configure_update_notifications(
    int32_t enabled,
    llavon_settings_save_update_notifications_callback save_callback,
    void* save_context);

// Starts the settings UI's dedicated STA thread. Calling this function more
// than once is safe. Returns zero on success.
LLAVON_SETTINGS_UI_API int32_t llavon_settings_ui_start(void);

// These operations only enqueue work for the settings UI thread and return
// immediately. They never execute XAML code on the caller's thread.
LLAVON_SETTINGS_UI_API void llavon_settings_ui_show(void);
LLAVON_SETTINGS_UI_API void llavon_settings_ui_hide(void);
// May be called from the service writer thread; queues the count for the UI.
LLAVON_SETTINGS_UI_API void llavon_settings_ui_set_pending_count(size_t count);

// Shows the input-mode context menu at a screen-coordinate anchor.
LLAVON_SETTINGS_UI_API void llavon_settings_ui_show_context_menu(
    int32_t screen_x, int32_t screen_y);

// Stops and joins the dedicated UI thread. Returns zero when the thread has
// completed its XAML shutdown sequence.
LLAVON_SETTINGS_UI_API int32_t llavon_settings_ui_stop(void);

#ifdef __cplusplus
}
#endif
