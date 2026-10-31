#include "textService.h"

#include <algorithm>
#include <array>
#include <chrono>
#include <cstdint>
#include <cwctype>
#include <filesystem>
#include <format>
#include <fstream>
#include <inputscope.h>
#include <optional>

#include <rfl/DefaultIfMissing.hpp>
#include <rfl/json.hpp>

#include "candidateUiController.hpp"
#include "core/bopomofo.hpp"
#include "editSession.hpp"
#include "engine/engine.hpp"
#include "engine/engine_worker.hpp"
#include "engine/impl/engine.h"
#include "inputModeLangBarItem.hpp"
#include "system/globals.h"
#include "system/ui_dispatcher.hpp"
#include "utils/healper.hpp"

using namespace std::literals;

namespace {

// InputScope.h declares this property GUID but does not provide a linked definition.
inline constexpr GUID kInputScopePropertyGuid = {
    0x1713dd5a, 0x68e7, 0x4a5b, {0x9a, 0xf6, 0x59, 0x2a, 0x59, 0x5c, 0x77, 0x8d}};

bool has_private_input_scope(ITfContext* context, TfEditCookie cookie,
                             ITfRange* range) noexcept {
    if (!context || !range) return false;

    winrt::com_ptr<ITfReadOnlyProperty> property;
    if (FAILED(context->GetAppProperty(kInputScopePropertyGuid, property.put())) ||
        !property) {
        return false;
    }

    VARIANT value;
    VariantInit(&value);
    const HRESULT value_hr = property->GetValue(cookie, range, &value);
    bool is_private = false;
    if (SUCCEEDED(value_hr) &&
        (value.vt == VT_UNKNOWN || value.vt == VT_DISPATCH) && value.punkVal) {
        winrt::com_ptr<ITfInputScope> input_scope;
        if (SUCCEEDED(value.punkVal->QueryInterface<ITfInputScope>(
                input_scope.put())) && input_scope) {
            InputScope* scopes = nullptr;
            UINT count = 0;
            if (SUCCEEDED(input_scope->GetInputScopes(&scopes, &count)) && scopes) {
                for (UINT index = 0; index < count; ++index) {
                    if (scopes[index] == IS_PRIVATE) {
                        is_private = true;
                        break;
                    }
                }
            }
            CoTaskMemFree(scopes);
        }
    }
    VariantClear(&value);
    return is_private;
}

struct InputSettings {
    bool shift_space_width_toggle_enabled = false;
    bool immediate_conversion_enabled = false;
};

InputSettings load_input_settings() noexcept {
    try {
        const DWORD required = GetEnvironmentVariableW(L"LOCALAPPDATA", nullptr, 0);
        if (required == 0) return {};

        std::wstring local_app_data(required, L'\0');
        const DWORD copied = GetEnvironmentVariableW(
            L"LOCALAPPDATA", local_app_data.data(), required);
        if (copied == 0 || copied >= required) return {};
        local_app_data.resize(copied);

        const auto path = std::filesystem::path(local_app_data) /
                          L"Llavon IME" / L"settings.json";
        std::ifstream input(path, std::ios::binary);
        if (!input) return {};

        const auto settings = rfl::json::read<
            InputSettings, rfl::DefaultIfMissing>(input);
        return settings ? settings.value() : InputSettings{};
    } catch (...) {
        return {};
    }
}

void request_settings_window() noexcept {
    // This class and message name form the small cross-process contract with
    // the service tray window. No pointers or process-local message IDs cross
    // the process boundary.
    constexpr wchar_t service_window_class[] = L"LlavonImeServiceTrayWindow";
    constexpr wchar_t open_settings_message_name[] = L"LlavonIme.OpenSettings";

    const HWND service_window = FindWindowW(service_window_class, nullptr);
    const UINT message = RegisterWindowMessageW(open_settings_message_name);
    if (!service_window || message == 0) {
        return;
    }

    DWORD service_process_id = 0;
    GetWindowThreadProcessId(service_window, &service_process_id);
    if (service_process_id != 0) {
        // Best effort: let the settings window come to the foreground when
        // this command originated from the user's language-bar interaction.
        AllowSetForegroundWindow(service_process_id);
    }
    PostMessageW(service_window, message, 0, 0);
}

void request_settings_menu(POINT location) noexcept {
    constexpr wchar_t service_window_class[] = L"LlavonImeServiceTrayWindow";
    constexpr wchar_t show_menu_message_name[] = L"LlavonIme.ShowInputModeMenu";

    const HWND service_window = FindWindowW(service_window_class, nullptr);
    const UINT message = RegisterWindowMessageW(show_menu_message_name);
    if (!service_window || message == 0) {
        return;
    }

    DWORD service_process_id = 0;
    GetWindowThreadProcessId(service_window, &service_process_id);
    if (service_process_id != 0) {
        AllowSetForegroundWindow(service_process_id);
    }
    PostMessageW(service_window, message,
                 static_cast<WPARAM>(static_cast<std::uint32_t>(location.x)),
                 static_cast<LPARAM>(static_cast<std::uint32_t>(location.y)));
}

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

template <typename... Handlers>
struct overloaded : Handlers... {
    using Handlers::operator()...;
};

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

// What the key types on the current keyboard layout.
std::optional<std::u16string> layout_text(WPARAM wParam, LPARAM lParam) {
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

}  // namespace

namespace tsf {

TextService::TextService() : candidate_ui_(std::make_unique<CandidateUiController>()) {}

TextService::~TextService() {
    // TSF normally calls Deactivate first. Keep destruction safe and complete
    // for partial activation and unusual host shutdown paths as well.
    deactivate();
}

bool append_range_text(TfEditCookie ec, ITfRange* range, std::u16string& context);

void TextService::start_engine_worker() {
    if (engine_worker_) return;
    ui_dispatcher_ = std::make_unique<UiDispatcher>(Globals::hinstance);
    try {
        engine_worker_ = std::make_unique<EngineWorker>(
            &make_pipe_engine,
            [dispatcher = ui_dispatcher_.get()](EngineWorker::UiTask task) {
                dispatcher->post(std::move(task));
            },
            EngineWorker::Callbacks{
                .prediction =
                    [this](ConversionResult result) {
                        auto lifetime = get_strong();
                        try { controller_.on_conversion(std::move(result)); }
                        catch (...) { handle_com_exception(); }
                    },
                .input_mode_changed =
                    [this](InputMode) {
                        auto lifetime = get_strong();
                        try { refresh_input_mode_indicator(false); }
                        catch (...) { handle_com_exception(); }
                    },
            });
        controller_.set_backend(engine_worker_.get());
        engine_worker_->refresh_input_mode();
    } catch (...) {
        stop_engine_worker();
        throw;
    }
}

void TextService::stop_engine_worker() noexcept {
    controller_.set_backend(nullptr);
    // Results still queued for this thread must not run against a stopped worker.
    if (ui_dispatcher_) ui_dispatcher_->close();
    engine_worker_.reset();
    ui_dispatcher_.reset();
}

DocumentId TextService::document_of(ITfContext* context) {
    winrt::com_ptr<IUnknown> identity;
    if (!context || FAILED(context->QueryInterface(IID_PPV_ARGS(identity.put())))) return 0;
    const auto document = reinterpret_cast<DocumentId>(identity.get());
    auto& known = documents_[document];
    if (!known) known.copy_from(context);
    return document;
}

winrt::com_ptr<ITfContext> TextService::context_of(DocumentId document) const {
    const auto known = documents_.find(document);
    return known == documents_.end() ? nullptr : known->second;
}

bool TextService::composing_in(ITfContext* context) const {
    winrt::com_ptr<IUnknown> identity;
    if (!context || !controller_.active() ||
        FAILED(context->QueryInterface(IID_PPV_ARGS(identity.put())))) {
        return false;
    }
    return controller_.owns(reinterpret_cast<DocumentId>(identity.get()));
}

// Keys arrived in another document while a composition is open elsewhere:
// it is finished in its own document, and this key is handled normally.
void TextService::leave_other_document(DocumentId document) {
    if (controller_.active() && !controller_.owns(document)) {
        candidate_ui_->hide();
        controller_.seal();
    }
}

InputMode TextService::cached_input_mode() const {
    return engine_worker_ ? engine_worker_->cached_input_mode() : InputMode::English;
}

// ---- CompositionHost -----------------------------------------------------

// ---- edits -----------------------------------------------------------------
// TSF grants an edit session when the document allows it, which may be long
// after the request. Every write goes through one queue per document and runs
// in request order inside whichever session is granted next; nothing ever
// asks for a synchronous lock, which a document may refuse while an earlier
// request is still waiting.

bool TextService::run_edit(DocumentId document, std::function<void()> edit) {
    if (!context_of(document) || _tfClientId == TF_CLIENTID_NULL) return false;
    queued_edits_[document].push_back(std::move(edit));
    if (!active_edit_ || active_edit_->document != document) request_edits(document);
    return true;
}

bool TextService::has_queued_edits(DocumentId document) const {
    const auto queue = queued_edits_.find(document);
    return queue != queued_edits_.end() && !queue->second.empty();
}

// Each granted session runs everything queued by then, so a session that
// finds the queue empty is harmless.
void TextService::request_edits(DocumentId document) {
    const auto context = context_of(document);
    if (!context) return;
    const auto requested = std::chrono::steady_clock::now();
    auto session = winrt::make_self<EditSession>();
    session->set_operation([self = get_strong(), document, requested](TfEditCookie cookie) {
        self->run_edits(document, cookie, requested);
    });
    HRESULT session_hr = E_FAIL;
    const HRESULT hr = context->RequestEditSession(_tfClientId, session.get(),
                                                   TF_ES_READWRITE | TF_ES_ASYNCDONTCARE, &session_hr);
    if (FAILED(hr) || (FAILED(session_hr) && session_hr != TF_E_LOCKED)) {
        logger_.log([hr, session_hr] {
            return std::format("[EDIT] request failed hr=0x{:08x} session=0x{:08x}", static_cast<std::uint32_t>(hr),
                               static_cast<std::uint32_t>(session_hr));
        });
    } else if (session_hr == TF_S_ASYNC) {
        logger_.log("[EDIT] request queued by the document");
    }
}

void TextService::run_edits(DocumentId document, TfEditCookie cookie,
                            std::chrono::steady_clock::time_point requested) {
    const auto waited = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - requested);
    if (waited > std::chrono::milliseconds(50) && has_queued_edits(document)) {
        logger_.log([waited] { return std::format("[EDIT] granted after {:.1f}ms", waited.count()); });
    }
    const auto outer = std::exchange(active_edit_, ActiveEdit{document, cookie});
    before_return restore([&] { active_edit_ = outer; });
    run_queued(document, [] { return true; });
}

// Runs queued edits of document while more() holds, in order, including
// those queued meanwhile.
void TextService::run_queued(DocumentId document, const std::function<bool()>& more) {
    while (more()) {
        const auto queue = queued_edits_.find(document);
        if (queue == queued_edits_.end() || queue->second.empty()) return;
        auto edit = std::move(queue->second.front());
        queue->second.pop_front();
        try {
            edit();
        } catch (...) {
            handle_com_exception();
        }
    }
}

TfEditCookie TextService::edit_cookie() const {
    if (!active_edit_) throw std::logic_error("document accessed outside an edit session");
    return active_edit_->cookie;
}

void TextService::erase_before_caret(DocumentId document) {
    const auto context = context_of(document);
    if (!context) return;
    const auto cookie = edit_cookie();
    TF_SELECTION selection = {};
    ULONG fetched = 0;
    if (FAILED(context->GetSelection(cookie, TF_DEFAULT_SELECTION, 1, &selection, &fetched)) || fetched == 0 ||
        !selection.range) {
        return;
    }
    winrt::com_ptr<ITfRange> range;
    range.attach(selection.range);
    BOOL empty = FALSE;
    range->IsEmpty(cookie, &empty) | win::check();
    if (empty) {
        LONG moved = 0;
        if (FAILED(range->ShiftStart(cookie, -1, &moved, nullptr)) || moved == 0) return;
        WCHAR first = 0;
        ULONG read = 0;
        if (SUCCEEDED(range->GetText(cookie, 0, &first, 1, &read)) && read == 1 && first >= 0xDC00 &&
            first <= 0xDFFF) {
            range->ShiftStart(cookie, -1, &moved, nullptr);
        }
    }
    range->SetText(cookie, 0, L"", 0) | win::check();
}

// ---- CompositionView -----------------------------------------------------

std::u16string TextService::preceding_text(DocumentId document, std::uint64_t epoch) {
    const auto context = context_of(document);
    return context ? get_pre_composit_context(context.get(), edit_cookie(), epoch) : std::u16string{};
}

void TextService::render(DocumentId document, std::uint64_t epoch, const Rendering& rendering,
                         const E2eTrace* trace) {
    const auto context = context_of(document);
    if (!context) return;
    const auto requested = std::chrono::steady_clock::now();
    if (trace) ensure_text_edit_sink(context.get());
    write_composition_text(context.get(), edit_cookie(), epoch, rendering, trace, requested);
}

void TextService::commit(DocumentId document, std::uint64_t epoch) {
    candidate_ui_->hide();
    if (composition_epoch_ != epoch) return;
    if (termination_) {
        // Already ended by the host; only our display attribute remains.
        if (const auto context = context_of(document); context && termination_->range) {
            winrt::com_ptr<ITfProperty> attribute_property;
            if (SUCCEEDED(context->GetProperty(GUID_PROP_ATTRIBUTE, attribute_property.put()))) {
                attribute_property->Clear(termination_->cookie, termination_->range);
            }
        }
        drop_composition();
        return;
    }
    const auto context = context_of(document);
    if (!context || !itfComposition) {
        drop_composition();
        return;
    }
    finish_composition(context.get(), edit_cookie(), epoch);
}

void TextService::discard(DocumentId document, std::uint64_t epoch) {
    candidate_ui_->hide();
    if (composition_epoch_ != epoch) {
        private_scopes_.erase(epoch);
        return;
    }
    const auto context = context_of(document);
    if (!context || !itfComposition || termination_) {
        drop_composition();
        return;
    }
    erase_composition(context.get(), edit_cookie(), epoch);
}

// Ends the composition where it is, without touching its text, and puts the
// caret after it so typing continues there.
void TextService::seal(DocumentId document, std::uint64_t epoch) {
    candidate_ui_->hide();
    const auto context = context_of(document);
    const auto composition = itfComposition;
    const bool shown = composition_epoch_ == epoch;
    if (shown) drop_composition();
    if (!context || !shown) return;

    const auto clear_attribute = [&context](TfEditCookie cookie, ITfRange* range) {
        winrt::com_ptr<ITfProperty> attribute_property;
        if (range && SUCCEEDED(context->GetProperty(GUID_PROP_ATTRIBUTE, attribute_property.put()))) {
            attribute_property->Clear(cookie, range);
        }
    };
    const auto keep = [&](TfEditCookie cookie, ITfRange* range) {
        SealedRange kept{.context = context};
        if (range && SUCCEEDED(range->Clone(kept.range.put()))) {
            kept.range->SetGravity(cookie, TF_GRAVITY_FORWARD, TF_GRAVITY_BACKWARD);
            sealed_ranges_[epoch] = std::move(kept);
        }
    };

    if (termination_) {
        clear_attribute(termination_->cookie, termination_->range);
        keep(termination_->cookie, termination_->range);
        return;
    }
    if (!composition) return;

    const auto cookie = edit_cookie();
    winrt::com_ptr<ITfRange> range;
    if (SUCCEEDED(composition->GetRange(range.put()))) {
        clear_attribute(cookie, range.get());
        keep(cookie, range.get());
        winrt::com_ptr<ITfRange> caret;
        if (SUCCEEDED(range->Clone(caret.put())) && SUCCEEDED(caret->Collapse(cookie, TF_ANCHOR_END))) {
            TF_SELECTION selection = {};
            selection.range = caret.get();
            selection.style.ase = TF_AE_END;
            selection.style.fInterimChar = FALSE;
            context->SetSelection(cookie, 1, &selection);
        }
    }
    composition->EndComposition(cookie);
}

// ---- SealedTextStore -----------------------------------------------------

std::optional<std::u16string> TextService::sealed_text(DocumentId /*document*/, std::uint64_t epoch) {
    const auto found = sealed_ranges_.find(epoch);
    if (found == sealed_ranges_.end()) return std::nullopt;
    winrt::com_ptr<ITfRange> reader;
    std::u16string text;
    if (FAILED(found->second.range->Clone(reader.put())) || !append_range_text(edit_cookie(), reader.get(), text)) {
        return std::nullopt;
    }
    return text;
}

void TextService::write_sealed(DocumentId /*document*/, std::uint64_t epoch, const std::u16string& text) {
    const auto found = sealed_ranges_.find(epoch);
    if (found == sealed_ranges_.end()) return;
    found->second.range->SetText(edit_cookie(), 0, convu16(text).data(), static_cast<LONG>(text.size())) |
        win::check();
}

void TextService::erase_before_sealed(DocumentId /*document*/, std::uint64_t epoch, std::size_t count) {
    const auto found = sealed_ranges_.find(epoch);
    if (found == sealed_ranges_.end()) return;
    const auto cookie = edit_cookie();
    winrt::com_ptr<ITfRange> before;
    found->second.range->Clone(before.put()) | win::check();
    before->Collapse(cookie, TF_ANCHOR_START) | win::check();
    for (std::size_t erased = 0; erased < count; ++erased) {
        LONG moved = 0;
        if (FAILED(before->ShiftStart(cookie, -1, &moved, nullptr)) || moved == 0) break;
        WCHAR first = 0;
        ULONG fetched = 0;
        if (SUCCEEDED(before->GetText(cookie, 0, &first, 1, &fetched)) && fetched == 1 && first >= 0xDC00 &&
            first <= 0xDFFF) {
            before->ShiftStart(cookie, -1, &moved, nullptr);
        }
    }
    before->SetText(cookie, 0, L"", 0) | win::check();
}

void TextService::release_sealed(DocumentId /*document*/, std::uint64_t epoch) {
    sealed_ranges_.erase(epoch);
}

bool TextService::caret_after_sealed(DocumentId document, std::uint64_t epoch) {
    const auto found = sealed_ranges_.find(epoch);
    const auto context = context_of(document);
    if (found == sealed_ranges_.end() || !context) return false;
    const auto cookie = edit_cookie();
    TF_SELECTION selection = {};
    ULONG fetched = 0;
    if (FAILED(context->GetSelection(cookie, TF_DEFAULT_SELECTION, 1, &selection, &fetched)) || fetched == 0 ||
        !selection.range) {
        return false;
    }
    winrt::com_ptr<ITfRange> caret;
    caret.attach(selection.range);
    BOOL empty = FALSE;
    LONG order = 0;
    return SUCCEEDED(caret->IsEmpty(cookie, &empty)) && empty &&
           SUCCEEDED(caret->CompareStart(cookie, found->second.range.get(), TF_ANCHOR_END, &order)) && order == 0;
}

void TextService::record_commit(DocumentId document, std::uint64_t epoch, CommitSample sample) {
    const auto scope = private_scopes_.extract(epoch);
    const bool private_input_scope = !scope.empty() && scope.mapped();
    record_commit(std::move(sample), private_input_scope, context_of(document).get());
}

HRESULT TextService::handle_com_exception(std::source_location location) noexcept {
    return tsf::handle_com_exception(logger_, location);
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

    start_engine_worker();
    threadMgr.copy_from(pThreadMgr);
    _tfClientId = tfClientId;
    refresh_input_settings();
    candidate_ui_->attach(pThreadMgr, tfClientId);

    input_mode_lang_bar_item_ = winrt::make_self<InputModeLangBarItem>(
        [this]() {
            if (engine_worker_) engine_worker_->toggle_input_mode();
            refresh_input_mode_indicator();
        },
        [] { request_settings_window(); },
        [](POINT location) { request_settings_menu(location); });
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
    candidate_ui_->hide();
    if (_tfClientId != TF_CLIENTID_NULL) {
        // Without a backend from here on, text still converting is finished
        // with the local table, the last resort.
        stop_engine_worker();
        controller_.shutdown();
    } else {
        stop_engine_worker();
        controller_.reset();
    }
    clear_composition_state();
    queued_edits_.clear();
    unadvise_text_edit_sink();

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

InputMode TextService::refresh_input_mode_indicator(bool query_backend) {
    if (engine_worker_ && query_backend) engine_worker_->refresh_input_mode();
    const InputMode mode = engine_worker_ ? engine_worker_->cached_input_mode() : InputMode::English;
    key_state_.mode_changed(mode);
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
    LONG conversion_mode = mode == InputMode::Chinese ? TF_CONVERSIONMODE_NATIVE : 0;
    if (full_width_mode_) conversion_mode |= TF_CONVERSIONMODE_FULLSHAPE;
    set_compartment(GUID_COMPARTMENT_KEYBOARD_INPUTMODE_CONVERSION, conversion_mode);
}

void TextService::refresh_input_settings() {
    const auto settings = load_input_settings();
    controller_.set_immediate_conversion(settings.immediate_conversion_enabled);
    const bool enabled = settings.shift_space_width_toggle_enabled;
    if (!enabled && shift_space_width_toggle_enabled_ && full_width_mode_) {
        full_width_mode_ = false;
        if (threadMgr && _tfClientId != TF_CLIENTID_NULL) {
            sync_input_mode_compartments(engine_worker_ ? engine_worker_->cached_input_mode() : InputMode::English);
        }
    }
    shift_space_width_toggle_enabled_ = enabled;
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

bool TextService::has_composition_state() const {
    return itfComposition || composition_context_ || controller_.active() || candidate_ui_->is_active();
}

// Forgets the TSF side of the shown composition.
void TextService::drop_composition() noexcept {
    itfComposition = nullptr;
    composition_context_ = nullptr;
    composition_epoch_.reset();
}

void TextService::clear_composition_state() {
    candidate_ui_->hide();
    drop_composition();
    controller_.reset();
}

void TextService::record_commit(CommitSample sample, bool private_input_scope,
                                ITfContext* context) noexcept {
    if (private_input_scope || sample.answer.empty() || sample.input.empty()) {
        return;
    }
    try {
        const std::uint64_t token = engine_worker_ ? engine_worker_->record_commit(sample) : 0;
        if (token == 0) return;
        recent_commit_context_.copy_from(context);
        recent_commit_time_ = std::chrono::steady_clock::now();
        recent_commit_token_ = token;
        recent_commit_tail_ = sample.context + sample.answer;
        constexpr std::size_t maximum_context_length = 256;
        if (recent_commit_tail_.size() > maximum_context_length) {
            recent_commit_tail_.erase(
                0, recent_commit_tail_.size() - maximum_context_length);
        }
    } catch (...) {
        // Collection is best effort and must never break text input.
    }
}

void TextService::forget_recent_commit() noexcept {
    recent_commit_context_ = nullptr;
    recent_commit_time_.reset();
    recent_commit_tail_.clear();
    recent_commit_token_ = 0;
}

void TextService::discard_recent_commit(ITfContext* context) noexcept {
    constexpr auto correction_window = std::chrono::seconds(10);
    try {
        if (!context || !recent_commit_context_ || !recent_commit_time_ ||
            !same_com_object(recent_commit_context_.get(), context) ||
            std::chrono::steady_clock::now() - *recent_commit_time_ > correction_window) {
            forget_recent_commit();
            return;
        }
        winrt::com_ptr<ITfContext> target;
        target.copy_from(context);
        run_edit(document_of(context), [this, target, tail = recent_commit_tail_, token = recent_commit_token_] {
            if (engine_worker_ && get_pre_composit_context(target.get(), edit_cookie(), std::nullopt) == tail) {
                engine_worker_->discard_commit(token);
            }
        });
        forget_recent_commit();
    } catch (...) {
        forget_recent_commit();
    }
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
    // Conversions keep running: their results land in the composition's own
    // context, wherever focus is now.
    forget_recent_commit();
    refresh_input_settings();
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
STDMETHODIMP TextService::OnPushContext(ITfContext* /*pContext*/) try {
    return S_OK;
} catch (...) {
    return handle_com_exception();
}

/**
 * @brief Implements ITfThreadMgrEventSink::OnPopContext.
 *
 * Releases contexts without additional cleanup.
 */
STDMETHODIMP TextService::OnPopContext(ITfContext* pContext) try {
    // The context is going away together with any text in it.
    const auto document = document_of(pContext);
    if (controller_.owns(document)) clear_composition_state();
    controller_.document_closed(document);
    queued_edits_.erase(document);
    std::erase_if(sealed_ranges_, [pContext](const auto& entry) {
        return same_com_object(entry.second.context.get(), pContext);
    });
    documents_.erase(document);
    if (recent_commit_context_ &&
        same_com_object(recent_commit_context_.get(), pContext)) {
        forget_recent_commit();
    }
    return S_OK;
} catch (...) {
    return handle_com_exception();
}

/**
 * @brief Implements ITfKeyEventSink::OnSetFocus.
 *
 * Tracks foreground changes but currently keeps no extra state.
 */
STDMETHODIMP TextService::OnSetFocus(BOOL fForeground) try {
    if (fForeground) {
        refresh_input_settings();
        refresh_input_mode_indicator();
    } else {
        forget_recent_commit();
    }
    return S_OK;
} catch (...) {
    return handle_com_exception();
}

keys::KeyPress TextService::key_press(WPARAM wParam, LPARAM lParam) const {
    keys::KeyPress press{
        .key = wParam,
        .modifiers = {.shift = key_down(VK_SHIFT), .ctrl = key_down(VK_CONTROL), .alt = key_down(VK_MENU)},
        .repeated = (lParam & (1LL << 30)) != 0};
    if (keys::printable(wParam, press.modifiers)) press.text = layout_text(wParam, lParam);
    return press;
}

keys::KeyContext TextService::key_context(ITfContext* context, WPARAM wParam, InputMode mode) {
    const auto document = document_of(context);
    const bool composing = controller_.owns(document);
    return {.mode = mode,
            .composing = composing,
            .reading_complete = composing && controller_.buffer().current_compositable(),
            .candidates_open = candidate_ui_->is_active(),
            .candidates_take_key = candidate_ui_->can_handle_key(wParam),
            .document_behind = wParam == VK_BACK && !composing &&
                               (controller_.sealed_pending(document) || has_queued_edits(document)),
            .full_width = full_width_mode_,
            .width_toggle_enabled = shift_space_width_toggle_enabled_,
            .backtick_prefix = key_state_.backtick_prefix()};
}

bool TextService::perform(ITfContext* context, DocumentId document, const keys::KeyPress& press,
                          const keys::KeyAction& action, E2eTrace& trace) {
    namespace act = keys::action;
    return std::visit(
        overloaded{
            [](const act::PassThrough&) { return false; },
            [](const act::ShiftDown&) { return true; },
            [&](const act::ToggleWidth&) {
                if (!press.repeated) full_width_mode_ = !full_width_mode_;
                sync_input_mode_compartments(cached_input_mode());
                return true;
            },
            [](const act::ArmBacktick&) { return true; },
            [&](const act::Insert& insert) {
                candidate_ui_->hide();
                insert_text(document, insert.text);
                return true;
            },
            [&](const act::Compose& compose) {
                candidate_ui_->hide();
                controller_.type_text(document, compose.text, trace);
                return true;
            },
            [&](const act::ComposeEnglish& compose) {
                candidate_ui_->hide();
                controller_.discard_reading(document);
                controller_.type_text(document, compose.text, trace);
                return true;
            },
            [&](const act::Reading& reading) {
                candidate_ui_->hide();
                if (!Bopomofo::tone.contains(reading.symbol)) {
                    trace.ready_started = std::chrono::steady_clock::now();
                    if (engine_worker_) engine_worker_->ready();
                    trace.ready_finished = std::chrono::steady_clock::now();
                }
                trace.pre_context_started = std::chrono::steady_clock::now();
                trace.pre_context_finished = trace.pre_context_started;
                controller_.type_reading(document, reading.symbol, trace);
                return true;
            },
            [&](const act::Candidates&) { return perform_candidate_key(context, document, press, trace); },
            [&](const act::OpenCandidates&) {
                show_candidate_list_for_current_input(context, false);
                return true;
            },
            [&](const act::MoveCaret& move) {
                candidate_ui_->hide();
                controller_.move_caret(document, move.forward);
                return true;
            },
            [&](const act::Backspace&) {
                candidate_ui_->hide();
                controller_.backspace(document, trace);
                return true;
            },
            [&](const act::BackspaceInOrder&) {
                controller_.backspace_in_order(document);
                return true;
            },
            [&](const act::Enter&) {
                candidate_ui_->hide();
                controller_.enter(document, trace);
                return true;
            },
            [&](const act::Escape&) {
                controller_.escape(document);
                return true;
            },
        },
        action);
}

bool TextService::perform_candidate_key(ITfContext* context, DocumentId document, const keys::KeyPress& press,
                                        E2eTrace& trace) {
    switch (candidate_ui_->handle_key(press.key)) {
        case CandidateKeyResult::navigated:
        case CandidateKeyResult::aborted:
            return true;
        case CandidateKeyResult::finalized:
            // The selection is fixed; the rest converts again around it.
            controller_.candidate_finalized(document, trace);
            return true;
        case CandidateKeyResult::not_handled:
        default:
            break;
    }
    auto without_list = key_context(context, press.key, cached_input_mode());
    without_list.candidates_open = false;
    return perform(context, document, press, keys::route(press, without_list), trace);
}

/**
 * @brief Implements ITfKeyEventSink::OnTestKeyDown.
 *
 * Decides the key once; OnKeyDown carries out the same decision.
 */
STDMETHODIMP TextService::OnTestKeyDown(ITfContext* pContext, WPARAM wParam, LPARAM lParam, BOOL* pfEaten) try {
    if (!pfEaten) return E_INVALIDARG;
    key_down_started_at_ = std::chrono::steady_clock::now();
    key_down_started_key_ = wParam;
    tested_key_.reset();
    *pfEaten = FALSE;
    if (!context_accepts_input(pContext)) return S_OK;

    const auto press = key_press(wParam, lParam);
    key_state_.observe(press);
    auto action = keys::route(press, key_context(pContext, wParam, read_backend_input_mode()));
    *pfEaten = keys::eaten(action) ? TRUE : FALSE;
    tested_key_ = TestedKey{wParam, std::move(action)};
    return S_OK;
} catch (...) {
    return handle_com_exception();
}

/**
 * @brief Implements ITfKeyEventSink::OnTestKeyUp.
 *
 * Eats the release of a Shift tap, which switches the input mode.
 */
STDMETHODIMP TextService::OnTestKeyUp(ITfContext* /*pContext*/, WPARAM wParam, LPARAM /*lParam*/, BOOL* pfEaten) try {
    if (!pfEaten) return E_INVALIDARG;
    *pfEaten = keys::is_shift(wParam) && key_state_.shift_tap_pending() ? TRUE : FALSE;
    return S_OK;
} catch (...) {
    return handle_com_exception();
}

/**
 * @brief Implements ITfKeyEventSink::OnKeyDown.
 *
 * Carries out the decision made for the key.
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

    auto tested = std::exchange(tested_key_, std::nullopt);
    if (!context_accepts_input(pContext)) return S_OK;

    const auto document = document_of(pContext);
    leave_other_document(document);
    // A result held back while the candidate list was open lands once it closes.
    before_return after_key([this, document] {
        if (!candidate_ui_->is_active()) controller_.candidates_closed(document);
    });

    const auto press = key_press(wParam, lParam);
    e2e_trace.mode_started = std::chrono::steady_clock::now();
    auto action = tested && tested->key == wParam
                      ? std::move(tested->action)
                      : keys::route(press, key_context(pContext, wParam, refresh_input_mode_indicator(false)));
    e2e_trace.mode_finished = std::chrono::steady_clock::now();
    e2e_trace.ready_started = e2e_trace.mode_finished;
    e2e_trace.ready_finished = e2e_trace.mode_finished;

    if (wParam == VK_BACK && !has_composition_state() &&
        !std::holds_alternative<keys::action::BackspaceInOrder>(action)) {
        discard_recent_commit(pContext);
    }
    key_state_.handled(press, action);
    *pfEaten = perform(pContext, document, press, action, e2e_trace) ? TRUE : FALSE;
    return S_OK;
} catch (...) {
    return handle_com_exception();
}

/**
 * @brief Implements ITfKeyEventSink::OnKeyUp.
 *
 * A Shift tap switches the input mode.
 */
STDMETHODIMP TextService::OnKeyUp(ITfContext* pContext, WPARAM wParam, LPARAM /*lParam*/, BOOL* pfEaten) try {
    if (!pfEaten) return E_INVALIDARG;
    *pfEaten = FALSE;
    if (!keys::is_shift(wParam) || !key_state_.shift_released()) return S_OK;

    const bool was_chinese = cached_input_mode() == InputMode::Chinese;
    if (engine_worker_) engine_worker_->toggle_input_mode();
    const auto mode = refresh_input_mode_indicator();
    // A Shift switch drops the unfinished reading, just like temporary
    // Shift-English input. Keep converted text and pending syllables.
    if (was_chinese && mode == InputMode::English && composing_in(pContext)) {
        candidate_ui_->hide();
        controller_.discard_reading(document_of(pContext));
    }
    *pfEaten = TRUE;
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
STDMETHODIMP TextService::OnCompositionTerminated(TfEditCookie ecWrite, ITfComposition* pComposition) try {
    if (itfComposition && pComposition && !same_com_object(itfComposition.get(), pComposition)) {
        return S_OK;
    }
    // The host ended the composition (focus change, click, its own commit).
    // Keep what the user typed; text still converting is finished in place.
    const auto document = document_of(composition_context_.get());
    const auto terminated = composition_epoch_;
    winrt::com_ptr<ITfRange> range;
    if (pComposition && SUCCEEDED(pComposition->GetRange(range.put())) && composition_context_ && terminated) {
        private_scopes_[*terminated] |= has_private_input_scope(composition_context_.get(), ecWrite, range.get());
    }
    candidate_ui_->hide();
    termination_ = Termination{ecWrite, range.get()};
    const auto outer = std::exchange(active_edit_, ActiveEdit{document, ecWrite});
    before_return restore([&] {
        active_edit_ = outer;
        termination_.reset();
    });
    // Input typed before the host ended the composition ends with it, even
    // when the document still shows an earlier composition. Edits queued for
    // the shown composition run now, with the host's cookie, so its text is
    // complete before it is sealed.
    if (controller_.owns(document)) controller_.seal();
    run_queued(document, [&] { return itfComposition && composition_epoch_ == terminated; });
    drop_composition();
    if (has_queued_edits(document)) request_edits(document);
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

    logger_.log(llavon::debug::LogInformation::general,
                [elapsed_ms, test_to_key_ms, key_pre_mode_ms, mode_ms, mode_to_ready_ms,
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

void TextService::finish_composition(ITfContext* pContext, TfEditCookie ec, std::uint64_t epoch) {
    before_return cleanup([this]() { drop_composition(); });
    if (!itfComposition) return;
    // The composition range already contains the final text. Rewriting it
    // here makes Chromium treat the commit as a replacement operation,
    // which can delete adjacent text around contenteditable boundaries.
    winrt::com_ptr<ITfRange> range;
    itfComposition->GetRange(range.put()) | win::check();
    private_scopes_[epoch] |= has_private_input_scope(pContext, ec, range.get());

    winrt::com_ptr<ITfRange> caret_range;
    range->Clone(caret_range.put()) | win::check();
    caret_range->Collapse(ec, TF_ANCHOR_END) | win::check();

    TF_SELECTION selection = {};
    selection.range = caret_range.get();
    selection.style.ase = TF_AE_END;
    selection.style.fInterimChar = FALSE;
    pContext->SetSelection(ec, 1, &selection) | win::check();

    // EndComposition only clears GUID_PROP_COMPOSING. The display
    // attribute is owned by this text service and must be removed here.
    winrt::com_ptr<ITfProperty> attribute_property;
    if (SUCCEEDED(pContext->GetProperty(GUID_PROP_ATTRIBUTE, attribute_property.put()))) {
        attribute_property->Clear(ec, range.get());
    }

    const auto composition = itfComposition;
    composition->EndComposition(ec) | win::check();
}

void TextService::erase_composition(ITfContext* pContext, TfEditCookie ec, std::uint64_t epoch) {
    before_return cleanup([this]() { drop_composition(); });
    private_scopes_.erase(epoch);
    if (!itfComposition) return;

    winrt::com_ptr<ITfRange> range;
    itfComposition->GetRange(range.put()) | win::check();

    winrt::com_ptr<ITfRange> caret_range;
    range->Clone(caret_range.put()) | win::check();
    caret_range->Collapse(ec, TF_ANCHOR_START) | win::check();

    range->SetText(ec, 0, L"", 0) | win::check();
    const auto composition = itfComposition;
    composition->EndComposition(ec) | win::check();

    TF_SELECTION selection = {};
    selection.range = caret_range.get();
    selection.style.ase = TF_AE_END;
    selection.style.fInterimChar = FALSE;
    pContext->SetSelection(ec, 1, &selection) | win::check();
}

void TextService::insert_text(DocumentId document, std::u16string text) {
    if (text.empty()) return;
    run_edit(document, [this, document, text = std::move(text)] {
        const auto context = context_of(document);
        if (!context) return;
        const auto ec = edit_cookie();
        winrt::com_ptr<ITfInsertAtSelection> insert_at_selection;
        context->QueryInterface<ITfInsertAtSelection>(insert_at_selection.put()) | win::check();
        winrt::com_ptr<ITfRange> range;
        insert_at_selection->InsertTextAtSelection(ec, TF_IAS_QUERYONLY, nullptr, 0, range.put()) | win::check();
        range->SetText(ec, 0, convu16(text.data()), static_cast<LONG>(text.size())) | win::check();

        winrt::com_ptr<ITfRange> caret_range;
        range->Clone(caret_range.put()) | win::check();
        caret_range->Collapse(ec, TF_ANCHOR_END) | win::check();

        TF_SELECTION selection = {};
        selection.range = caret_range.get();
        selection.style.ase = TF_AE_END;
        selection.style.fInterimChar = FALSE;
        context->SetSelection(ec, 1, &selection) | win::check();
    });
}

/**
 * @brief Updates the active composition text, starting the composition if needed.
 */
void TextService::write_composition_text(ITfContext* pContext, TfEditCookie ec, std::uint64_t epoch,
                                         const Rendering& rendering, const E2eTrace* e2e_trace,
                                         std::chrono::steady_clock::time_point edit_request_started) {
    const auto& text = rendering.text;
    const auto edit_operation_started = std::chrono::steady_clock::now();
    if (itfComposition && composition_epoch_ != epoch) {
        logger_.log("[EDIT] composition shown for another epoch; render skipped");
        return;
    }
    // A composition the host is ending cannot be started again inside its
    // termination; the next render shows it.
    if (!itfComposition && termination_) return;
    winrt::com_ptr<ITfRange> range;
    if (!itfComposition) {
        winrt::com_ptr<ITfContextComposition> contextComposition;
        pContext->QueryInterface<ITfContextComposition>(contextComposition.put()) | win::check();
        winrt::com_ptr<ITfInsertAtSelection> insertAtSelection;
        pContext->QueryInterface<ITfInsertAtSelection>(insertAtSelection.put()) | win::check();
        insertAtSelection->InsertTextAtSelection(ec, TF_IAS_QUERYONLY, nullptr, 0, range.put()) | win::check();
        contextComposition->StartComposition(
            ec, range.get(), static_cast<ITfCompositionSink*>(this), itfComposition.put()) |
            win::check();
        composition_context_.copy_from(pContext);
        composition_epoch_ = epoch;
    }

    range = nullptr;
    itfComposition->GetRange(range.put()) | win::check();
    const auto edit_set_text_started = std::chrono::steady_clock::now();
    range->SetText(ec, 0, convu16(text.data()), ULONG(text.size())) | win::check();
    const auto edit_set_text_finished = std::chrono::steady_clock::now();

    apply_composition_display_attribute(pContext, ec, range.get()) | win::check();
    const auto edit_attribute_finished = std::chrono::steady_clock::now();

    const bool select_span = rendering.selection && rendering.selection->second > 0;
    winrt::com_ptr<ITfRange> selection_range;
    range->Clone(selection_range.put()) | win::check();
    LONG shifted = 0;
    selection_range->Collapse(ec, TF_ANCHOR_START) | win::check();
    if (select_span) {
        const auto start = static_cast<LONG>(rendering.selection->first);
        const auto length = static_cast<LONG>(rendering.selection->second);
        selection_range->ShiftEnd(ec, start + length, &shifted, nullptr) | win::check();
        selection_range->ShiftStart(ec, start, &shifted, nullptr) | win::check();
    } else {
        const auto caret = static_cast<LONG>(std::min(rendering.caret, text.size()));
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
    if (!composing_in(pContext) || !controller_.buffer().current_has_candidate_list()) {
        candidate_ui_->hide();
        return;
    }

    show_candidate_list(controller_.buffer().candidate_target(), pContext);
    if (expand) {
        candidate_ui_->expand();
    }
}

/**
 * @brief Displays the candidate list UI with the given candidates.
 */
void TextService::show_candidate_list(const BopomofoPos& bopomofoPos, ITfContext* pContext) {
    winrt::com_ptr<ITfRange> composition_range;
    if (!itfComposition || FAILED(itfComposition->GetRange(composition_range.put()))) {
        candidate_ui_->hide();
        return;
    }
    std::shared_ptr<std::vector<std::wstring>> candidate_ptr = std::make_shared<std::vector<std::wstring>>();
    for (const auto& str : bopomofoPos.get_candidate_texts()) {
        std::wstring wstr(str.begin(), str.end());
        candidate_ptr->push_back(wstr);
    }

    // The list refers to its entry by id: the buffer may change while it is open.
    const auto entry = bopomofoPos.id;
    controller_.candidates_opened(entry);
    candidate_ui_->show(pContext, composition_range.get(), controller_.buffer().candidate_target_span(),
                        *candidate_ptr, [this, entry, candidate_ptr](std::wstring word) {
        const auto& candidate = *candidate_ptr;
        const auto selected = std::ranges::find(candidate, word);
        if (selected != candidate.end()) {
            controller_.select_candidate(entry, static_cast<std::size_t>(selected - candidate.begin()));
        }
    });
}

inline constexpr LONG kMaxPreCompositionContextChars = 384;
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

std::u16string TextService::get_pre_composit_context(ITfContext* pContext, TfEditCookie ec,
                                                    std::optional<std::uint64_t> epoch) {
    if (!pContext) return {};
    winrt::com_ptr<ITfRange> anchor_range;
    if (itfComposition) {
        itfComposition->GetRange(anchor_range.put()) | win::check();
    } else {
        TF_SELECTION selection = {};
        ULONG fetched = 0;
        pContext->GetSelection(ec, TF_DEFAULT_SELECTION, 1, &selection, &fetched) | win::check();
        if (fetched == 0 || !selection.range) return {};
        anchor_range.attach(selection.range);
    }

    if (epoch) private_scopes_[*epoch] |= has_private_input_scope(pContext, ec, anchor_range.get());
    anchor_range->Collapse(ec, TF_ANCHOR_START) | win::check();
    std::u16string context;
    if (!read_pre_context_with_acp(ec, anchor_range.get(), context)) {
        read_pre_context_with_shift(ec, anchor_range.get(), context);
    }
    return context;
}

}  // namespace tsf
