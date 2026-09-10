#include "textService.h"

#include <algorithm>
#include <array>
#include <chrono>
#include <cstdint>
#include <cwctype>
#include <format>
#include <optional>

#include "candidateUiController.hpp"
#include "core/bopomofo.hpp"
#include "editSession.hpp"
#include "engine/engine.hpp"
#include "inputModeLangBarItem.hpp"
#include "system/globals.h"
#include "utils/healper.hpp"

using namespace std::literals;

namespace {

inline constexpr winrt::guid kCompositionDisplayAttributeGuid = {
    0x82769d2d, 0x9e5d, 0x4ace, {0x97, 0x53, 0x91, 0x3c, 0x0c, 0x7c, 0x4e, 0x38}};

TF_DISPLAYATTRIBUTE make_composition_display_attribute() {
    TF_DISPLAYATTRIBUTE attribute = {};
    attribute.crText.type = TF_CT_NONE;
    attribute.crBk.type = TF_CT_NONE;
    attribute.lsStyle = TF_LS_DOT;
    attribute.fBoldLine = FALSE;
    attribute.crLine.type = TF_CT_NONE;
    attribute.bAttr = TF_ATTR_TARGET_NOTCONVERTED;
    return attribute;
}

class CompositionDisplayAttributeInfo
    : public winrt::implements<CompositionDisplayAttributeInfo, ITfDisplayAttributeInfo> {
public:
    CompositionDisplayAttributeInfo() : attribute_(make_composition_display_attribute()), default_(attribute_) {}

    STDMETHODIMP GetGUID(GUID* pguid) override {
        if (!pguid) {
            return E_INVALIDARG;
        }
        *pguid = kCompositionDisplayAttributeGuid;
        return S_OK;
    }

    STDMETHODIMP GetDescription(BSTR* pbstrDesc) override {
        if (!pbstrDesc) {
            return E_INVALIDARG;
        }
        *pbstrDesc = SysAllocString(L"拉風輸入法組字");
        return (*pbstrDesc != nullptr) ? S_OK : E_OUTOFMEMORY;
    }

    STDMETHODIMP GetAttributeInfo(TF_DISPLAYATTRIBUTE* pda) override {
        if (!pda) {
            return E_INVALIDARG;
        }
        *pda = attribute_;
        return S_OK;
    }

    STDMETHODIMP SetAttributeInfo(const TF_DISPLAYATTRIBUTE* pda) override {
        if (!pda) {
            return E_INVALIDARG;
        }
        attribute_ = *pda;
        return S_OK;
    }

    STDMETHODIMP Reset() override {
        attribute_ = default_;
        return S_OK;
    }

private:
    TF_DISPLAYATTRIBUTE attribute_ = {};
    TF_DISPLAYATTRIBUTE default_ = {};
};

class DisplayAttributeEnum : public winrt::implements<DisplayAttributeEnum, IEnumTfDisplayAttributeInfo> {
public:
    explicit DisplayAttributeEnum(winrt::com_ptr<ITfDisplayAttributeInfo> info, ULONG index = 0)
        : info_(std::move(info)), index_(index) {}

    STDMETHODIMP Clone(IEnumTfDisplayAttributeInfo** ppEnum) override {
        if (!ppEnum) {
            return E_INVALIDARG;
        }
        *ppEnum = nullptr;
        auto enumerator = winrt::make_self<DisplayAttributeEnum>(info_, index_);
        enumerator.as<IEnumTfDisplayAttributeInfo>().copy_to(ppEnum);
        return S_OK;
    }

    STDMETHODIMP Next(ULONG ulCount, ITfDisplayAttributeInfo** rgInfo, ULONG* pcFetched) override {
        if (!rgInfo) {
            return E_INVALIDARG;
        }
        if (ulCount != 1 && !pcFetched) {
            return E_INVALIDARG;
        }

        ULONG fetched = 0;
        while (fetched < ulCount && index_ == 0 && info_) {
            rgInfo[fetched] = info_.get();
            rgInfo[fetched]->AddRef();
            ++fetched;
            ++index_;
        }

        if (pcFetched) {
            *pcFetched = fetched;
        }

        return (fetched == ulCount) ? S_OK : S_FALSE;
    }

    STDMETHODIMP Reset() override {
        index_ = 0;
        return S_OK;
    }

    STDMETHODIMP Skip(ULONG ulCount) override {
        if (ulCount == 0) {
            return S_OK;
        }
        if (index_ == 0) {
            index_ = 1;
            return (ulCount == 1) ? S_OK : S_FALSE;
        }
        return S_FALSE;
    }

private:
    winrt::com_ptr<ITfDisplayAttributeInfo> info_;
    ULONG index_ = 0;
};

HRESULT get_composition_display_attribute_atom(TfGuidAtom* atom) {
    if (!atom) {
        return E_INVALIDARG;
    }

    static TfGuidAtom cached_atom = TF_INVALID_GUIDATOM;
    if (cached_atom != TF_INVALID_GUIDATOM) {
        *atom = cached_atom;
        return S_OK;
    }

    ITfCategoryMgr* raw_category_mgr = nullptr;
    const HRESULT hr = CoCreateInstance(CLSID_TF_CategoryMgr, nullptr, CLSCTX_INPROC_SERVER, IID_ITfCategoryMgr,
                                        reinterpret_cast<void**>(&raw_category_mgr));
    if (FAILED(hr)) {
        return hr;
    }

    winrt::com_ptr<ITfCategoryMgr> category_mgr;
    category_mgr.attach(raw_category_mgr);

    const HRESULT register_hr = category_mgr->RegisterGUID(kCompositionDisplayAttributeGuid, &cached_atom);
    if (FAILED(register_hr)) {
        return register_hr;
    }

    *atom = cached_atom;
    return S_OK;
}

HRESULT apply_composition_display_attribute(ITfContext* context, TfEditCookie ec, ITfRange* range) {
    if (!context || !range) {
        return E_INVALIDARG;
    }

    TfGuidAtom atom = TF_INVALID_GUIDATOM;
    HRESULT hr = get_composition_display_attribute_atom(&atom);
    if (FAILED(hr)) {
        return hr;
    }

    winrt::com_ptr<ITfProperty> attribute_property;
    hr = context->GetProperty(GUID_PROP_ATTRIBUTE, attribute_property.put());
    if (FAILED(hr)) {
        return hr;
    }

    VARIANT value;
    VariantInit(&value);
    value.vt = VT_I4;
    value.lVal = atom;
    return attribute_property->SetValue(ec, range, &value);
}

bool key_down(int virtual_key) {
    return (GetKeyState(virtual_key) & 0x8000) != 0 || (GetAsyncKeyState(virtual_key) & 0x8000) != 0;
}

bool shift_key(WPARAM wParam) {
    return wParam == VK_SHIFT || wParam == VK_LSHIFT || wParam == VK_RSHIFT;
}

bool modifier_key(WPARAM wParam) {
    return shift_key(wParam) || wParam == VK_CONTROL || wParam == VK_LCONTROL || wParam == VK_RCONTROL ||
           wParam == VK_MENU || wParam == VK_LMENU || wParam == VK_RMENU;
}

bool backtick_shortcut_key(WPARAM wParam) {
    return wParam == VK_OEM_3 && !key_down(VK_SHIFT) && !key_down(VK_CONTROL) && !key_down(VK_MENU);
}

bool modified_passthrough_key(WPARAM wParam) {
    if (modifier_key(wParam)) {
        return true;
    }

    return key_down(VK_CONTROL) || key_down(VK_MENU) || key_down(VK_SHIFT);
}

bool same_com_object(IUnknown* left, IUnknown* right) {
    if (!left || !right) {
        return false;
    }

    winrt::com_ptr<IUnknown> left_identity;
    winrt::com_ptr<IUnknown> right_identity;
    if (FAILED(left->QueryInterface(IID_PPV_ARGS(left_identity.put()))) ||
        FAILED(right->QueryInterface(IID_PPV_ARGS(right_identity.put())))) {
        return false;
    }
    return left_identity.get() == right_identity.get();
}

bool context_compartment_flag(ITfContext* context, REFGUID guid) {
    if (!context) {
        return true;
    }

    winrt::com_ptr<ITfCompartmentMgr> compartment_mgr;
    if (FAILED(context->QueryInterface(IID_PPV_ARGS(compartment_mgr.put())))) {
        return false;
    }

    winrt::com_ptr<ITfCompartment> compartment;
    if (FAILED(compartment_mgr->GetCompartment(guid, compartment.put()))) {
        return false;
    }

    VARIANT value;
    VariantInit(&value);
    const HRESULT hr = compartment->GetValue(&value);
    const bool enabled =
        SUCCEEDED(hr) &&
        ((value.vt == VT_I4 && value.lVal != 0) || (value.vt == VT_UI4 && value.ulVal != 0) ||
         (value.vt == VT_BOOL && value.boolVal != VARIANT_FALSE));
    VariantClear(&value);
    return enabled;
}

bool english_printable_key(WPARAM wParam) {
    if (modifier_key(wParam) || key_down(VK_CONTROL) || key_down(VK_MENU)) {
        return false;
    }

    if (wParam == VK_SPACE || (wParam >= '0' && wParam <= '9') || (wParam >= 'A' && wParam <= 'Z') ||
        (wParam >= VK_NUMPAD0 && wParam <= VK_NUMPAD9)) {
        return true;
    }

    switch (wParam) {
        case VK_OEM_1:
        case VK_OEM_PLUS:
        case VK_OEM_COMMA:
        case VK_OEM_MINUS:
        case VK_OEM_PERIOD:
        case VK_OEM_2:
        case VK_OEM_3:
        case VK_OEM_4:
        case VK_OEM_5:
        case VK_OEM_6:
        case VK_OEM_7:
        case VK_OEM_8:
        case VK_OEM_102:
        case VK_MULTIPLY:
        case VK_ADD:
        case VK_SEPARATOR:
        case VK_SUBTRACT:
        case VK_DECIMAL:
        case VK_DIVIDE:
            return true;
        default:
            return false;
    }
}

std::optional<std::u16string> printable_key_text(WPARAM wParam, LPARAM lParam) {
    if (!english_printable_key(wParam)) {
        return std::nullopt;
    }

    BYTE keyboard_state[256] = {};
    if (!GetKeyboardState(keyboard_state)) {
        return std::nullopt;
    }

    std::array<WCHAR, 8> chars = {};
    UINT scan_code = static_cast<UINT>((lParam >> 16) & 0xff);
    if (scan_code == 0) {
        scan_code = MapVirtualKeyW(static_cast<UINT>(wParam), MAPVK_VK_TO_VSC);
    }

    constexpr UINT no_keyboard_state_change = 0x4;
    const int count = ToUnicode(static_cast<UINT>(wParam), scan_code, keyboard_state, chars.data(),
                                static_cast<int>(chars.size()), no_keyboard_state_change);
    if (count <= 0) {
        return std::nullopt;
    }

    std::u16string text;
    const int safe_count = std::min(count, static_cast<int>(chars.size()));
    for (int i = 0; i < safe_count; i++) {
        const WCHAR ch = chars[i];
        if (std::iswcntrl(ch)) {
            return std::nullopt;
        }
        text.push_back(static_cast<char16_t>(ch));
    }
    return text.empty() ? std::nullopt : std::optional{text};
}

std::optional<std::u16string> shifted_printable_symbol_text(WPARAM wParam, LPARAM lParam) {
    if (!key_down(VK_SHIFT) || key_down(VK_CONTROL) || key_down(VK_MENU) || modifier_key(wParam)) {
        return std::nullopt;
    }

    if ((wParam >= 'A' && wParam <= 'Z') || wParam == VK_SPACE || (wParam >= VK_NUMPAD0 && wParam <= VK_DIVIDE)) {
        return std::nullopt;
    }

    return printable_key_text(wParam, lParam);
}

std::optional<std::u16string> punctuation_shortcut(WPARAM wParam) {
    if (!key_down(VK_CONTROL) || key_down(VK_MENU) || modifier_key(wParam)) {
        return std::nullopt;
    }

    // Follow the common Traditional Chinese IME punctuation layout used by
    // Microsoft Bopomofo/Chewing-like tables: Ctrl selects punctuation for
    // keys that are otherwise occupied by Bopomofo symbols.
    // Leave Ctrl+/ available to applications (for example, toggling
    // comments), while Ctrl+Shift+/ keeps the Microsoft Bopomofo Ctrl+?
    // shortcut for a full-width question mark.
    if (key_down(VK_SHIFT)) {
        switch (wParam) {
            case VK_OEM_COMMA:
                return u"《";
            case VK_OEM_PERIOD:
                return u"》";
            case VK_OEM_1:
                return u"：";
            case VK_OEM_7:
                return u"＂";
            case VK_OEM_4:
                return u"『";
            case VK_OEM_6:
                return u"』";
            case '1':
                return u"！";
            case '9':
                return u"（";
            case '0':
                return u"）";
            case VK_OEM_2:
                return u"？";
            default:
                return std::nullopt;
        }
    }

    switch (wParam) {
        case VK_OEM_COMMA:
            return u"，";
        case VK_OEM_PERIOD:
            return u"。";
        case VK_OEM_1:
            return u"；";
        case VK_OEM_7:
            return u"、";
        case VK_OEM_MINUS:
            return u"—";
        case VK_OEM_4:
            return u"「";
        case VK_OEM_6:
            return u"」";
        default:
            return std::nullopt;
    }
}

}  // namespace

namespace tsf {

TextService::TextService() : candidate_ui_(std::make_unique<CandidateUiController>()) {}

TextService::~TextService() {
    unadvise_text_edit_sink();
}

HRESULT TextService::handle_com_exception(std::source_location location) noexcept {
    return tsf::handle_com_exception(logger_, location);
}

//FIXME: shouldn't be in tsf?
std::optional<std::u16string> TextService::multifuntional_shortcut(WPARAM wParam) {
    if (!backtick_used_as_modifier_ || key_down(VK_MENU) || modifier_key(wParam)) {
        return std::nullopt;
    }
    backtick_used_as_modifier_ = false;
    //TODO: a UI and more keys(and user-defined?)
    switch (wParam) {
        case VK_OEM_COMMA:
            return u"，";
        case VK_OEM_PERIOD:
            return u"。";
        case VK_OEM_1:
            return u"；";
        case VK_OEM_2:
            return u"？";
        case VK_OEM_3:
            return u"‵";
        case VK_OEM_7:
            return u"、";
        case VK_OEM_MINUS:
            return u"—";
        case VK_OEM_4:
            return u"「";
        case VK_OEM_6:
            return u"」";
        default:
            return std::nullopt;
    }
}

/**
 * @brief Implements ITfTextInputProcessor::Activate.
 *
 * Delegates activation to the shared setup path.
 */
STDMETHODIMP TextService::Activate(ITfThreadMgr* pThreadMgr, TfClientId tfClientId) try {
    return activate(pThreadMgr, tfClientId);
} catch (...) {
    return handle_com_exception();
}

/**
 * @brief Implements ITfTextInputProcessor::Deactivate.
 *
 * Tears down TSF state through the shared cleanup path.
 */
STDMETHODIMP TextService::Deactivate() try {
    deactivate();
    return S_OK;
} catch (...) {
    return handle_com_exception();
}

/**
 * @brief Implements ITfTextInputProcessorEx::ActivateEx.
 *
 * Uses the same activation flow and currently ignores extra flags.
 */
STDMETHODIMP TextService::ActivateEx(ITfThreadMgr* pThreadMgr, TfClientId tfClientId, DWORD /*dwFlags*/) try {
    return activate(pThreadMgr, tfClientId);
} catch (...) {
    return handle_com_exception();
}

/**
 * @brief Shared activation helper.
 *
 * Attaches TSF sinks, stores thread manager state, and starts debug logging.
 */
HRESULT TextService::activate(ITfThreadMgr* pThreadMgr, TfClientId tfClientId) {
    if (!pThreadMgr) return E_INVALIDARG;

    threadMgr.copy_from(pThreadMgr);
    _tfClientId = tfClientId;
    candidate_ui_->attach(pThreadMgr, tfClientId);

    input_mode_lang_bar_item_ = winrt::make_self<InputModeLangBarItem>([this]() {
        get_engine()->toggle_input_mode();
        refresh_input_mode_indicator();
    });
    input_mode_lang_bar_item_->add_to_language_bar(threadMgr.get());
    refresh_input_mode_indicator();

    winrt::com_ptr<ITfSource> itfSource;
    HRESULT hr = threadMgr->QueryInterface<ITfSource>(itfSource.put());
    if (FAILED(hr)) {
        deactivate();
        return hr;
    }

    hr = itfSource->AdviseSink(
        IID_ITfThreadMgrEventSink, static_cast<ITfThreadMgrEventSink*>(this), &dwThreadMgrEventSinkCookie);
    if (FAILED(hr)) {
        deactivate();
        return hr;
    }

    winrt::com_ptr<ITfKeystrokeMgr> itfKeystrokeMgr;
    hr = threadMgr->QueryInterface<ITfKeystrokeMgr>(itfKeystrokeMgr.put());
    if (FAILED(hr)) {
        deactivate();
        return hr;
    }

    hr = itfKeystrokeMgr->AdviseKeyEventSink(tfClientId, static_cast<ITfKeyEventSink*>(this), TRUE);
    if (FAILED(hr)) {
        deactivate();
        return hr;
    }

    return S_OK;
}

/**
 * @brief Shared deactivation helper.
 *
 * Releases TSF sinks, clears composition state, and stops debug logging.
 */
void TextService::deactivate() {
    unadvise_text_edit_sink();
    candidate_ui_->hide();

    if (itfComposition) {
        itfComposition->EndComposition(TF_INVALID_COOKIE);
        itfComposition = nullptr;
    }
    clear_composition_state();

    if (threadMgr) {
        if (input_mode_lang_bar_item_) {
            input_mode_lang_bar_item_->remove_from_language_bar(threadMgr.get());
            input_mode_lang_bar_item_ = nullptr;
        }

        winrt::com_ptr<ITfKeystrokeMgr> itfKeystrokeMgr;
        if (SUCCEEDED(threadMgr->QueryInterface<ITfKeystrokeMgr>(itfKeystrokeMgr.put()))) {
            itfKeystrokeMgr->UnadviseKeyEventSink(_tfClientId);
        }

        if (dwThreadMgrEventSinkCookie != TF_INVALID_COOKIE) {
            winrt::com_ptr<ITfSource> pSource;
            if (SUCCEEDED(threadMgr->QueryInterface(IID_PPV_ARGS(pSource.put())))) {
                pSource->UnadviseSink(dwThreadMgrEventSinkCookie);
            }
            dwThreadMgrEventSinkCookie = TF_INVALID_COOKIE;
        }

        threadMgr = nullptr;
    }
    _tfClientId = TF_CLIENTID_NULL;
    candidate_ui_->detach();
}

InputMode TextService::read_backend_input_mode() {
    return refresh_input_mode_indicator();
}

InputMode TextService::refresh_input_mode_indicator() {
    const InputMode mode = get_engine()->current_input_mode();
    if (input_mode_lang_bar_item_) {
        input_mode_lang_bar_item_->set_mode(mode);
    }
    sync_input_mode_compartments(mode);
    return mode;
}

void TextService::sync_input_mode_compartments(InputMode mode) {
    if (!threadMgr || _tfClientId == TF_CLIENTID_NULL) {
        return;
    }

    winrt::com_ptr<ITfCompartmentMgr> compartment_mgr;
    if (FAILED(threadMgr->QueryInterface(IID_PPV_ARGS(compartment_mgr.put())))) {
        return;
    }

    const auto set_compartment = [&](REFGUID guid, LONG value) {
        winrt::com_ptr<ITfCompartment> compartment;
        if (FAILED(compartment_mgr->GetCompartment(guid, compartment.put()))) {
            return;
        }

        VARIANT variant;
        VariantInit(&variant);
        variant.vt = VT_I4;
        variant.lVal = value;
        compartment->SetValue(_tfClientId, &variant);
    };

    set_compartment(GUID_COMPARTMENT_KEYBOARD_OPENCLOSE, mode == InputMode::Chinese ? TRUE : FALSE);
    set_compartment(GUID_COMPARTMENT_KEYBOARD_INPUTMODE_CONVERSION,
                    mode == InputMode::Chinese ? TF_CONVERSIONMODE_NATIVE : 0);
}

bool TextService::context_accepts_input(ITfContext* context) const {
    if (!context || context_compartment_flag(context, GUID_COMPARTMENT_EMPTYCONTEXT) ||
        context_compartment_flag(context, GUID_COMPARTMENT_KEYBOARD_DISABLED)) {
        return false;
    }

    TF_STATUS status = {};
    const HRESULT hr = context->GetStatus(&status);
    if (hr == TF_E_DISCONNECTED) {
        return false;
    }
    return FAILED(hr) || (status.dwDynamicFlags & TF_SD_READONLY) == 0;
}

bool TextService::composition_belongs_to(ITfContext* context) const {
    return itfComposition && composition_context_ && same_com_object(composition_context_.get(), context);
}

bool TextService::has_composition_state() const {
    return itfComposition || composition_context_ || !compositionBuffer.empty() || candidate_ui_->is_active();
}

void TextService::clear_composition_state() {
    candidate_ui_->hide();
    itfComposition = nullptr;
    composition_context_ = nullptr;
    compositionBuffer.clear();
}

/**
 * @brief Implements ITfThreadMgrEventSink::OnInitDocumentMgr.
 *
 * No document-manager initialization is required yet.
 */
STDMETHODIMP TextService::OnInitDocumentMgr(ITfDocumentMgr* /*pDocMgr*/) try { return S_OK; } catch (...) {
    return handle_com_exception();
}

/**
 * @brief Implements ITfThreadMgrEventSink::OnUninitDocumentMgr.
 *
 * No document-manager teardown is required yet.
 */
STDMETHODIMP TextService::OnUninitDocumentMgr(ITfDocumentMgr* /*pDocMgr*/) try { return S_OK; } catch (...) {
    return handle_com_exception();
}

/**
 * @brief Implements ITfThreadMgrEventSink::OnSetFocus.
 *
 * Receives document focus changes but does not react to them yet.
 */
STDMETHODIMP TextService::OnSetFocus(ITfDocumentMgr* /*pDocMgrFocus*/, ITfDocumentMgr* /*pDocMgrPrevFocus*/) try {
    refresh_input_mode_indicator();
    return S_OK;
} catch (...) {
    return handle_com_exception();
}

/**
 * @brief Implements ITfThreadMgrEventSink::OnPushContext.
 *
 * Accepts new contexts without additional bookkeeping.
 */
STDMETHODIMP TextService::OnPushContext(ITfContext* /*pContext*/) try { return S_OK; } catch (...) {
    return handle_com_exception();
}

/**
 * @brief Implements ITfThreadMgrEventSink::OnPopContext.
 *
 * Releases contexts without additional cleanup.
 */
STDMETHODIMP TextService::OnPopContext(ITfContext* /*pContext*/) try { return S_OK; } catch (...) {
    return handle_com_exception();
}

/**
 * @brief Implements ITfKeyEventSink::OnSetFocus.
 *
 * Tracks foreground changes but currently keeps no extra state.
 */
STDMETHODIMP TextService::OnSetFocus(BOOL fForeground) try {
    if (fForeground) {
        refresh_input_mode_indicator();
    }
    return S_OK;
} catch (...) {
    return handle_com_exception();
}

/**
 * @brief Implements ITfKeyEventSink::OnTestKeyDown.
 *
 * Reports whether the service intends to consume the key-down event.
 */
STDMETHODIMP TextService::OnTestKeyDown(ITfContext* pContext, WPARAM wParam, LPARAM lParam, BOOL* pfEaten) try {
    if (!pfEaten) return E_INVALIDARG;
    key_down_started_at_ = std::chrono::steady_clock::now();
    key_down_started_key_ = wParam;

    if (!context_accepts_input(pContext)) {
        *pfEaten = FALSE;
        return S_OK;
    }

    if (shift_key(wParam)) {
        *pfEaten = TRUE;
        return S_OK;
    }

    if (key_down(VK_SHIFT)) {
        shift_toggle_pending_ = false;
        shift_used_as_modifier_ = true;
    }

    if (backtick_shortcut_key(wParam)) {
        *pfEaten = TRUE;
        return S_OK;
    }

    const bool english_mode = read_backend_input_mode() == InputMode::English;
    const bool active_composition = composition_belongs_to(pContext) && !compositionBuffer.empty();
    const bool is_bopomofo_key = Bopomofo::lookup(static_cast<int>(wParam)) != std::nullopt;
    if (english_mode && !active_composition) {
        *pfEaten = (punctuation_shortcut(wParam) || multifuntional_shortcut(wParam) || english_printable_key(wParam))
                       ? TRUE
                       : FALSE;
        return S_OK;
    }

    if (punctuation_shortcut(wParam)) {
        *pfEaten = TRUE;
        return S_OK;
    }

    if (multifuntional_shortcut(wParam)) {
        *pfEaten = TRUE;
        return S_OK;
    }

    if (shifted_printable_symbol_text(wParam, lParam)) {
        *pfEaten = TRUE;
        return S_OK;
    }

    if (active_composition && !is_bopomofo_key && printable_key_text(wParam, lParam)) {
        *pfEaten = TRUE;
        return S_OK;
    }

    if (english_mode && english_printable_key(wParam)) {
        *pfEaten = TRUE;
        return S_OK;
    }

    if (modified_passthrough_key(wParam)) {
        *pfEaten = FALSE;
        return S_OK;
    }

    if (active_composition && candidate_ui_->is_active()) {
        const bool backspace_composition = (wParam == VK_BACK);
        *pfEaten = (candidate_ui_->can_handle_key(wParam) || backspace_composition) ? TRUE : FALSE;
        return S_OK;
    }

    if (active_composition) {
        const bool editing_key = (wParam == VK_RETURN || wParam == VK_ESCAPE || wParam == VK_BACK ||
                                  wParam == VK_LEFT || wParam == VK_RIGHT || wParam == VK_DOWN ||
                                  wParam == VK_SPACE);
        if (editing_key) {
            *pfEaten = TRUE;
            return S_OK;
        }
    }

    const bool starts_composition = is_bopomofo_key && wParam != VK_SPACE;
    *pfEaten = starts_composition ? TRUE : FALSE;
    return S_OK;
} catch (...) {
    return handle_com_exception();
}

/**
 * @brief Implements ITfKeyEventSink::OnTestKeyUp.
 *
 * Leaves key-up events unhandled by default.
 */
STDMETHODIMP TextService::OnTestKeyUp(ITfContext* /*pContext*/, WPARAM wParam, LPARAM /*lParam*/, BOOL* pfEaten) try {
    if (!pfEaten) return E_INVALIDARG;
    *pfEaten = shift_key(wParam) && shift_toggle_pending_ ? TRUE : FALSE;
    return S_OK;
} catch (...) {
    return handle_com_exception();
}

/**
 * @brief Implements ITfKeyEventSink::OnKeyDown.
 *
 * Updates the active composition or handles commit and cancel keys.
 */
STDMETHODIMP TextService::OnKeyDown(ITfContext* pContext, WPARAM wParam, LPARAM lParam, BOOL* pfEaten) try {
    if (!pfEaten) return E_INVALIDARG;
    *pfEaten = FALSE;
    const bool started_at_on_test = key_down_started_at_ && key_down_started_key_ == wParam;
    const auto e2e_start =
        started_at_on_test ? *key_down_started_at_ : std::chrono::steady_clock::now();
    key_down_started_at_.reset();
    E2eTrace e2e_trace{};
    e2e_trace.start = e2e_start;
    e2e_trace.on_key_started = std::chrono::steady_clock::now();
    e2e_trace.key = static_cast<std::uint64_t>(wParam);
    e2e_trace.sequence = next_e2e_sequence_++;
    e2e_trace.started_at_on_test = started_at_on_test;

    if (!context_accepts_input(pContext)) {
        return S_OK;
    }

    if (has_composition_state() && !composition_belongs_to(pContext)) {
        if (itfComposition && composition_context_) {
            const HRESULT hr = end_composition(composition_context_.get());
            if (FAILED(hr)) {
                clear_composition_state();
            }
        } else {
            clear_composition_state();
        }
    }

    if (shift_key(wParam)) {
        const bool repeated_keydown = (lParam & (1LL << 30)) != 0;
        if (!repeated_keydown) {
            shift_toggle_pending_ = true;
            shift_used_as_modifier_ = false;
        }
        *pfEaten = TRUE;
        return S_OK;
    }

    if (key_down(VK_SHIFT)) {
        shift_toggle_pending_ = false;
        shift_used_as_modifier_ = true;
    }
    if (backtick_shortcut_key(wParam) && !backtick_used_as_modifier_) {
        backtick_used_as_modifier_ = true;
        *pfEaten = TRUE;
        return S_OK;
    }

    e2e_trace.mode_started = std::chrono::steady_clock::now();
    const bool english_mode = read_backend_input_mode() == InputMode::English;
    e2e_trace.mode_finished = std::chrono::steady_clock::now();
    e2e_trace.ready_started = e2e_trace.mode_finished;
    e2e_trace.ready_finished = e2e_trace.mode_finished;
    auto punctuation = punctuation_shortcut(wParam);
    if (!punctuation) punctuation = multifuntional_shortcut(wParam);
    if (punctuation) {
        compositionBuffer.add_chosen_candidate((*punctuation)[0]);
        set_composition_text(pContext, compositionBuffer.to_string());
        *pfEaten = TRUE;
        return S_OK;
    }

    if (english_mode && compositionBuffer.empty()) {
        if (const auto text = printable_key_text(wParam, lParam)) {
            insert_text(pContext, *text);
            *pfEaten = TRUE;
            return S_OK;
        }

        return S_OK;
    }

    auto symbol = shifted_printable_symbol_text(wParam, lParam);
    if (!symbol && !compositionBuffer.empty() &&
        Bopomofo::lookup(static_cast<int>(wParam)) == std::nullopt) {
        symbol = printable_key_text(wParam, lParam);
    }
    if (symbol) {
        if (compositionBuffer.empty()) {
            insert_text(pContext, *symbol);
            *pfEaten = TRUE;
            return S_OK;
        }

        for (const char16_t ch : *symbol) {
            compositionBuffer.add_chosen_candidate(ch);
        }
        set_composition_text(pContext, compositionBuffer.to_string());
        *pfEaten = TRUE;
        return S_OK;
    }

    if (english_mode) {
        if (const auto text = printable_key_text(wParam, lParam)) {
            candidate_ui_->hide();
            for (const char16_t ch : *text) {
                compositionBuffer.add_chosen_candidate(ch);
            }
            set_composition_text(pContext, compositionBuffer.to_string());
            *pfEaten = TRUE;
            return S_OK;
        }
    }

    if (modified_passthrough_key(wParam)) {
        return S_OK;
    }

    if (candidate_ui_->is_active()) {
        const CandidateKeyResult result = candidate_ui_->handle_key(wParam);
        switch (result) {
            case CandidateKeyResult::navigated:
                *pfEaten = TRUE;
                return S_OK;
            case CandidateKeyResult::finalized:
                compositionBuffer.next();
                refresh_composition_after_candidate_finalize(pContext, e2e_trace);
                *pfEaten = TRUE;
                return S_OK;
            case CandidateKeyResult::aborted:
                *pfEaten = TRUE;
                return S_OK;
            case CandidateKeyResult::not_handled:
            default:
                break;
        }
    }

    if (wParam == VK_RETURN && !compositionBuffer.empty()) {
        end_composition(pContext);
        *pfEaten = TRUE;
        return S_OK;
    }

    if (wParam == VK_ESCAPE && itfComposition) {
        discard_composition(pContext);

        //used in multifunctional shortcut handling
        backtick_used_as_modifier_ = false;
        *pfEaten = TRUE;
        return S_OK;
    }

    if (wParam == VK_BACK && !compositionBuffer.empty()) {
        if (!compositionBuffer.remove_last()) {
            candidate_ui_->hide();
            *pfEaten = TRUE;
            return S_OK;
        }
        if (compositionBuffer.empty()) {
            discard_composition(pContext);
        } else {
            set_composition_text(pContext, compositionBuffer.to_string());
        }
        *pfEaten = TRUE;
        return S_OK;
    }

    if (wParam == VK_DOWN && !compositionBuffer.empty()) {
        show_candidate_list_for_current_input(pContext, false);
        *pfEaten = TRUE;
        return S_OK;
    }

    if (wParam == VK_LEFT && !compositionBuffer.empty()) {
        candidate_ui_->hide();
        compositionBuffer.pre();
        set_composition_text(pContext, compositionBuffer.to_string());
        *pfEaten = TRUE;
        return S_OK;
    }

    if (wParam == VK_RIGHT && !compositionBuffer.empty()) {
        candidate_ui_->hide();
        compositionBuffer.next();
        set_composition_text(pContext, compositionBuffer.to_string());
        *pfEaten = TRUE;
        return S_OK;
    }

    if (wParam == VK_SPACE && !compositionBuffer.empty() && compositionBuffer.current_compositable()) {
        end_composition(pContext);
        insert_text(pContext, u" ");
        *pfEaten = TRUE;
        return S_OK;
    }

    auto cur_char = Bopomofo::lookup(static_cast<int>(wParam));
    if (cur_char == std::nullopt || (wParam == VK_SPACE && compositionBuffer.empty())) {
        candidate_ui_->hide();
        // TODO: Handle non-Bopomofo keys.
        *pfEaten = FALSE;
        return S_OK;
    }

    if (wParam != VK_SPACE) {
        e2e_trace.ready_started = std::chrono::steady_clock::now();
        get_engine()->ready();
        e2e_trace.ready_finished = std::chrono::steady_clock::now();
    }

    // if (!itfComposition) {
    //     start_composition(pContext);
    // }
    compositionBuffer.add(cur_char.value());
    const auto invalid_span = compositionBuffer.current_invalid_span();
    bool prediction_performed = false;
    if (!invalid_span) {
        e2e_trace.pre_context_started = std::chrono::steady_clock::now();
        auto pre_context = get_pre_composit_context(pContext);
        e2e_trace.pre_context_finished = std::chrono::steady_clock::now();
        e2e_trace.predict_started = std::chrono::steady_clock::now();
        prediction_performed = compositionBuffer.predict_paddings(std::move(pre_context));
        e2e_trace.predict_finished = std::chrono::steady_clock::now();
    }
    if (invalid_span) {
        set_composition_text(pContext, compositionBuffer.to_string(), invalid_span->first, invalid_span->second);
    } else {
        set_composition_text(pContext, compositionBuffer.to_string(), std::u16string::npos, 0,
                             prediction_performed ? std::optional<E2eTrace>(e2e_trace)
                                                  : std::nullopt);
    }
    *pfEaten = TRUE;
    return S_OK;
} catch (...) {
    return handle_com_exception();
}

/**
 * @brief Implements ITfKeyEventSink::OnKeyUp.
 *
 * Leaves key-up events unconsumed after key-down handling.
 */
STDMETHODIMP TextService::OnKeyUp(ITfContext* /*pContext*/, WPARAM wParam, LPARAM /*lParam*/, BOOL* pfEaten) try {
    if (!pfEaten) return E_INVALIDARG;
    if (shift_key(wParam) && shift_toggle_pending_) {
        if (!shift_used_as_modifier_) {
            get_engine()->toggle_input_mode();
            refresh_input_mode_indicator();
        }

        shift_toggle_pending_ = false;
        shift_used_as_modifier_ = false;
        *pfEaten = TRUE;
        return S_OK;
    }

    *pfEaten = FALSE;
    return S_OK;
} catch (...) {
    return handle_com_exception();
}

/**
 * @brief Implements ITfKeyEventSink::OnPreservedKey.
 *
 * Declines preserved-key handling because no preserved keys are registered.
 */
STDMETHODIMP TextService::OnPreservedKey(ITfContext* /*pContext*/, REFGUID /*rguid*/, BOOL* pfEaten) try {
    if (!pfEaten) return E_INVALIDARG;
    *pfEaten = FALSE;
    return S_OK;
} catch (...) {
    return handle_com_exception();
}

/**
 * @brief Implements ITfCompositionSink::OnCompositionTerminated.
 *
 * Clears local composition state when TSF ends the composition externally.
 */
STDMETHODIMP TextService::OnCompositionTerminated(TfEditCookie /*ecWrite*/, ITfComposition* pComposition) try {
    if (itfComposition && pComposition && !same_com_object(itfComposition.get(), pComposition)) {
        return S_OK;
    }
    clear_composition_state();
    return S_OK;
} catch (...) {
    return handle_com_exception();
}

STDMETHODIMP TextService::OnEndEdit(ITfContext* context, TfEditCookie /*read_only_cookie*/,
                                    ITfEditRecord* /*edit_record*/) try {
    if (!pending_e2e_trace_ || !text_edit_sink_context_ ||
        !same_com_object(text_edit_sink_context_.get(), context)) {
        return S_OK;
    }

    const PendingE2eTrace pending = *pending_e2e_trace_;
    pending_e2e_trace_.reset();
    const auto ended = std::chrono::steady_clock::now();
    const auto& trace = pending.trace;
    const auto milliseconds = [](const auto started, const auto finished) {
        return std::chrono::duration<double, std::milli>(finished - started).count();
    };

    const double test_to_key_ms = milliseconds(trace.start, trace.on_key_started);
    const double key_pre_mode_ms = milliseconds(trace.on_key_started, trace.mode_started);
    const double mode_ms = milliseconds(trace.mode_started, trace.mode_finished);
    const double mode_to_ready_ms = milliseconds(trace.mode_finished, trace.ready_started);
    const double ready_ms = milliseconds(trace.ready_started, trace.ready_finished);
    const double ready_to_context_ms =
        milliseconds(trace.ready_finished, trace.pre_context_started);
    const double pre_context_ms =
        milliseconds(trace.pre_context_started, trace.pre_context_finished);
    const double context_to_predict_ms =
        milliseconds(trace.pre_context_finished, trace.predict_started);
    const double predict_rtt_ms = milliseconds(trace.predict_started, trace.predict_finished);
    const double post_predict_ms =
        milliseconds(trace.predict_finished, pending.edit_request_started);
    const double edit_wait_ms =
        milliseconds(pending.edit_request_started, pending.edit_operation_started);
    const double edit_apply_ms =
        milliseconds(pending.edit_operation_started, pending.edit_operation_finished);
    const double edit_prepare_ms =
        milliseconds(pending.edit_operation_started, pending.edit_set_text_started);
    const double edit_set_text_ms =
        milliseconds(pending.edit_set_text_started, pending.edit_set_text_finished);
    const double edit_attribute_ms =
        milliseconds(pending.edit_set_text_finished, pending.edit_attribute_finished);
    const double edit_selection_ms =
        milliseconds(pending.edit_attribute_finished, pending.edit_operation_finished);
    const double edit_notify_ms = milliseconds(pending.edit_operation_finished, ended);
    const double elapsed_ms = milliseconds(trace.start, ended);
    const double partition_ms =
        test_to_key_ms + key_pre_mode_ms + mode_ms + mode_to_ready_ms + ready_ms +
        ready_to_context_ms + pre_context_ms + context_to_predict_ms + predict_rtt_ms +
        post_predict_ms + edit_wait_ms + edit_apply_ms + edit_notify_ms;
    const double partition_error_ms = elapsed_ms - partition_ms;

    logger_.log([elapsed_ms, test_to_key_ms, key_pre_mode_ms, mode_ms, mode_to_ready_ms,
                 ready_ms, ready_to_context_ms, pre_context_ms, context_to_predict_ms,
                 predict_rtt_ms, post_predict_ms, edit_wait_ms, edit_apply_ms,
                 edit_prepare_ms, edit_set_text_ms, edit_attribute_ms, edit_selection_ms,
                 edit_notify_ms, partition_ms, partition_error_ms, key = trace.key,
                 sequence = trace.sequence, started_at_on_test = trace.started_at_on_test] {
        return std::format(
            "[TIME] frontend_e2e_ms={:.3f} sequence={} key={} start={} test_to_key_ms={:.3f} "
            "key_pre_mode_ms={:.3f} mode_ms={:.3f} mode_to_ready_ms={:.3f} "
            "ready_ms={:.3f} ready_to_context_ms={:.3f} pre_context_ms={:.3f} "
            "context_to_predict_ms={:.3f} predict_rtt_ms={:.3f} post_predict_ms={:.3f} "
            "edit_wait_ms={:.3f} edit_apply_ms={:.3f} edit_prepare_ms={:.3f} "
            "edit_set_text_ms={:.3f} edit_attribute_ms={:.3f} edit_selection_ms={:.3f} "
            "edit_notify_ms={:.3f} "
            "partition_ms={:.3f} partition_error_ms={:.3f} endpoint=on_end_edit",
            elapsed_ms, sequence, key,
            started_at_on_test ? "on_test_key_down" : "on_key_down", test_to_key_ms,
            key_pre_mode_ms, mode_ms,
            mode_to_ready_ms, ready_ms, ready_to_context_ms, pre_context_ms,
            context_to_predict_ms, predict_rtt_ms, post_predict_ms, edit_wait_ms,
            edit_apply_ms, edit_prepare_ms, edit_set_text_ms, edit_attribute_ms,
            edit_selection_ms, edit_notify_ms, partition_ms, partition_error_ms);
    });
    return S_OK;
} catch (...) {
    return handle_com_exception();
}

/**
 * @brief Implements ITfDisplayAttributeProvider::EnumDisplayAttributeInfo.
 *
 * Returns not implemented because display attributes are not exposed yet.
 */
STDMETHODIMP TextService::EnumDisplayAttributeInfo(IEnumTfDisplayAttributeInfo** ppEnum) try {
    if (!ppEnum) {
        return E_INVALIDARG;
    }

    *ppEnum = nullptr;
    auto info = winrt::make_self<CompositionDisplayAttributeInfo>();
    auto enumerator = winrt::make_self<DisplayAttributeEnum>(info.as<ITfDisplayAttributeInfo>());
    enumerator.as<IEnumTfDisplayAttributeInfo>().copy_to(ppEnum);
    return S_OK;
} catch (...) {
    return handle_com_exception();
}

/**
 * @brief Implements ITfDisplayAttributeProvider::GetDisplayAttributeInfo.
 *
 * Returns not implemented because no display attribute metadata is defined yet.
 */
STDMETHODIMP_(HRESULT __stdcall)
TextService::GetDisplayAttributeInfo(REFGUID guid, ITfDisplayAttributeInfo** ppInfo) try {
    if (!ppInfo) {
        return E_INVALIDARG;
    }

    *ppInfo = nullptr;
    if (!IsEqualGUID(guid, kCompositionDisplayAttributeGuid)) {
        return E_INVALIDARG;
    }

    auto info = winrt::make_self<CompositionDisplayAttributeInfo>();
    info.as<ITfDisplayAttributeInfo>().copy_to(ppInfo);
    return S_OK;
} catch (...) {
    return handle_com_exception();
}

/**
 * @brief Starts a new TSF composition session.
 *
 * Creates the TSF composition objects needed for a new input session.
 */
HRESULT TextService::start_composition(ITfContext* pContext) {
    if (!pContext) return E_INVALIDARG;
    if (itfComposition) return S_OK;

    winrt::com_ptr<ITfContextComposition> contextComposition;
    HRESULT hr = pContext->QueryInterface<ITfContextComposition>(contextComposition.put());
    if (FAILED(hr)) return hr;

    winrt::com_ptr<ITfInsertAtSelection> insertAtSelection;
    hr = pContext->QueryInterface<ITfInsertAtSelection>(insertAtSelection.put());
    if (FAILED(hr)) return hr;

    winrt::com_ptr<EditSession> editSession = winrt::make_self<EditSession>();

    editSession->set_operation([this, pContext, contextComposition, insertAtSelection](TfEditCookie ec) {
        winrt::com_ptr<ITfRange> range;
        if (FAILED(insertAtSelection->InsertTextAtSelection(ec, TF_IAS_QUERYONLY, L"", 0, range.put()))) {
            return;
        }
        if (FAILED(contextComposition->StartComposition(
                ec, range.get(), static_cast<ITfCompositionSink*>(this), itfComposition.put()))) {
            return;
        }
        composition_context_.copy_from(pContext);
    });

    HRESULT hrSession;
    pContext->RequestEditSession(_tfClientId, editSession.get(), TF_ES_READWRITE | TF_ES_SYNC, &hrSession) |
        win::check();

    return S_OK;
}

/**
 * @brief Ends the active TSF composition session.
 *
 * Clears the current composition object and buffered text.
 */
HRESULT TextService::end_composition(ITfContext* pContext) {
    candidate_ui_->hide();
    if (!itfComposition) {
        clear_composition_state();
        return S_OK;
    }

    const std::u16string text = compositionBuffer.to_string();
    winrt::com_ptr<EditSession> editSession = winrt::make_self<EditSession>();
    editSession->set_operation([this, pContext, text](TfEditCookie ec) {
        before_return cleanup([this]() { clear_composition_state(); });
        if (itfComposition) {
            winrt::com_ptr<ITfRange> range;
            itfComposition->GetRange(range.put()) | win::check();
            range->SetText(ec, 0, convu16(text.data()), static_cast<LONG>(text.size())) | win::check();

            winrt::com_ptr<ITfRange> caret_range;
            range->Clone(caret_range.put()) | win::check();
            caret_range->Collapse(ec, TF_ANCHOR_END) | win::check();

            itfComposition->EndComposition(ec) | win::check();
            itfComposition = nullptr;

            TF_SELECTION selection = {};
            selection.range = caret_range.get();
            selection.style.ase = TF_AE_END;
            selection.style.fInterimChar = FALSE;
            pContext->SetSelection(ec, 1, &selection) | win::check();

        }
    });

    HRESULT hrSession;
    pContext->RequestEditSession(_tfClientId, editSession.get(), TF_ES_READWRITE | TF_ES_SYNC, &hrSession) |
        win::check();

    return S_OK;
}

HRESULT TextService::discard_composition(ITfContext* pContext) {
    candidate_ui_->hide();
    compositionBuffer.clear();
    if (!pContext) return E_INVALIDARG;
    if (!itfComposition) {
        clear_composition_state();
        return S_OK;
    }

    winrt::com_ptr<EditSession> editSession = winrt::make_self<EditSession>();
    editSession->set_operation([this, pContext](TfEditCookie ec) {
        before_return cleanup([this]() { clear_composition_state(); });
        if (!itfComposition) {
            return;
        }

        winrt::com_ptr<ITfRange> range;
        itfComposition->GetRange(range.put()) | win::check();

        winrt::com_ptr<ITfRange> caret_range;
        range->Clone(caret_range.put()) | win::check();
        caret_range->Collapse(ec, TF_ANCHOR_START) | win::check();

        range->SetText(ec, 0, L"", 0) | win::check();
        itfComposition->EndComposition(ec) | win::check();
        itfComposition = nullptr;

        TF_SELECTION selection = {};
        selection.range = caret_range.get();
        selection.style.ase = TF_AE_END;
        selection.style.fInterimChar = FALSE;
        pContext->SetSelection(ec, 1, &selection) | win::check();
    });

    HRESULT hrSession;
    pContext->RequestEditSession(_tfClientId, editSession.get(), TF_ES_READWRITE | TF_ES_SYNC, &hrSession) |
        win::check();

    return S_OK;
}

HRESULT TextService::insert_text(ITfContext* pContext, const std::u16string& text) {
    if (!pContext) return E_INVALIDARG;
    if (text.empty()) return S_OK;

    winrt::com_ptr<ITfInsertAtSelection> insertAtSelection;
    HRESULT hr = pContext->QueryInterface<ITfInsertAtSelection>(insertAtSelection.put());
    if (FAILED(hr)) return hr;

    winrt::com_ptr<EditSession> editSession = winrt::make_self<EditSession>();
    editSession->set_operation([pContext, insertAtSelection, text](TfEditCookie ec) {
        winrt::com_ptr<ITfRange> range;
        insertAtSelection->InsertTextAtSelection(ec, TF_IAS_QUERYONLY, nullptr, 0, range.put()) | win::check();
        range->SetText(ec, 0, convu16(text.data()), static_cast<LONG>(text.size())) | win::check();

        winrt::com_ptr<ITfRange> caret_range;
        range->Clone(caret_range.put()) | win::check();
        caret_range->Collapse(ec, TF_ANCHOR_END) | win::check();

        TF_SELECTION selection = {};
        selection.range = caret_range.get();
        selection.style.ase = TF_AE_END;
        selection.style.fInterimChar = FALSE;
        pContext->SetSelection(ec, 1, &selection) | win::check();
    });

    HRESULT hrSession;
    pContext->RequestEditSession(_tfClientId, editSession.get(), TF_ES_READWRITE | TF_ES_SYNC, &hrSession) |
        win::check();

    return S_OK;
}

/**
 * @brief Updates the active composition text.
 *
 * Applies the visible composition string to the current TSF context.
 */
HRESULT TextService::set_composition_text(ITfContext* pContext, const std::u16string& text,
                                          size_t select_start, size_t select_length,
                                          std::optional<E2eTrace> e2e_trace) {
    if (!pContext) return E_INVALIDARG;

    winrt::com_ptr<ITfContextComposition> contextComposition;
    HRESULT hr = pContext->QueryInterface<ITfContextComposition>(contextComposition.put());
    if (FAILED(hr)) return hr;

    winrt::com_ptr<ITfInsertAtSelection> insertAtSelection;
    hr = pContext->QueryInterface<ITfInsertAtSelection>(insertAtSelection.put());
    if (FAILED(hr)) return hr;

    winrt::com_ptr<EditSession> editSession = winrt::make_self<EditSession>();
    std::chrono::steady_clock::time_point edit_request_started;
    editSession->set_operation([=, this, &edit_request_started](TfEditCookie ec) {
        const auto edit_operation_started = std::chrono::steady_clock::now();
        winrt::com_ptr<ITfRange> range;
        if (!itfComposition) {
            insertAtSelection->InsertTextAtSelection(ec, TF_IAS_QUERYONLY, nullptr, 0, range.put()) | win::check();
            contextComposition->StartComposition(
                ec, range.get(), static_cast<ITfCompositionSink*>(this), itfComposition.put()) |
                win::check();
            composition_context_.copy_from(pContext);
        }

        range = nullptr;
        itfComposition->GetRange(range.put()) | win::check();
        const auto edit_set_text_started = std::chrono::steady_clock::now();
        range->SetText(ec, 0, convu16(text.data()), ULONG(text.size())) | win::check();
        const auto edit_set_text_finished = std::chrono::steady_clock::now();

        apply_composition_display_attribute(pContext, ec, range.get()) | win::check();
        const auto edit_attribute_finished = std::chrono::steady_clock::now();

        const bool select_span = select_start != std::u16string::npos && select_length > 0;
        winrt::com_ptr<ITfRange> selection_range;
        range->Clone(selection_range.put()) | win::check();
        if (select_span) {
            const LONG start = static_cast<LONG>(select_start);
            const LONG length = static_cast<LONG>(select_length);
            LONG shifted = 0;
            selection_range->Collapse(ec, TF_ANCHOR_START) | win::check();
            selection_range->ShiftEnd(ec, start + length, &shifted, nullptr) | win::check();
            selection_range->ShiftStart(ec, start, &shifted, nullptr) | win::check();
        } else {
            const LONG caret = static_cast<LONG>(std::min(compositionBuffer.caret_offset(), text.size()));
            LONG shifted = 0;
            selection_range->Collapse(ec, TF_ANCHOR_START) | win::check();
            selection_range->ShiftEnd(ec, caret, &shifted, nullptr) | win::check();
            selection_range->Collapse(ec, TF_ANCHOR_END) | win::check();
        }

        TF_SELECTION selection = {};
        selection.range = selection_range.get();
        selection.style.ase = select_span ? TF_AE_NONE : TF_AE_END;
        selection.style.fInterimChar = FALSE;
        pContext->SetSelection(ec, 1, &selection) | win::check();
        if (e2e_trace) {
            pending_e2e_trace_ = PendingE2eTrace{
                .trace = *e2e_trace,
                .edit_request_started = edit_request_started,
                .edit_operation_started = edit_operation_started,
                .edit_set_text_started = edit_set_text_started,
                .edit_set_text_finished = edit_set_text_finished,
                .edit_attribute_finished = edit_attribute_finished,
                .edit_operation_finished = std::chrono::steady_clock::now(),
            };
        }
    });
    if (e2e_trace) {
        const HRESULT sink_hr = ensure_text_edit_sink(pContext);
        if (FAILED(sink_hr)) return sink_hr;
        pending_e2e_trace_.reset();
    }
    HRESULT session_hr = E_FAIL;
    edit_request_started = std::chrono::steady_clock::now();
    const HRESULT request_hr = pContext->RequestEditSession(
        _tfClientId, editSession.get(), TF_ES_READWRITE | TF_ES_SYNC, &session_hr);
    if (FAILED(request_hr) || FAILED(session_hr)) {
        if (e2e_trace && pending_e2e_trace_ &&
            pending_e2e_trace_->trace.sequence == e2e_trace->sequence) {
            pending_e2e_trace_.reset();
        }
        return FAILED(request_hr) ? request_hr : session_hr;
    }
    return S_OK;
}

void TextService::refresh_composition_after_candidate_finalize(
    ITfContext* pContext, E2eTrace e2e_trace) {
    if (!pContext || compositionBuffer.empty()) {
        return;
    }

    compositionBuffer.invalidate_all_predictions();
    e2e_trace.pre_context_started = std::chrono::steady_clock::now();
    auto pre_context = get_pre_composit_context(pContext);
    e2e_trace.pre_context_finished = std::chrono::steady_clock::now();
    e2e_trace.predict_started = std::chrono::steady_clock::now();
    const bool prediction_performed = compositionBuffer.predict_paddings(std::move(pre_context));
    e2e_trace.predict_finished = std::chrono::steady_clock::now();
    set_composition_text(pContext, compositionBuffer.to_string(), std::u16string::npos, 0,
                         prediction_performed ? std::optional<E2eTrace>(e2e_trace)
                                              : std::nullopt);
}

HRESULT TextService::ensure_text_edit_sink(ITfContext* context) {
    if (!context) return E_INVALIDARG;
    if (text_edit_sink_context_ && same_com_object(text_edit_sink_context_.get(), context) &&
        text_edit_sink_cookie_ != TF_INVALID_COOKIE) {
        return S_OK;
    }

    unadvise_text_edit_sink();
    winrt::com_ptr<ITfSource> source;
    HRESULT hr = context->QueryInterface<ITfSource>(source.put());
    if (FAILED(hr)) return hr;

    DWORD cookie = TF_INVALID_COOKIE;
    hr = source->AdviseSink(IID_ITfTextEditSink, static_cast<ITfTextEditSink*>(this), &cookie);
    if (FAILED(hr)) return hr;

    text_edit_sink_context_.copy_from(context);
    text_edit_sink_cookie_ = cookie;
    return S_OK;
}

void TextService::unadvise_text_edit_sink() noexcept {
    pending_e2e_trace_.reset();
    if (text_edit_sink_context_ && text_edit_sink_cookie_ != TF_INVALID_COOKIE) {
        winrt::com_ptr<ITfSource> source;
        if (SUCCEEDED(text_edit_sink_context_->QueryInterface<ITfSource>(source.put()))) {
            source->UnadviseSink(text_edit_sink_cookie_);
        }
    }
    text_edit_sink_cookie_ = TF_INVALID_COOKIE;
    text_edit_sink_context_ = nullptr;
}

void TextService::show_candidate_list_for_current_input(ITfContext* pContext, bool expand) {
    if (!pContext || compositionBuffer.empty() || !compositionBuffer.current_has_candidate_list()) {
        candidate_ui_->hide();
        return;
    }

    auto& target = compositionBuffer.candidate_target();
    show_candidate_list(target, pContext);
    if (expand) {
        candidate_ui_->expand();
    }
}

/**
 * @brief Displays the candidate list UI with the given candidates.
 */
void TextService::show_candidate_list(BopomofoPos& bopomofoPos, ITfContext* pContext) {
    // TODO
    std::shared_ptr<std::vector<std::wstring>> candidate_ptr = std::make_shared<std::vector<std::wstring>>();
    for (auto candi : bopomofoPos.get_candidates()) {
        std::u16string str;
        utf8::append16(candi, str);
        std::wstring wstr(str.begin(), str.end());
        candidate_ptr->push_back(wstr);
    }

    candidate_ui_->show(pContext, *candidate_ptr, [&bopomofoPos, candidate_ptr](std::wstring word) {
        auto& candidate = *candidate_ptr;
        for (int i = 0; i < candidate.size(); i++) {
            if (word == candidate[i]) {
                bopomofoPos.set_choose_index(i);
                return;
            }
        }
    });
}

inline constexpr LONG kMaxPreCompositionContextChars = 256;
inline constexpr ULONG kRangeReadChunkChars = 128;

bool append_range_text(TfEditCookie ec, ITfRange* range, std::u16string& context) {
    if (!range) {
        return false;
    }

    std::array<WCHAR, kRangeReadChunkChars> chunk = {};
    while (true) {
        ULONG copied = 0;
        const HRESULT hr = range->GetText(ec, TF_TF_MOVESTART, chunk.data(), static_cast<ULONG>(chunk.size()), &copied);
        if (FAILED(hr)) {
            return false;
        }
        if (copied == 0) {
            return true;
        }

        context.append(reinterpret_cast<const char16_t*>(chunk.data()), copied);
        if (copied < chunk.size()) {
            return true;
        }
    }
}

bool read_pre_context_with_acp(TfEditCookie ec, ITfRange* anchor_range, std::u16string& context) {
    if (!anchor_range) {
        return false;
    }

    winrt::com_ptr<ITfRangeACP> anchor_acp;
    if (FAILED(anchor_range->QueryInterface(IID_PPV_ARGS(anchor_acp.put())))) {
        return false;
    }

    LONG anchor_start = 0;
    LONG anchor_length = 0;
    if (FAILED(anchor_acp->GetExtent(&anchor_start, &anchor_length))) {
        return false;
    }
    if (anchor_start <= 0) {
        return true;
    }

    const LONG start = std::max<LONG>(0, anchor_start - kMaxPreCompositionContextChars);
    const LONG length = anchor_start - start;
    if (length <= 0) {
        return true;
    }

    winrt::com_ptr<ITfRange> read_range;
    if (FAILED(anchor_range->Clone(read_range.put())) || !read_range) {
        return false;
    }

    winrt::com_ptr<ITfRangeACP> read_acp;
    if (FAILED(read_range->QueryInterface(IID_PPV_ARGS(read_acp.put())))) {
        return false;
    }
    if (FAILED(read_acp->SetExtent(start, length))) {
        return false;
    }

    context.clear();
    return append_range_text(ec, read_range.get(), context);
}

bool read_pre_context_with_shift(TfEditCookie ec, ITfRange* anchor_range, std::u16string& context) {
    if (!anchor_range) {
        return false;
    }

    winrt::com_ptr<ITfRange> read_range;
    if (FAILED(anchor_range->Clone(read_range.put())) || !read_range) {
        return false;
    }

    LONG shifted = 0;
    if (FAILED(read_range->ShiftStart(ec, -kMaxPreCompositionContextChars, &shifted, nullptr))) {
        return false;
    }

    context.clear();
    return append_range_text(ec, read_range.get(), context);
}

std::u16string TextService::get_pre_composit_context(ITfContext* pContext) {
    if (!pContext) {
        return {};
    }

    std::u16string context;
    winrt::com_ptr<EditSession> editSession = winrt::make_self<EditSession>();
    editSession->set_operation([this, pContext, &context](TfEditCookie ec) {
        winrt::com_ptr<ITfRange> anchor_range;

        if (itfComposition) {
            itfComposition->GetRange(anchor_range.put()) | win::check();
        } else {
            TF_SELECTION selection = {};
            ULONG fetched = 0;
            pContext->GetSelection(ec, TF_DEFAULT_SELECTION, 1, &selection, &fetched) | win::check();
            if (fetched == 0 || !selection.range) {
                return;
            }

            anchor_range.attach(selection.range);
        }

        anchor_range->Collapse(ec, TF_ANCHOR_START) | win::check();
        if (!read_pre_context_with_acp(ec, anchor_range.get(), context)) {
            read_pre_context_with_shift(ec, anchor_range.get(), context);
        }
    });

    HRESULT hr = S_OK;
    pContext->RequestEditSession(_tfClientId, editSession.get(), TF_ES_READ | TF_ES_SYNC, &hr) | win::check();
    return context;
}

}  // namespace tsf
