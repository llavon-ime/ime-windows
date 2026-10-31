#include "tsf/key_router.hpp"

#include "core/bopomofo.hpp"

namespace tsf::keys {

namespace {

bool is_modifier(WPARAM key) noexcept {
    return is_shift(key) || key == VK_CONTROL || key == VK_LCONTROL || key == VK_RCONTROL || key == VK_MENU ||
           key == VK_LMENU || key == VK_RMENU;
}

// The common Traditional Chinese layout (Microsoft Bopomofo, Chewing): Ctrl
// selects punctuation on keys otherwise taken by Bopomofo symbols. Ctrl+/
// stays with applications (toggling comments); Ctrl+Shift+/ is the
// full-width question mark.
std::optional<std::u16string> ctrl_punctuation(WPARAM key, Modifiers modifiers) {
    if (!modifiers.ctrl || modifiers.alt || is_modifier(key)) return std::nullopt;
    if (modifiers.shift) {
        switch (key) {
            case VK_OEM_COMMA: return u"《";
            case VK_OEM_PERIOD: return u"》";
            case VK_OEM_1: return u"：";
            case VK_OEM_7: return u"＂";
            case VK_OEM_4: return u"『";
            case VK_OEM_6: return u"』";
            case '1': return u"！";
            case '9': return u"（";
            case '0': return u"）";
            case VK_OEM_2: return u"？";
            default: return std::nullopt;
        }
    }
    switch (key) {
        case VK_OEM_COMMA: return u"，";
        case VK_OEM_PERIOD: return u"。";
        case VK_OEM_1: return u"；";
        case VK_OEM_7: return u"、";
        case VK_OEM_MINUS: return u"—";
        case VK_OEM_4: return u"「";
        case VK_OEM_6: return u"」";
        default: return std::nullopt;
    }
}

std::optional<std::u16string> backtick_punctuation(WPARAM key, Modifiers modifiers) {
    if (modifiers.alt || is_modifier(key)) return std::nullopt;
    switch (key) {
        case VK_OEM_COMMA: return u"，";
        case VK_OEM_PERIOD: return u"。";
        case VK_OEM_1: return u"；";
        case VK_OEM_2: return u"？";
        case VK_OEM_3: return u"‵";
        case VK_OEM_7: return u"、";
        case VK_OEM_MINUS: return u"—";
        case VK_OEM_4: return u"「";
        case VK_OEM_6: return u"」";
        default: return std::nullopt;
    }
}

bool plain(Modifiers modifiers) noexcept {
    return !modifiers.shift && !modifiers.ctrl && !modifiers.alt;
}

KeyAction composing_key(const KeyPress& press, const KeyContext& context) {
    switch (press.key) {
        case VK_RETURN: return action::Enter{};
        case VK_ESCAPE: return action::Escape{};
        case VK_BACK: return action::Backspace{};
        case VK_DOWN: return action::OpenCandidates{};
        case VK_LEFT: return action::MoveCaret{false};
        case VK_RIGHT: return action::MoveCaret{true};
        case VK_SPACE:
            if (context.reading_complete) return action::Compose{context.full_width ? u"　" : u" "};
            break;
        default: break;
    }
    return action::PassThrough{};
}

}  // namespace

bool is_shift(WPARAM key) noexcept {
    return key == VK_SHIFT || key == VK_LSHIFT || key == VK_RSHIFT;
}

bool printable(WPARAM key, Modifiers modifiers) noexcept {
    if (is_modifier(key) || modifiers.ctrl || modifiers.alt) return false;
    if (key == VK_SPACE || (key >= '0' && key <= '9') || (key >= 'A' && key <= 'Z') ||
        (key >= VK_NUMPAD0 && key <= VK_NUMPAD9)) {
        return true;
    }
    switch (key) {
        case VK_OEM_1: case VK_OEM_PLUS: case VK_OEM_COMMA: case VK_OEM_MINUS: case VK_OEM_PERIOD:
        case VK_OEM_2: case VK_OEM_3: case VK_OEM_4: case VK_OEM_5: case VK_OEM_6: case VK_OEM_7:
        case VK_OEM_8: case VK_OEM_102: case VK_MULTIPLY: case VK_ADD: case VK_SEPARATOR:
        case VK_SUBTRACT: case VK_DECIMAL: case VK_DIVIDE:
            return true;
        default:
            return false;
    }
}

std::u16string to_full_width(std::u16string_view text) {
    std::u16string result;
    result.reserve(text.size());
    for (const char16_t character : text) {
        if (character == u' ') {
            result.push_back(u'　');
        } else if (character >= u'!' && character <= u'~') {
            result.push_back(static_cast<char16_t>(character + 0xfee0));
        } else {
            result.push_back(character);
        }
    }
    return result;
}

KeyAction route(const KeyPress& press, const KeyContext& context) {
    const auto key = press.key;
    const auto& modifiers = press.modifiers;
    const auto width = [&](const std::u16string& text) { return context.full_width ? to_full_width(text) : text; };

    if (is_shift(key)) return action::ShiftDown{};
    if (context.width_toggle_enabled && key == VK_SPACE && modifiers.shift && !modifiers.ctrl && !modifiers.alt) {
        return action::ToggleWidth{};
    }

    // Holding Shift in Chinese mode is temporary English input.
    const bool english = context.mode == InputMode::English || modifiers.shift;
    if (!english && key == VK_OEM_3 && plain(modifiers)) {
        return context.backtick_prefix ? KeyAction{action::Compose{u"‵"}} : KeyAction{action::ArmBacktick{}};
    }
    if (key == VK_BACK && !modifiers.ctrl && !modifiers.alt && !context.composing && context.sealed_text_at_caret) {
        return action::EraseSealed{};
    }
    if (auto text = ctrl_punctuation(key, modifiers)) return action::Compose{std::move(*text)};
    if (!english && context.backtick_prefix) {
        if (auto text = backtick_punctuation(key, modifiers)) return action::Compose{std::move(*text)};
    }

    if (english && press.text) {
        if (!context.composing) return action::Insert{width(*press.text)};
        if (context.mode == InputMode::Chinese) return action::ComposeEnglish{width(*press.text)};
        return action::Compose{width(*press.text)};
    }
    if (english && !context.composing) return action::PassThrough{};

    const auto reading = Bopomofo::lookup(static_cast<int>(key));
    if (press.text && !reading) {
        if (context.composing) return action::Compose{width(*press.text)};
        if (context.full_width) return action::Insert{to_full_width(*press.text)};
    }
    if (context.full_width && !context.composing && key == VK_SPACE && plain(modifiers)) {
        return action::Insert{u"　"};
    }

    const bool editing = (key == VK_BACK || key == VK_RETURN) && !modifiers.ctrl && !modifiers.alt;
    const bool modified = is_modifier(key) || modifiers.ctrl || modifiers.alt || modifiers.shift;
    if (modified && !(context.composing && editing)) return action::PassThrough{};

    if (context.composing && context.candidates_open) {
        if (context.candidates_take_key) return action::Candidates{};
        return key == VK_BACK ? KeyAction{action::Backspace{}} : KeyAction{action::PassThrough{}};
    }
    if (context.composing) {
        if (auto handled = composing_key(press, context); eaten(handled)) return handled;
    }
    if (reading && (key != VK_SPACE || context.composing)) return action::Reading{*reading};
    return action::PassThrough{};
}

void KeyState::observe(const KeyPress& press) noexcept {
    if (press.modifiers.shift && !is_shift(press.key)) shift_alone_ = false;
}

void KeyState::handled(const KeyPress& press, const KeyAction& action) noexcept {
    observe(press);
    if (std::holds_alternative<action::ShiftDown>(action)) {
        if (!press.repeated) shift_alone_ = true;
        return;
    }
    if (is_modifier(press.key)) return;
    backtick_prefix_ = std::holds_alternative<action::ArmBacktick>(action);
}

void KeyState::mode_changed(InputMode mode) noexcept {
    if (mode == InputMode::English) backtick_prefix_ = false;
}

}  // namespace tsf::keys
