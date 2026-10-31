#pragma once

#include <cstddef>
#include <cstdint>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

#include "core/buffer.hpp"
#include "core/candidate_lock.hpp"
#include "core/composition_host.hpp"
#include "core/conversion.hpp"
#include "core/e2e_trace.hpp"
#include "core/sealed_text.hpp"

namespace tsf {

// The composition state machine, free of TSF. Every call happens on the
// owning UI thread; conversions return through on_conversion.
//
// - Typing never waits: each edit renders at once and sends the complete
//   composition for conversion.
// - A result is merged into every syllable that is still the one it was
//   computed for, so continued typing never wastes it.
// - Enter commits at once when what is shown is the conversion of exactly
//   this content. Otherwise it seals the composition (see SealedQueue), and
//   further typing already starts the next composition.
// - A composition forced out of its document is sealed the same way.
// - Only a lost backend falls back to the local table.
class CompositionController {
public:
    explicit CompositionController(CompositionHost& host) : host_(host), sealed_(host, host, host, conversions_) {}

    CompositionController(const CompositionController&) = delete;
    CompositionController& operator=(const CompositionController&) = delete;

    void set_backend(ConversionBackend* backend) noexcept { conversions_.set_backend(backend); }
    void set_immediate_conversion(bool enabled) noexcept;

    bool active() const noexcept { return composition_.has_value(); }
    bool owns(DocumentId document) const noexcept { return composition_ && composition_->document == document; }
    // The open composition's epoch, or the one the next composition gets.
    std::uint64_t epoch() const noexcept { return composition_ ? composition_->epoch : next_epoch_; }
    const CompositionBuffer& buffer() const noexcept;
    std::size_t sealed_count() const noexcept { return sealed_.size(); }
    std::size_t outstanding_conversions() const noexcept { return conversions_.size(); }

    // Key input for document. A composition still open in another document
    // must be sealed by the caller first.
    void type_reading(DocumentId document, char16_t symbol, E2eTrace trace = {});
    void type_text(DocumentId document, std::u16string_view text, E2eTrace trace = {});
    bool discard_reading(DocumentId document);
    void backspace(DocumentId document, E2eTrace trace = {});
    void move_caret(DocumentId document, bool forward);
    void enter(DocumentId document, E2eTrace trace = {});
    void escape(DocumentId document);
    // Backspace outside a composition. Returns false when the key belongs to
    // the document.
    bool erase_sealed(DocumentId document);
    bool sealed_at_caret(DocumentId document) { return !owns(document) && sealed_.at_caret(document); }

    void candidates_opened(std::uint64_t entry);
    void select_candidate(std::uint64_t entry, std::size_t index);
    // After a selection: step past it and convert the rest around it.
    void candidate_finalized(DocumentId document, E2eTrace trace = {});
    // Merges a result held back while the list was open.
    void candidates_closed(DocumentId document);

    void on_conversion(ConversionResult result);
    // Seals the open composition in its document (Enter while converting,
    // focus moved, host ended it).
    void seal();
    void document_closed(DocumentId document);
    // Forgets the composition without touching the document.
    void reset() noexcept { composition_.reset(); }
    // No backend from now on: finishes everything with the local table.
    void shutdown();

private:
    struct Sent {
        std::uint64_t revision = 0;
        std::uint64_t sequence = 0;
    };

    struct Composition {
        std::uint64_t epoch = 0;
        DocumentId document = 0;
        std::u16string preceding;
        CompositionBuffer buffer;
        CandidateLock candidates;
        // The newest conversion sent.
        std::optional<Sent> sent;
        // The content revision whose conversion is shown.
        std::optional<std::uint64_t> converted;

        // What is shown is final for exactly the current content.
        bool settled() const;
        Rendering rendering() const;
    };

    Composition* live(DocumentId document) { return owns(document) ? &*composition_ : nullptr; }
    Composition* live_epoch(std::uint64_t epoch) {
        return composition_ && composition_->epoch == epoch ? &*composition_ : nullptr;
    }
    Composition* open(DocumentId document, bool prefill);
    void update(Composition& composition, E2eTrace trace = {});
    void request(Composition& composition, E2eTrace trace = {});
    void apply(Composition& composition, std::uint64_t sequence, const ConversionTracker::Request& request,
               std::vector<BopomofoPos> converted);
    void commit();
    void backend_lost();

    CompositionHost& host_;
    ConversionTracker conversions_;
    SealedQueue sealed_;
    std::optional<Composition> composition_;
    std::uint64_t next_epoch_ = 1;
    bool immediate_conversion_ = false;
};

}  // namespace tsf
