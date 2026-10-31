#include "core/composition_controller.hpp"

#include <algorithm>

#include "core/training.hpp"

namespace tsf {

bool CompositionController::Composition::settled() const {
    if (!buffer.ready_to_commit()) return false;
    const bool needs_backend = !std::ranges::all_of(buffer.entries(), &BopomofoPos::chosen);
    return !needs_backend || converted == buffer.revision();
}

Rendering CompositionController::Composition::rendering() const {
    return {.text = buffer.to_string(), .caret = buffer.caret_offset(), .selection = buffer.current_invalid_span()};
}

void CompositionController::set_immediate_conversion(bool enabled) noexcept {
    immediate_conversion_ = enabled;
    if (composition_) composition_->buffer.set_immediate_conversion(enabled);
}

const CompositionBuffer& CompositionController::buffer() const noexcept {
    static const CompositionBuffer empty;
    return composition_ ? composition_->buffer : empty;
}

CompositionController::Composition* CompositionController::open(DocumentId document, bool prefill) {
    if (composition_) return live(document);
    auto& composition = composition_.emplace(Composition{.epoch = next_epoch_++, .document = document});
    composition.buffer.set_immediate_conversion(immediate_conversion_);
    // Text before the composition cannot change while it is open, so it is
    // read once and reused by every conversion of this composition. Sealed
    // text right before it counts as what it is going to become.
    host_.run_edit(document, [this, document, epoch = composition.epoch, prefill] {
        auto preceding = host_.preceding_text(document, epoch);
        if (auto context = sealed_.context_at_caret(document)) preceding = std::move(*context);
        if (prefill) conversions_.prefill(preceding);
        context_ready(epoch, std::move(preceding));
    });
    return composition_ ? &*composition_ : nullptr;
}

void CompositionController::context_ready(std::uint64_t epoch, std::u16string preceding) {
    if (auto* composition = live_epoch(epoch)) {
        composition->preceding = std::move(preceding);
        request(*composition);
    } else {
        sealed_.context_ready(epoch, std::move(preceding));
    }
}

// A render shows the state as of when it runs: renders made inside edits may
// already have shown newer state. Only an ended composition is shown as it
// was, which is how it ended.
void CompositionController::show(const Composition& composition, std::optional<E2eTrace> trace) {
    host_.run_edit(composition.document, [this, document = composition.document, epoch = composition.epoch,
                                          ended = composition.rendering(), trace] {
        const auto* live = live_epoch(epoch);
        host_.render(document, epoch, live ? live->rendering() : ended, trace ? &*trace : nullptr);
    });
}

void CompositionController::update(Composition& composition, E2eTrace trace) {
    if (composition.buffer.empty()) {
        discard();
        return;
    }
    show(composition);
    request(composition, trace);
}

void CompositionController::request(Composition& composition, E2eTrace trace) {
    if (!composition.preceding) return;
    auto snapshot = composition.buffer.conversion_snapshot();
    if (!snapshot || (composition.sent && composition.sent->revision == snapshot->revision)) return;
    trace.predict_started = std::chrono::steady_clock::now();
    const auto revision = snapshot->revision;
    const auto sequence = conversions_.send({.epoch = composition.epoch,
                                             .revision = revision,
                                             .document = composition.document,
                                             .snapshot = std::move(snapshot->entries),
                                             .trace = trace},
                                            *composition.preceding);
    if (sequence) composition.sent = Sent{revision, *sequence};
}

void CompositionController::type_reading(DocumentId document, char16_t symbol, E2eTrace trace) {
    auto* composition = open(document, true);
    if (!composition) return;
    composition->buffer.add(symbol);
    update(*composition, trace);
}

void CompositionController::type_text(DocumentId document, std::u16string_view text, E2eTrace trace) {
    if (text.empty()) return;
    auto* composition = open(document, false);
    if (!composition) return;
    for (const char16_t ch : text) composition->buffer.add_chosen_candidate(ch);
    update(*composition, trace);
}

bool CompositionController::discard_reading(DocumentId document) {
    auto* composition = live(document);
    if (!composition || !composition->buffer.discard_current_reading()) return false;
    update(*composition);
    return true;
}

void CompositionController::backspace(DocumentId document, E2eTrace trace) {
    if (auto* composition = live(document); composition && composition->buffer.remove_last()) {
        update(*composition, trace);
    }
}

void CompositionController::move_caret(DocumentId document, bool forward) {
    auto* composition = live(document);
    if (!composition) return;
    auto& buffer = composition->buffer;
    if (forward ? buffer.next() : buffer.pre()) show(*composition);
}

void CompositionController::enter(DocumentId document, E2eTrace trace) {
    auto* composition = live(document);
    if (!composition || composition->buffer.has_uncomposed_reading()) return;
    if (composition->settled()) {
        commit();
        return;
    }
    request(*composition, trace);
    seal();
}

void CompositionController::escape(DocumentId document) {
    if (owns(document)) discard();
}

void CompositionController::backspace_in_order(DocumentId document) {
    if (owns(document)) return;
    host_.run_edit(document, [this, document] {
        if (!sealed_.erase_at_caret(document)) host_.erase_before_caret(document);
    });
}

void CompositionController::candidates_opened(std::uint64_t entry) {
    if (composition_) composition_->candidates.open(entry);
}

void CompositionController::select_candidate(std::uint64_t entry, std::size_t index) {
    if (composition_) composition_->buffer.select_candidate(entry, index);
}

void CompositionController::candidate_finalized(DocumentId document, E2eTrace trace) {
    auto* composition = live(document);
    if (!composition) return;
    composition->buffer.next();
    update(*composition, trace);
}

void CompositionController::candidates_closed(DocumentId document) {
    auto* composition = live(document);
    if (!composition) return;
    auto held = composition->candidates.close();
    if (!held) return;
    const auto outcome = composition->buffer.merge_conversion(held->sequence, {std::move(held->requested)},
                                                              {std::move(held->converted)});
    if (outcome.changed) show(*composition);
}

void CompositionController::on_conversion(ConversionResult result) {
    auto request = conversions_.take(result.sequence);
    if (!request) return;
    switch (result.status) {
        case ConversionResult::Status::Superseded:
            return;
        case ConversionResult::Status::BackendLost:
            backend_lost();
            return;
        case ConversionResult::Status::Converted:
            break;
    }
    request->trace.predict_finished = result.finished;
    if (!live_epoch(request->epoch)) {
        sealed_.on_conversion(request->epoch, result.sequence, request->snapshot, std::move(result.padding));
        return;
    }
    // Merged in order with the edits queued before it, so what is shown
    // changes together with the state.
    host_.run_edit(request->document, [this, request = std::move(*request), sequence = result.sequence,
                                       converted = std::move(result.padding)]() mutable {
        if (auto* composition = live_epoch(request.epoch)) {
            apply(*composition, sequence, request, std::move(converted));
        } else {
            sealed_.on_conversion(request.epoch, sequence, request.snapshot, std::move(converted));
        }
    });
}

// In an edit. The merge only touches syllables that are still exactly what
// was sent.
void CompositionController::apply(Composition& composition, std::uint64_t sequence,
                                  const ConversionTracker::Request& request, std::vector<BopomofoPos> converted) {
    auto outcome = composition.buffer.merge_conversion(sequence, request.snapshot, std::move(converted),
                                                       composition.candidates.entry());
    if (outcome.held) {
        composition.candidates.hold(sequence, std::move(outcome.held->first), std::move(outcome.held->second));
    }
    if (request.revision == composition.buffer.revision()) composition.converted = request.revision;
    if (outcome.changed) {
        host_.render(composition.document, composition.epoch, composition.rendering(), &request.trace);
    }
}

// The state ends first: the queued edit may run right away.
void CompositionController::commit() {
    const auto document = composition_->document;
    const auto epoch = composition_->epoch;
    auto sample = training_sample(composition_->buffer, composition_->preceding.value_or(std::u16string{}));
    composition_.reset();
    host_.run_edit(document, [this, document, epoch, sample = std::move(sample)]() mutable {
        host_.commit(document, epoch);
        if (sample) host_.record_commit(document, epoch, std::move(*sample));
    });
}

void CompositionController::discard() {
    const auto document = composition_->document;
    const auto epoch = composition_->epoch;
    composition_.reset();
    host_.run_edit(document, [this, document, epoch] { host_.discard(document, epoch); });
}

void CompositionController::seal() {
    if (!composition_) return;
    auto& composition = *composition_;
    const bool frozen = composition.buffer.freeze_readings();
    if (composition.settled()) {
        commit();
        return;
    }
    // A conversion already in flight for exactly this content finishes it.
    std::optional<std::uint64_t> in_flight;
    if (const auto& sent = composition.sent; !frozen && sent && sent->revision == composition.buffer.revision() &&
                                             conversions_.pending(sent->sequence)) {
        in_flight = sent->sequence;
    }
    auto sealed = std::move(composition);
    composition_.reset();
    sealed_.add(sealed.epoch, sealed.document, std::move(sealed.buffer), std::move(sealed.preceding), in_flight);
    host_.run_edit(sealed.document, [this, document = sealed.document, epoch = sealed.epoch] {
        host_.seal(document, epoch);
        sealed_.placed(epoch);
    });
}

// Nothing can convert the syllables still waiting, so the local table is the
// last resort for them.
void CompositionController::backend_lost() {
    conversions_.clear();
    sealed_.finish_locally();
    if (!composition_) return;
    composition_->sent.reset();
    // The buffer changes together with what the document shows, so a commit
    // in between never takes text that is not on screen.
    host_.run_edit(composition_->document, [this, epoch = composition_->epoch] {
        auto* composition = live_epoch(epoch);
        if (!composition) return;
        composition->converted = composition->buffer.revision();
        if (composition->buffer.accept_local_candidates()) {
            host_.render(composition->document, composition->epoch, composition->rendering(), nullptr);
        }
    });
}

void CompositionController::document_closed(DocumentId document) {
    if (owns(document)) composition_.reset();
    sealed_.forget(document);
}

void CompositionController::shutdown() {
    conversions_.set_backend(nullptr);
    conversions_.clear();
    seal();
    sealed_.finish_locally();
}

}  // namespace tsf
