#pragma once

#include <msctf.h>
#include <unknwn.h>
#include <llavon-debug/logger.hpp>
#include <winrt/base.h>

#include <chrono>
#include <memory>
#include <optional>
#include <source_location>
#include <string>
#include <variant>
#include <vector>

#include "core/buffer.hpp"
#include "engine/impl/engine.h"

namespace tsf {

class CandidateUiController;
class InputModeLangBarItem;

// clang-format off
class TextService : 
    public winrt::implements<
        TextService,
        ITfTextInputProcessor,
        ITfTextInputProcessorEx, 
        ITfThreadMgrEventSink,
        ITfKeyEventSink, 
        ITfCompositionSink, 
        ITfTextEditSink,
        ITfDisplayAttributeProvider> {
    // clang-format on
public:
    TextService();
    ~TextService();

    // ITfTextInputProcessor
    STDMETHODIMP Activate(ITfThreadMgr* pThreadMgr, TfClientId tfClientId) override;
    STDMETHODIMP Deactivate() override;

    // ITfTextInputProcessorEx
    STDMETHODIMP ActivateEx(ITfThreadMgr* pThreadMgr, TfClientId tfClientId, DWORD dwFlags) override;

    // ITfThreadMgrEventSink
    STDMETHODIMP OnInitDocumentMgr(ITfDocumentMgr* pDocMgr) override;
    STDMETHODIMP OnUninitDocumentMgr(ITfDocumentMgr* pDocMgr) override;
    STDMETHODIMP OnSetFocus(ITfDocumentMgr* pDocMgrFocus, ITfDocumentMgr* pDocMgrPrevFocus) override;
    STDMETHODIMP OnPushContext(ITfContext* pContext) override;
    STDMETHODIMP OnPopContext(ITfContext* pContext) override;

    // ITfKeyEventSink
    STDMETHODIMP OnSetFocus(BOOL fForeground) override;
    STDMETHODIMP OnTestKeyDown(ITfContext* pContext, WPARAM wParam, LPARAM lParam, BOOL* pfEaten) override;
    STDMETHODIMP OnTestKeyUp(ITfContext* pContext, WPARAM wParam, LPARAM lParam, BOOL* pfEaten) override;
    STDMETHODIMP OnKeyDown(ITfContext* pContext, WPARAM wParam, LPARAM lParam, BOOL* pfEaten) override;
    STDMETHODIMP OnKeyUp(ITfContext* pContext, WPARAM wParam, LPARAM lParam, BOOL* pfEaten) override;
    STDMETHODIMP OnPreservedKey(ITfContext* pContext, REFGUID rguid, BOOL* pfEaten) override;

    // ITfCompositionSink
    STDMETHODIMP OnCompositionTerminated(TfEditCookie ecWrite, ITfComposition* pComposition) override;

    // ITfTextEditSink
    STDMETHODIMP OnEndEdit(ITfContext* context, TfEditCookie read_only_cookie,
                           ITfEditRecord* edit_record) override;

    // ITfDisplayAttributeProvider
    STDMETHODIMP EnumDisplayAttributeInfo(IEnumTfDisplayAttributeInfo** ppEnum) override;
    STDMETHODIMP GetDisplayAttributeInfo(REFGUID guid, ITfDisplayAttributeInfo** ppInfo) override;

private:
    struct E2eTrace {
        std::chrono::steady_clock::time_point start;
        std::chrono::steady_clock::time_point on_key_started;
        std::chrono::steady_clock::time_point mode_started;
        std::chrono::steady_clock::time_point mode_finished;
        std::chrono::steady_clock::time_point ready_started;
        std::chrono::steady_clock::time_point ready_finished;
        std::chrono::steady_clock::time_point pre_context_started;
        std::chrono::steady_clock::time_point pre_context_finished;
        std::chrono::steady_clock::time_point predict_started;
        std::chrono::steady_clock::time_point predict_finished;
        std::uint64_t key = 0;
        std::uint64_t sequence = 0;
        bool started_at_on_test = false;
    };

    struct PendingE2eTrace {
        E2eTrace trace;
        std::chrono::steady_clock::time_point edit_request_started;
        std::chrono::steady_clock::time_point edit_operation_started;
        std::chrono::steady_clock::time_point edit_set_text_started;
        std::chrono::steady_clock::time_point edit_set_text_finished;
        std::chrono::steady_clock::time_point edit_attribute_finished;
        std::chrono::steady_clock::time_point edit_operation_finished;
    };

    HRESULT activate(ITfThreadMgr* pThreadMgr, TfClientId tfClientId);
    void deactivate();
    HRESULT handle_com_exception(
        std::source_location location = std::source_location::current()) noexcept;

    HRESULT start_composition(ITfContext* pContext);
    HRESULT end_composition(ITfContext* pContext);
    HRESULT discard_composition(ITfContext* pContext);
    HRESULT insert_text(ITfContext* pContext, const std::u16string& text);
    HRESULT set_composition_text(ITfContext* pContext, const std::u16string& text,
                                 size_t select_start = std::u16string::npos,
                                 size_t select_length = 0,
                                 std::optional<E2eTrace> e2e_trace = std::nullopt);
    void refresh_composition_after_candidate_finalize(ITfContext* pContext,
                                                       E2eTrace e2e_trace);
    HRESULT ensure_text_edit_sink(ITfContext* context);
    void unadvise_text_edit_sink() noexcept;
    void show_candidate_list_for_current_input(ITfContext* pContext, bool expand);
    void show_candidate_list(BopomofoPos& pos, ITfContext* pContext);
    std::u16string get_pre_composit_context(ITfContext* pContext);
    InputMode read_backend_input_mode();
    InputMode refresh_input_mode_indicator();
    void sync_input_mode_compartments(InputMode mode);
    bool context_accepts_input(ITfContext* context) const;
    bool composition_belongs_to(ITfContext* context) const;
    bool has_composition_state() const;
    void clear_composition_state();
    // Multifunctional shortcut handling
    std::optional<std::u16string> multifuntional_shortcut(WPARAM wParam);

    winrt::com_ptr<ITfThreadMgr> threadMgr;
    TfClientId _tfClientId = TF_CLIENTID_NULL;
    DWORD dwThreadMgrEventSinkCookie = TF_INVALID_COOKIE;
    winrt::com_ptr<ITfComposition> itfComposition;
    winrt::com_ptr<ITfContext> composition_context_;
    llavon::debug::Logger logger_{"frontend"};
    std::unique_ptr<CandidateUiController> candidate_ui_;
    winrt::com_ptr<InputModeLangBarItem> input_mode_lang_bar_item_;
    CompositionBuffer compositionBuffer;
    bool shift_toggle_pending_ = false;
    bool shift_used_as_modifier_ = false;
    std::optional<std::chrono::steady_clock::time_point> key_down_started_at_;
    WPARAM key_down_started_key_ = 0;
    winrt::com_ptr<ITfContext> text_edit_sink_context_;
    DWORD text_edit_sink_cookie_ = TF_INVALID_COOKIE;
    std::optional<PendingE2eTrace> pending_e2e_trace_;
    std::uint64_t next_e2e_sequence_ = 1;
    // Multifunctional shortcut handling
    bool backtick_used_as_modifier_ = false;
};

};  // namespace tsf
