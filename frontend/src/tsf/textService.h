#pragma once

#include <msctf.h>
#include <unknwn.h>
#include <llavon-debug/logger.hpp>
#include <winrt/base.h>

#include <chrono>
#include <map>
#include <memory>
#include <optional>
#include <source_location>
#include <string>
#include <utility>
#include <vector>

#include "core/composition_controller.hpp"
#include "core/e2e_trace.hpp"
#include "engine/engine_types.hpp"
#include "engine/engine_worker.hpp"
#include "system/sysutil.hpp"
#include "tsf/key_router.hpp"

namespace tsf {

class CandidateUiController;
class InputModeLangBarItem;
class UiDispatcher;

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
        ITfDisplayAttributeProvider>, private module_lock_updater, private CompositionHost {
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
    struct PendingE2eTrace {
        E2eTrace trace;
        std::chrono::steady_clock::time_point edit_request_started;
        std::chrono::steady_clock::time_point edit_operation_started;
        std::chrono::steady_clock::time_point edit_set_text_started;
        std::chrono::steady_clock::time_point edit_set_text_finished;
        std::chrono::steady_clock::time_point edit_attribute_finished;
        std::chrono::steady_clock::time_point edit_operation_finished;
    };

    // Text left in a document by a sealed composition.
    struct SealedRange {
        winrt::com_ptr<ITfContext> context;
        winrt::com_ptr<ITfRange> range;
    };

    // The host ended the composition; its edit cookie and range are valid for
    // the duration of OnCompositionTerminated only.
    struct Termination {
        TfEditCookie cookie;
        ITfRange* range;
    };

    // A key decided in OnTestKeyDown, carried out in OnKeyDown.
    struct TestedKey {
        WPARAM key = 0;
        keys::KeyAction action;
    };

    // DocumentEditor
    bool run_edit(DocumentId document, std::function<void()> edit) override;
    // CompositionView
    std::u16string preceding_text(DocumentId document) override;
    void render(DocumentId document, const Rendering& rendering, const E2eTrace* trace) override;
    void commit(DocumentId document) override;
    void discard(DocumentId document) override;
    void seal(DocumentId document, std::uint64_t epoch) override;
    // SealedTextStore
    std::optional<std::u16string> sealed_text(DocumentId document, std::uint64_t epoch) override;
    void write_sealed(DocumentId document, std::uint64_t epoch, const std::u16string& text) override;
    void erase_before_sealed(DocumentId document, std::uint64_t epoch, std::size_t count) override;
    void release_sealed(DocumentId document, std::uint64_t epoch) override;
    bool caret_after_sealed(DocumentId document, std::uint64_t epoch) override;
    // TrainingSink
    void record_commit(DocumentId document, std::uint64_t epoch, CommitSample sample) override;

    keys::KeyPress key_press(WPARAM wParam, LPARAM lParam) const;
    keys::KeyContext key_context(ITfContext* context, WPARAM wParam, InputMode mode);
    bool perform(ITfContext* context, DocumentId document, const keys::KeyPress& press,
                 const keys::KeyAction& action, E2eTrace& trace);
    bool perform_candidate_key(ITfContext* context, DocumentId document, const keys::KeyPress& press,
                               E2eTrace& trace);

    void start_engine_worker();
    void stop_engine_worker() noexcept;
    DocumentId document_of(ITfContext* context);
    winrt::com_ptr<ITfContext> context_of(DocumentId document) const;
    void leave_other_document(DocumentId document);
    InputMode cached_input_mode() const;

    HRESULT activate(ITfThreadMgr* pThreadMgr, TfClientId tfClientId);
    void deactivate();
    HRESULT handle_com_exception(
        std::source_location location = std::source_location::current()) noexcept;

    void with_edit_cookie(ITfContext* context, const std::function<void(TfEditCookie)>& edit);
    void finish_composition(ITfContext* context, TfEditCookie cookie);
    void erase_composition(ITfContext* context, TfEditCookie cookie);
    void drop_composition() noexcept;
    HRESULT insert_text(ITfContext* pContext, const std::u16string& text);
    void write_composition_text(ITfContext* context, TfEditCookie cookie, const Rendering& rendering,
                                const E2eTrace* trace,
                                std::chrono::steady_clock::time_point requested);
    HRESULT ensure_text_edit_sink(ITfContext* context);
    void unadvise_text_edit_sink() noexcept;
    void show_candidate_list_for_current_input(ITfContext* pContext, bool expand);
    void show_candidate_list(const BopomofoPos& pos, ITfContext* pContext);
    std::u16string get_pre_composit_context(ITfContext* pContext);
    InputMode read_backend_input_mode();
    InputMode refresh_input_mode_indicator(bool query_backend = true);
    void sync_input_mode_compartments(InputMode mode);
    void refresh_input_settings();
    bool context_accepts_input(ITfContext* context) const;
    bool composing_in(ITfContext* context) const;
    bool has_composition_state() const;
    void clear_composition_state();
    void record_commit(CommitSample sample, bool private_input_scope,
                       ITfContext* context) noexcept;
    void forget_recent_commit() noexcept;
    void discard_recent_commit(ITfContext* context) noexcept;

    winrt::com_ptr<ITfThreadMgr> threadMgr;
    TfClientId _tfClientId = TF_CLIENTID_NULL;
    DWORD dwThreadMgrEventSinkCookie = TF_INVALID_COOKIE;
    // The TSF side of the live composition.
    winrt::com_ptr<ITfComposition> itfComposition;
    winrt::com_ptr<ITfContext> composition_context_;
    llavon::debug::Logger logger_{"frontend"};
    std::unique_ptr<CandidateUiController> candidate_ui_;
    winrt::com_ptr<InputModeLangBarItem> input_mode_lang_bar_item_;
    CompositionController controller_{*this};
    std::unique_ptr<UiDispatcher> ui_dispatcher_;
    std::unique_ptr<EngineWorker> engine_worker_;
    std::map<DocumentId, winrt::com_ptr<ITfContext>> documents_;
    std::map<std::uint64_t, SealedRange> sealed_ranges_;
    // Composition epochs typed into a private (password-like) input scope.
    std::map<std::uint64_t, bool> private_scopes_;
    // Set while a TSF edit session runs a controller edit.
    std::optional<TfEditCookie> edit_cookie_;
    std::optional<Termination> termination_;
    keys::KeyState key_state_;
    std::optional<TestedKey> tested_key_;
    bool full_width_mode_ = false;
    bool shift_space_width_toggle_enabled_ = false;
    std::optional<std::chrono::steady_clock::time_point> key_down_started_at_;
    WPARAM key_down_started_key_ = 0;
    winrt::com_ptr<ITfContext> text_edit_sink_context_;
    DWORD text_edit_sink_cookie_ = TF_INVALID_COOKIE;
    std::optional<PendingE2eTrace> pending_e2e_trace_;
    std::uint64_t next_e2e_sequence_ = 1;
    winrt::com_ptr<ITfContext> recent_commit_context_;
    std::optional<std::chrono::steady_clock::time_point> recent_commit_time_;
    std::u16string recent_commit_tail_;
    std::uint64_t recent_commit_token_ = 0;
};

};  // namespace tsf
