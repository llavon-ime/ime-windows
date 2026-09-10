#pragma once

#include <stdint.h>

#if defined(_WIN32)
#if defined(LLAVON_CANDIDATE_UI_EXPORTS)
#define LLAVON_CANDIDATE_UI_API __declspec(dllexport)
#else
#define LLAVON_CANDIDATE_UI_API __declspec(dllimport)
#endif
#else
#define LLAVON_CANDIDATE_UI_API
#endif

#ifdef __cplusplus
extern "C" {
#endif

typedef struct llavon_candidate_ui_string_view {
    const wchar_t* data;
    uint32_t length;
} llavon_candidate_ui_string_view;

typedef struct llavon_candidate_ui_presentation {
    uint32_t struct_size;
    uint64_t owner_window;
    int32_t anchor_x;
    int32_t anchor_y;
    uint32_t candidate_count;
    const llavon_candidate_ui_string_view* candidates;
    uint32_t selection_index;
    uint32_t layout_columns;
    uint32_t number_column;
    uint8_t can_prev_page;
    uint8_t can_next_page;
    // This consumes the legacy structure's tail padding so anchor_top starts
    // outside the legacy sizeof boundary and can be detected via struct_size.
    uint32_t reserved_anchor_rect;
    // Top edge of the TSF GetTextExt rectangle. Older callers may omit it and
    // the candidate UI will fall back to anchor_y.
    int32_t anchor_top;
} llavon_candidate_ui_presentation;

// Starts the candidate UI's dedicated STA thread. Calling this function more
// than once is safe. Returns zero on success.
LLAVON_CANDIDATE_UI_API int32_t llavon_candidate_ui_start(void);

// Copies a complete presentation snapshot and queues it to the candidate UI
// thread. The caller may release all pointed-to strings after this returns.
LLAVON_CANDIDATE_UI_API int32_t llavon_candidate_ui_present(
    const llavon_candidate_ui_presentation* presentation);

// These calls only enqueue work and never execute XAML on the caller's thread.
LLAVON_CANDIDATE_UI_API void llavon_candidate_ui_hide(void);

// Stops and joins the dedicated UI thread.
LLAVON_CANDIDATE_UI_API int32_t llavon_candidate_ui_stop(void);

#ifdef __cplusplus
}
#endif
