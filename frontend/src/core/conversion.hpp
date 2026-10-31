#pragma once

#include <chrono>
#include <cstddef>
#include <cstdint>
#include <map>
#include <optional>
#include <string>
#include <utility>
#include <vector>

#include "core/bopomofo.hpp"
#include "core/composition_host.hpp"
#include "core/e2e_trace.hpp"

namespace tsf {

struct ConversionResult {
    enum class Status {
        Converted,
        // A newer request for the same composition replaced it unsent.
        Superseded,
        // The backend is gone; input has already switched to English.
        BackendLost,
    };
    std::uint64_t sequence = 0;
    Status status = Status::Superseded;
    std::vector<BopomofoPos> padding;
    std::chrono::steady_clock::time_point finished;
};

class ConversionBackend {
public:
    virtual ~ConversionBackend() = default;
    virtual void prefill(std::u16string context) = 0;
    // Answers with a ConversionResult. A queued request is superseded by the
    // next one of the same group.
    virtual void predict(std::uint64_t sequence, std::uint64_t group, std::u16string context,
                         std::vector<BopomofoPos> entries) = 0;
};

// Conversions sent and not answered yet, and what each was sent for.
class ConversionTracker {
public:
    struct Request {
        // The composition it converts; also its group at the backend.
        std::uint64_t epoch = 0;
        std::uint64_t revision = 0;
        DocumentId document = 0;
        std::vector<BopomofoPos> snapshot;
        E2eTrace trace;
    };

    void set_backend(ConversionBackend* backend) noexcept { backend_ = backend; }

    void prefill(std::u16string context) {
        if (backend_) backend_->prefill(std::move(context));
    }

    // The request's sequence, or nothing without a backend.
    std::optional<std::uint64_t> send(Request request, std::u16string context) {
        if (!backend_) return std::nullopt;
        const auto sequence = ++sequence_;
        const auto group = request.epoch;
        auto entries = request.snapshot;
        pending_.emplace(sequence, std::move(request));
        backend_->predict(sequence, group, std::move(context), std::move(entries));
        return sequence;
    }

    std::optional<Request> take(std::uint64_t sequence) {
        auto node = pending_.extract(sequence);
        if (node.empty()) return std::nullopt;
        return std::move(node.mapped());
    }

    bool pending(std::uint64_t sequence) const { return pending_.contains(sequence); }
    std::size_t size() const noexcept { return pending_.size(); }
    void clear() noexcept { pending_.clear(); }

private:
    ConversionBackend* backend_ = nullptr;
    std::uint64_t sequence_ = 0;
    std::map<std::uint64_t, Request> pending_;
};

}  // namespace tsf
