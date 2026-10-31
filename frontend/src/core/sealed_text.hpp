#pragma once

#include <cstddef>
#include <cstdint>
#include <optional>
#include <string>
#include <variant>
#include <vector>

#include "core/buffer.hpp"
#include "core/composition_host.hpp"
#include "core/conversion.hpp"
#include "engine/engine_types.hpp"

namespace tsf {

// Text a composition left in its document before its conversion was done.
// It becomes final when the awaited conversion lands; Backspace typed right
// after it erases from the end of that final text.
class SealedSegment {
public:
    // The text before it is unknown until the document has been read.
    struct WaitingForContext {};
    struct Converting {
        std::uint64_t awaited = 0;
    };
    struct Finished {};
    using Stage = std::variant<WaitingForContext, Converting, Finished>;

    SealedSegment(std::uint64_t epoch, DocumentId document, CompositionBuffer buffer,
                  std::optional<std::u16string> preceding);

    std::uint64_t epoch() const noexcept { return epoch_; }
    DocumentId document() const noexcept { return document_; }
    const std::u16string& shown() const noexcept { return shown_; }
    const CompositionBuffer& buffer() const noexcept { return buffer_; }
    const std::optional<std::u16string>& preceding() const noexcept { return preceding_; }
    bool finished() const noexcept { return std::holds_alternative<Finished>(stage_); }
    bool in_document() const noexcept { return placement_ == Placement::InDocument; }
    void placed() noexcept { placement_ = Placement::InDocument; }
    bool waiting_for_context() const noexcept { return std::holds_alternative<WaitingForContext>(stage_); }

    void await(std::uint64_t sequence) noexcept { stage_ = Converting{sequence}; }
    void set_preceding(std::u16string preceding) { preceding_ = std::move(preceding); }
    void merge(std::uint64_t sequence, const std::vector<BopomofoPos>& requested, std::vector<BopomofoPos> converted);
    void finish_locally();
    void erase_last();

    // What it becomes, minus what was erased.
    std::u16string text() const;
    // Erased characters beyond its own start.
    std::size_t erases_before() const noexcept;
    // The text a composition typed right after it continues from.
    std::u16string following_context() const;
    std::optional<CommitSample> training_sample() const;

private:
    // The state is ahead of the document: the edit that leaves the text in
    // the document may still be queued.
    enum class Placement { Queued, InDocument };

    std::uint64_t epoch_;
    DocumentId document_;
    CompositionBuffer buffer_;
    std::u16string shown_;
    std::optional<std::u16string> preceding_;
    std::size_t erased_ = 0;
    Stage stage_;
    Placement placement_ = Placement::Queued;
};

// The sealed segments of all documents. Each is written back once finished;
// one that erases into the text before it waits for the segments before it.
// Methods marked "in an edit" run inside a DocumentEditor::run_edit callback.
class SealedQueue {
public:
    SealedQueue(DocumentEditor& editor, SealedTextStore& store, TrainingSink& training,
                ConversionTracker& conversions)
        : editor_(editor), store_(store), training_(training), conversions_(conversions) {}

    SealedQueue(const SealedQueue&) = delete;
    SealedQueue& operator=(const SealedQueue&) = delete;

    // Converted by in_flight when given, else by a new request as soon as
    // the text before it is known.
    void add(std::uint64_t epoch, DocumentId document, CompositionBuffer buffer,
             std::optional<std::u16string> preceding, std::optional<std::uint64_t> in_flight);
    void context_ready(std::uint64_t epoch, std::u16string preceding);
    // In an edit: the document now holds the sealed text.
    void placed(std::uint64_t epoch);
    void on_conversion(std::uint64_t epoch, std::uint64_t sequence, const std::vector<BopomofoPos>& requested,
                       std::vector<BopomofoPos> converted);
    // In an edit: Backspace outside a composition. False when no sealed text
    // is at the caret.
    bool erase_at_caret(DocumentId document);
    // In an edit.
    std::optional<std::u16string> context_at_caret(DocumentId document);
    bool pending(DocumentId document) const noexcept;
    // No conversion will come any more.
    void finish_locally();
    void forget(DocumentId document);
    std::size_t size() const noexcept { return segments_.size(); }

private:
    SealedSegment* find(std::uint64_t epoch);
    SealedSegment* newest_at_caret(DocumentId document);
    void convert(SealedSegment& segment);
    void write_when_possible(DocumentId document);
    void write_finished(DocumentId document);
    void write(const SealedSegment& segment);

    DocumentEditor& editor_;
    SealedTextStore& store_;
    TrainingSink& training_;
    ConversionTracker& conversions_;
    // Oldest first.
    std::vector<SealedSegment> segments_;
};

}  // namespace tsf
