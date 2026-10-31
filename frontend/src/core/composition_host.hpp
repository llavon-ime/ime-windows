#pragma once

#include <cstddef>
#include <cstdint>
#include <functional>
#include <optional>
#include <string>
#include <utility>

#include "core/e2e_trace.hpp"
#include "engine/engine_types.hpp"

namespace tsf {

// Identifies a document (a TSF context) without holding on to it.
using DocumentId = std::uintptr_t;

struct Rendering {
    std::u16string text;
    std::size_t caret = 0;
    std::optional<std::pair<std::size_t, std::size_t>> selection;
};

// The TSF contract: a document is read and written only inside an edit
// session the document grants, possibly much later than requested. Every
// method of the interfaces below except run_edit is called inside a run_edit
// callback.
class DocumentEditor {
public:
    virtual ~DocumentEditor() = default;
    // Queues edit behind every edit queued before it for the document.
    // Returns false when the document is gone.
    virtual bool run_edit(DocumentId document, std::function<void()> edit) = 0;
    // What Backspace does in the document itself.
    virtual void erase_before_caret(DocumentId document) = 0;
};

// The composition of a given epoch.
class CompositionView {
public:
    virtual ~CompositionView() = default;
    virtual std::u16string preceding_text(DocumentId document, std::uint64_t epoch) = 0;
    // Shows the composition, starting it if needed.
    virtual void render(DocumentId document, std::uint64_t epoch, const Rendering& rendering,
                        const E2eTrace* trace) = 0;
    // The shown text is final: ends the composition in place.
    virtual void commit(DocumentId document, std::uint64_t epoch) = 0;
    // Removes the composition together with its text.
    virtual void discard(DocumentId document, std::uint64_t epoch) = 0;
    // Ends the composition but leaves its text, tracked as sealed text under
    // epoch; the caret stays right after it.
    virtual void seal(DocumentId document, std::uint64_t epoch) = 0;
};

class SealedTextStore {
public:
    virtual ~SealedTextStore() = default;
    // Nothing once the text is gone or no longer tracked.
    virtual std::optional<std::u16string> sealed_text(DocumentId document, std::uint64_t epoch) = 0;
    virtual void write_sealed(DocumentId document, std::uint64_t epoch, const std::u16string& text) = 0;
    // Removes count characters right before the sealed text.
    virtual void erase_before_sealed(DocumentId document, std::uint64_t epoch, std::size_t count) = 0;
    virtual void release_sealed(DocumentId document, std::uint64_t epoch) = 0;
    // The caret sits right after the sealed text, with nothing selected.
    virtual bool caret_after_sealed(DocumentId document, std::uint64_t epoch) = 0;
};

class TrainingSink {
public:
    virtual ~TrainingSink() = default;
    virtual void record_commit(DocumentId document, std::uint64_t epoch, CommitSample sample) = 0;
};

class CompositionHost : public DocumentEditor,
                        public CompositionView,
                        public SealedTextStore,
                        public TrainingSink {};

}  // namespace tsf
