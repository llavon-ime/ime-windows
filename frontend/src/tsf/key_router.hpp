#pragma once

#include <windows.h>

#include <optional>
#include <string>
#include <utility>
#include <variant>

#include "engine/engine_types.hpp"

namespace tsf::keys {

struct Modifiers {
    bool shift = false;
    bool ctrl = false;
    bool alt = false;
};

struct KeyPress {
    WPARAM key = 0;
    Modifiers modifiers;
    bool repeated = false;
    // What the key types on the current layout; only for printable keys.
    std::optional<std::u16string> text;
};

// What holds when the key arrives.
struct KeyContext {
    InputMode mode = InputMode::English;
    // A composition is open in the key's document.
    bool composing = false;
    // The syllable at the caret is complete.
    bool reading_complete = false;
    bool candidates_open = false;
    bool candidates_take_key = false;
    bool sealed_text_at_caret = false;
    bool full_width = false;
    bool width_toggle_enabled = false;
    // The previous key was the backtick prefix.
    bool backtick_prefix = false;
};

namespace action {
struct PassThrough {};
struct ShiftDown {};
struct ToggleWidth {};
struct ArmBacktick {};
// Straight into the document.
struct Insert {
    std::u16string text;
};
// Into the composition, starting one if needed.
struct Compose {
    std::u16string text;
};
// Temporary English while composing: drops the unfinished reading first.
struct ComposeEnglish {
    std::u16string text;
};
struct Reading {
    char16_t symbol = 0;
};
// The open candidate list handles it.
struct Candidates {};
struct OpenCandidates {};
struct MoveCaret {
    bool forward = false;
};
struct Backspace {};
struct EraseSealed {};
struct Enter {};
struct Escape {};
}  // namespace action

using KeyAction = std::variant<action::PassThrough, action::ShiftDown, action::ToggleWidth, action::ArmBacktick,
                               action::Insert, action::Compose, action::ComposeEnglish, action::Reading,
                               action::Candidates, action::OpenCandidates, action::MoveCaret, action::Backspace,
                               action::EraseSealed, action::Enter, action::Escape>;

bool is_shift(WPARAM key) noexcept;
// Keys whose text the layout decides, given the modifiers held.
bool printable(WPARAM key, Modifiers modifiers) noexcept;
std::u16string to_full_width(std::u16string_view text);

// The single decision for a key; OnTestKeyDown and OnKeyDown both use it.
KeyAction route(const KeyPress& press, const KeyContext& context);

inline bool eaten(const KeyAction& action) noexcept {
    return !std::holds_alternative<action::PassThrough>(action);
}

// Key state that lasts across keys.
class KeyState {
public:
    // Seen while testing a key, which may never be handled.
    void observe(const KeyPress& press) noexcept;
    void handled(const KeyPress& press, const KeyAction& action) noexcept;
    void mode_changed(InputMode mode) noexcept;

    bool backtick_prefix() const noexcept { return backtick_prefix_; }
    // Shift went down with no other key since: its release switches the mode.
    bool shift_tap_pending() const noexcept { return shift_alone_; }
    // On Shift up: whether it was a tap.
    bool shift_released() noexcept { return std::exchange(shift_alone_, false); }

private:
    bool shift_alone_ = false;
    bool backtick_prefix_ = false;
};

}  // namespace tsf::keys
