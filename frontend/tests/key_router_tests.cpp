// The key decision table: one decision per key, shared by OnTestKeyDown and
// OnKeyDown.

#include "tsf/key_router.hpp"

#include <cstdio>
#include <stdexcept>
#include <string>
#include <variant>

namespace {

using namespace tsf::keys;
using tsf::InputMode;

struct Case {
    const char* name;
    KeyPress press;
    KeyContext context;
    KeyAction expected;
};

KeyPress key(WPARAM vk, std::optional<std::u16string> text = std::nullopt, Modifiers modifiers = {}) {
    return {.key = vk, .modifiers = modifiers, .text = std::move(text)};
}

constexpr Modifiers shift{.shift = true};
constexpr Modifiers ctrl{.ctrl = true};
constexpr Modifiers ctrl_shift{.shift = true, .ctrl = true};

const KeyContext chinese{.mode = InputMode::Chinese};
const KeyContext composing{.mode = InputMode::Chinese, .composing = true};
const KeyContext english{.mode = InputMode::English};

KeyContext with(KeyContext context, auto change) {
    change(context);
    return context;
}

std::u16string text_of(const KeyAction& action) {
    return std::visit(
        [](const auto& value) -> std::u16string {
            if constexpr (requires { value.text; }) {
                return value.text;
            } else if constexpr (requires { value.symbol; }) {
                return std::u16string(1, value.symbol);
            } else if constexpr (requires { value.forward; }) {
                return value.forward ? u"forward" : u"back";
            } else {
                return {};
            }
        },
        action);
}

void check(const Case& test) {
    const auto actual = route(test.press, test.context);
    if (actual.index() != test.expected.index() || text_of(actual) != text_of(test.expected)) {
        throw std::runtime_error(std::string("key decision: ") + test.name + " (got action " +
                                 std::to_string(actual.index()) + ")");
    }
}

void decisions() {
    using namespace action;
    const Case cases[] = {
        {"reading key starts a composition", key('A', u"a"), chinese, Reading{u'ㄇ'}},
        {"space alone goes to the document", key(VK_SPACE, u" "), chinese, PassThrough{}},
        {"space after a complete syllable is a space", key(VK_SPACE, u" "),
         with(composing, [](auto& c) { c.reading_complete = true; }), Compose{u" "}},
        {"space after an incomplete syllable is the first tone", key(VK_SPACE, u" "), composing, Reading{u' '}},
        {"symbol while composing joins it", key(VK_OEM_PLUS, u"="), composing, Compose{u"="}},
        {"symbol outside a composition goes to the document", key(VK_OEM_PLUS, u"="), chinese, PassThrough{}},
        {"full width symbol", key(VK_OEM_PLUS, u"="), with(chinese, [](auto& c) { c.full_width = true; }),
         Insert{u"＝"}},
        {"full width space", key(VK_SPACE, u" "), with(chinese, [](auto& c) { c.full_width = true; }),
         Insert{u"　"}},
        {"temporary English while composing", key('1', u"!", shift), composing, ComposeEnglish{u"!"}},
        {"temporary English outside a composition", key('1', u"!", shift), chinese, Insert{u"!"}},
        {"English letter", key('A', u"a"), english, Insert{u"a"}},
        {"English letter into a composition left open", key('A', u"a"),
         with(english, [](auto& c) { c.composing = true; }), Compose{u"a"}},
        {"English Enter", key(VK_RETURN), english, PassThrough{}},
        {"Ctrl punctuation", key(VK_OEM_COMMA, std::nullopt, ctrl), chinese, Compose{u"，"}},
        {"Ctrl+Shift+/ question mark", key(VK_OEM_2, std::nullopt, ctrl_shift), chinese, Compose{u"？"}},
        {"Ctrl+/ stays with the application", key(VK_OEM_2, std::nullopt, ctrl), chinese, PassThrough{}},
        {"Ctrl+C stays with the application", key('C', std::nullopt, ctrl), composing, PassThrough{}},
        {"backtick arms the prefix", key(VK_OEM_3, u"`"), chinese, ArmBacktick{}},
        {"backtick twice", key(VK_OEM_3, u"`"), with(chinese, [](auto& c) { c.backtick_prefix = true; }),
         Compose{u"‵"}},
        {"backtick punctuation", key(VK_OEM_COMMA, u","),
         with(chinese, [](auto& c) { c.backtick_prefix = true; }), Compose{u"，"}},
        {"backtick then a reading key", key('A', u"a"), with(chinese, [](auto& c) { c.backtick_prefix = true; }),
         Reading{u'ㄇ'}},
        {"English backtick", key(VK_OEM_3, u"`"), english, Insert{u"`"}},
        {"Shift", key(VK_SHIFT), chinese, ShiftDown{}},
        {"Shift+Space toggles width", key(VK_SPACE, u" ", shift),
         with(chinese, [](auto& c) { c.width_toggle_enabled = true; }), ToggleWidth{}},
        {"Shift+Space without the toggle", key(VK_SPACE, u" ", shift), chinese, Insert{u" "}},
        {"Backspace right after sealed text", key(VK_BACK),
         with(chinese, [](auto& c) { c.sealed_text_at_caret = true; }), EraseSealed{}},
        {"Backspace after sealed text in English", key(VK_BACK),
         with(english, [](auto& c) { c.sealed_text_at_caret = true; }), EraseSealed{}},
        {"Backspace with nothing sealed", key(VK_BACK), chinese, PassThrough{}},
        {"Ctrl+Backspace after sealed text", key(VK_BACK, std::nullopt, ctrl),
         with(chinese, [](auto& c) { c.sealed_text_at_caret = true; }), PassThrough{}},
        {"Backspace while composing", key(VK_BACK), composing, Backspace{}},
        {"Shift+Backspace while composing", key(VK_BACK, std::nullopt, shift), composing, Backspace{}},
        {"Ctrl+Backspace while composing", key(VK_BACK, std::nullopt, ctrl), composing, PassThrough{}},
        {"Enter while composing", key(VK_RETURN), composing, Enter{}},
        {"Shift+Enter while composing", key(VK_RETURN, std::nullopt, shift), composing, Enter{}},
        {"Enter outside a composition", key(VK_RETURN), chinese, PassThrough{}},
        {"Escape while composing", key(VK_ESCAPE), composing, action::Escape{}},
        {"Escape outside a composition", key(VK_ESCAPE), chinese, PassThrough{}},
        {"Left", key(VK_LEFT), composing, MoveCaret{false}},
        {"Right", key(VK_RIGHT), composing, MoveCaret{true}},
        {"Down opens candidates", key(VK_DOWN), composing, OpenCandidates{}},
        {"candidate key", key('1', u"1"),
         with(composing, [](auto& c) { c.candidates_open = c.candidates_take_key = true; }), Candidates{}},
        {"Backspace with candidates open", key(VK_BACK),
         with(composing, [](auto& c) { c.candidates_open = true; }), Backspace{}},
        {"other key with candidates open", key(VK_HOME),
         with(composing, [](auto& c) { c.candidates_open = true; }), PassThrough{}},
    };
    for (const auto& test : cases) check(test);
}

void require(bool condition, const char* message) {
    if (!condition) throw std::runtime_error(message);
}

void key_state() {
    {
        KeyState state;
        state.handled(key(VK_SHIFT), action::ShiftDown{});
        require(state.shift_tap_pending(), "Shift alone is a tap");
        require(state.shift_released(), "releasing a Shift tap switches the mode");
        require(!state.shift_tap_pending(), "a tap ends with its release");
    }
    {
        KeyState state;
        state.handled(key(VK_SHIFT), action::ShiftDown{});
        state.observe(key('A', u"A", shift));
        require(!state.shift_released(), "Shift used with another key is no tap");
    }
    {
        KeyState state;
        state.handled(key(VK_OEM_3, u"`"), action::ArmBacktick{});
        require(state.backtick_prefix(), "backtick arms the prefix");
        state.handled(key(VK_CONTROL, std::nullopt, ctrl), action::PassThrough{});
        require(state.backtick_prefix(), "a modifier alone keeps the prefix");
        state.handled(key('A', u"a"), action::Reading{u'ㄇ'});
        require(!state.backtick_prefix(), "the next key uses up the prefix");
    }
    {
        KeyState state;
        state.handled(key(VK_OEM_3, u"`"), action::ArmBacktick{});
        state.mode_changed(InputMode::English);
        require(!state.backtick_prefix(), "English mode drops the prefix");
    }
}

}  // namespace

int main() {
    try {
        decisions();
        key_state();
        std::printf("key router: ok\n");
        return 0;
    } catch (const std::exception& error) {
        std::fprintf(stderr, "%s\n", error.what());
        return 1;
    }
}
