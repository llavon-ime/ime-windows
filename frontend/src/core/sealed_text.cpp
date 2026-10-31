#include "core/sealed_text.hpp"

#include <algorithm>
#include <ranges>

#include "core/training.hpp"

namespace tsf {

namespace {

void drop_last_characters(std::u16string& text, std::size_t count) {
    for (; count > 0 && !text.empty(); --count) {
        text.pop_back();
        if (!text.empty() && text.back() >= 0xD800 && text.back() <= 0xDBFF) text.pop_back();
    }
}

}  // namespace

SealedSegment::SealedSegment(std::uint64_t epoch, DocumentId document, CompositionBuffer buffer,
                             std::u16string preceding, std::optional<std::uint64_t> awaited)
    : epoch_(epoch),
      document_(document),
      buffer_(std::move(buffer)),
      preceding_(std::move(preceding)),
      stage_(Finished{}) {
    shown_ = buffer_.to_string();
    if (awaited) {
        stage_ = Converting{*awaited};
    } else {
        finish_locally();
    }
}

void SealedSegment::merge(std::uint64_t sequence, const std::vector<BopomofoPos>& requested,
                          std::vector<BopomofoPos> converted) {
    buffer_.merge_conversion(sequence, requested, std::move(converted));
    // Older results only refine it; the awaited one makes it final.
    if (const auto* converting = std::get_if<Converting>(&stage_); converting && sequence >= converting->awaited) {
        stage_ = Finished{};
    }
}

void SealedSegment::finish_locally() {
    buffer_.accept_local_candidates();
    stage_ = Finished{};
}

void SealedSegment::erase_last() {
    ++erased_;
    if (erased_ >= buffer_.entries().size()) stage_ = Finished{};
}

std::u16string SealedSegment::text() const {
    const auto& entries = buffer_.entries();
    const auto kept = entries.size() - std::min(erased_, entries.size());
    std::u16string result;
    for (const auto& entry : entries | std::views::take(kept)) result += entry.current();
    return result;
}

std::size_t SealedSegment::erases_before() const noexcept {
    const auto length = buffer_.entries().size();
    return erased_ > length ? erased_ - length : 0;
}

std::u16string SealedSegment::following_context() const {
    auto context = preceding_;
    drop_last_characters(context, erases_before());
    return context + text();
}

std::optional<CommitSample> SealedSegment::training_sample() const {
    if (erased_ > 0) return std::nullopt;
    return tsf::training_sample(buffer_, preceding_);
}

void SealedQueue::add(std::uint64_t epoch, DocumentId document, CompositionBuffer buffer, std::u16string preceding,
                      std::optional<std::uint64_t> in_flight) {
    if (!in_flight) {
        if (auto snapshot = buffer.conversion_snapshot()) {
            in_flight = conversions_.send({.epoch = epoch,
                                           .revision = snapshot->revision,
                                           .document = document,
                                           .snapshot = std::move(snapshot->entries)},
                                          preceding);
        }
    }
    const auto& segment =
        segments_.emplace_back(epoch, document, std::move(buffer), std::move(preceding), in_flight);
    if (segment.finished()) write_when_possible(document);
}

void SealedQueue::on_conversion(std::uint64_t epoch, std::uint64_t sequence,
                                const std::vector<BopomofoPos>& requested, std::vector<BopomofoPos> converted) {
    auto* segment = find(epoch);
    if (!segment) return;
    segment->merge(sequence, requested, std::move(converted));
    if (segment->finished()) write_when_possible(segment->document());
}

bool SealedQueue::erase_at_caret(DocumentId document) {
    auto* segment = newest_at_caret(document);
    if (!segment) return false;
    segment->erase_last();
    if (segment->finished()) write_when_possible(document);
    return true;
}

std::optional<std::u16string> SealedQueue::context_at_caret(DocumentId document) {
    const auto* segment = newest_at_caret(document);
    if (!segment) return std::nullopt;
    return segment->following_context();
}

void SealedQueue::finish_locally() {
    std::vector<DocumentId> documents;
    for (auto& segment : segments_) {
        segment.finish_locally();
        if (!std::ranges::contains(documents, segment.document())) documents.push_back(segment.document());
    }
    for (const auto document : documents) write_when_possible(document);
}

void SealedQueue::forget(DocumentId document) {
    for (const auto& segment : segments_) {
        if (segment.document() == document) store_.release_sealed(document, segment.epoch());
    }
    std::erase_if(segments_, [document](const SealedSegment& segment) { return segment.document() == document; });
}

SealedSegment* SealedQueue::find(std::uint64_t epoch) {
    const auto found = std::ranges::find(segments_, epoch, &SealedSegment::epoch);
    return found == segments_.end() ? nullptr : &*found;
}

SealedSegment* SealedQueue::newest_at_caret(DocumentId document) {
    const auto newest = std::ranges::find_last(segments_, document, &SealedSegment::document);
    if (newest.empty() || !store_.caret_after_sealed(document, newest.begin()->epoch())) return nullptr;
    return &*newest.begin();
}

// Every request writes whatever is finished by the time the edit runs, so a
// request that finds nothing left to do is harmless.
void SealedQueue::write_when_possible(DocumentId document) {
    if (!editor_.run_edit(document, [this, document] { write_finished(document); })) forget(document);
}

void SealedQueue::write_finished(DocumentId document) {
    bool earlier_waiting = false;
    for (auto segment = segments_.begin(); segment != segments_.end();) {
        if (segment->document() != document) {
            ++segment;
            continue;
        }
        if (segment->finished() && !(earlier_waiting && segment->erases_before() > 0)) {
            write(*segment);
            segment = segments_.erase(segment);
            continue;
        }
        earlier_waiting = true;
        ++segment;
    }
}

// The text is replaced only if the user has not changed it meanwhile.
void SealedQueue::write(const SealedSegment& segment) {
    const auto document = segment.document();
    const auto epoch = segment.epoch();
    if (store_.sealed_text(document, epoch) == segment.shown()) {
        store_.write_sealed(document, epoch, segment.text());
        if (const auto count = segment.erases_before()) store_.erase_before_sealed(document, epoch, count);
        if (auto sample = segment.training_sample()) training_.record_commit(document, epoch, std::move(*sample));
    }
    store_.release_sealed(document, epoch);
}

}  // namespace tsf
