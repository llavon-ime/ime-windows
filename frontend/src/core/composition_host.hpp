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

class DocumentEditor {
public:
    virtual ~DocumentEditor() = default;
    // Runs edit once the document can be written, possibly right away.
    // Returns false when it never will.
    virtual bool run_edit(DocumentId document, std::function<void()> edit) = 0;
};

// The open composition. Called during key handling or inside run_edit.
class CompositionView {
public:
    virtual ~CompositionView() = default;
    virtual std::u16string preceding_text(DocumentId document) = 0;
    // Shows the composition, starting it if needed.
    virtual void render(DocumentId document, const Rendering& rendering, const E2eTrace* trace) = 0;
    // The shown text is final: ends the composition in place.
    virtual void commit(DocumentId document) = 0;
    // Removes the composition together with its text.
    virtual void discard(DocumentId document) = 0;
    // Ends the composition but leaves its text, tracked as sealed text under
    // epoch; the caret stays right after it.
    virtual void seal(DocumentId document, std::uint64_t epoch) = 0;
};

// Sealed text in its document. Reads and writes happen inside run_edit.
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
