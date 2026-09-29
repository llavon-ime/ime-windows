#include "service/gpu_activity_lease.hpp"
#include "service/amd_gpu_pci_address.hpp"

#include <stdexcept>
#include <vector>

using llavon::service::GpuActivityLease;
using Result = GpuActivityLease::Result;
using llavon::service::amd_pci_address;
using llavon::service::AmdPciAddress;
using namespace std::chrono_literals;

static_assert(amd_pci_address("0000:65:00.0").value() == AmdPciAddress{101, 0, 0});
static_assert(amd_pci_address("0000:ff:1f.7").value() == AmdPciAddress{255, 31, 7});
static_assert(!amd_pci_address("0001:65:00.0"));
static_assert(!amd_pci_address("0000:100:00.0"));
static_assert(!amd_pci_address("0000:65:20.0"));
static_assert(!amd_pci_address("0000:65:00.8"));
static_assert(!amd_pci_address("0000:65:00.0extra"));
static_assert(!amd_pci_address("0000::00.0"));
static_assert(!amd_pci_address("0000:65:00"));
static_assert(!amd_pci_address("0000:gg:00.0"));
static_assert(!amd_pci_address(""));
int main() {
    // Repeated keys renew one lease; stopping the service releases it even if
    // its io_context was stopped before the idle timer could run.
    asio::io_context context;
    std::vector<bool> calls;
    const auto backend = [&](bool enabled) { calls.push_back(enabled); return Result::success; };
    {
        GpuActivityLease lease(context, 1h);
        lease.set_backend(backend);
        for (int key = 0; key < 100; ++key) lease.activate();
        if (calls != std::vector<bool>{true}) return 1;
        context.stop();
    }
    if (calls != std::vector<bool>{true, false}) return 2;

    // Expiry does not discard a healthy backend: the next input can boost
    // again. Zero-duration timers keep this deterministic on busy CI hosts.
    context.restart();
    calls.clear();
    {
        GpuActivityLease lease(context, 0ms);
        lease.set_backend(backend);
        lease.activate();
        context.run();
        if (calls != std::vector<bool>{true, false}) return 3;
        context.restart();
        lease.activate();
        context.run();
    }
    if (calls != std::vector<bool>{true, false, true, false}) return 4;

    // Model/device replacement releases the old device. Its cancelled timer
    // must not disable the new device's lease.
    context.restart();
    calls.clear();
    std::vector<bool> replacement;
    {
        GpuActivityLease lease(context, 1h);
        lease.set_backend(backend);
        lease.activate();
        lease.set_backend([&](bool enabled) { replacement.push_back(enabled); return Result::success; });
        lease.activate();
        context.poll();
        if (calls != std::vector<bool>{true, false} || replacement != std::vector<bool>{true}) return 5;
    }
    if (replacement != std::vector<bool>{true, false}) return 6;

    // Unsupported/failed drivers are optional and are not retried per key.
    context.restart();
    int attempts = 0;
    {
        GpuActivityLease lease(context, 0ms);
        lease.activate();
        lease.set_backend([&](bool) { ++attempts; return Result::unavailable; });
        lease.activate();
        lease.activate();
        if (attempts != 1) return 7;
        lease.set_backend([](bool) -> Result { throw std::runtime_error("driver failure"); });
        lease.activate();
        context.run();
    }

    // If a timer callback is queued when the owner is destroyed, draining the
    // executor must not call the released backend or access its old owner.
    context.restart();
    calls.clear();
    {
        GpuActivityLease lease(context, 0ms);
        lease.set_backend(backend);
        lease.activate();
    }
    context.run();
    if (calls != std::vector<bool>{true, false}) return 8;

    // Turning the setting off keeps the current lease until expiry and blocks
    // new activations. Turning it back on allows the next activation.
    context.restart();
    calls.clear();
    {
        GpuActivityLease lease(context, 0ms);
        lease.set_backend(backend);
        lease.activate();
        lease.set_enabled(false);
        lease.activate();
        if (calls != std::vector<bool>{true}) return 9;
        context.run();
        if (calls != std::vector<bool>{true, false}) return 10;
        context.restart();
        lease.activate();
        if (calls != std::vector<bool>{true, false}) return 11;
        lease.set_enabled(true);
        lease.activate();
        context.run();
    }
    if (calls != std::vector<bool>{true, false, true, false}) return 12;

    // Transient enable failures retain the backend and rate-limit keystrokes.
    context.restart();
    attempts = 0;
    {
        GpuActivityLease lease(context, 0ms, 1h);
        lease.set_backend([&](bool) { ++attempts; return Result::retry_later; });
        for (int key = 0; key < 100; ++key) lease.activate();
        if (attempts != 1) return 13;
    }
    if (attempts != 1) return 14; // Failed activation owns no clock change.
    context.restart();
    calls.clear();
    {
        GpuActivityLease lease(context, 0ms, 0ms);
        lease.set_backend([&](bool enabled) {
            calls.push_back(enabled);
            return calls.size() == 1 ? Result::retry_later : Result::success;
        });
        lease.activate();
        lease.activate();
        context.run();
    }
    if (calls != std::vector<bool>{true, true, false}) return 15;

    // Release is retried without re-enabling or losing the original snapshot.
    context.restart();
    calls.clear();
    {
        GpuActivityLease lease(context, 0ms, 0ms);
        lease.set_backend([&](bool enabled) {
            calls.push_back(enabled);
            return !enabled && calls.size() == 2 ? Result::retry_later : Result::success;
        });
        lease.activate();
        if (context.run_one() != 1) return 16;
        lease.activate(); // Input must not postpone a pending restoration.
        lease.set_enabled(false);
        context.run();
        if (calls != std::vector<bool>{true, false, false}) return 17;
        context.restart();
        lease.set_enabled(true);
        lease.activate();
        context.run();
    }
    if (calls != std::vector<bool>{true, false, false, true, false}) return 18;

    // Exhausting retries suspends boosts but retains state for shutdown.
    context.restart();
    calls.clear();
    auto retained = std::make_shared<int>(0);
    const std::weak_ptr<int> weak_retained = retained;
    {
        GpuActivityLease lease(context, 0ms, 0ms);
        lease.set_backend([&, saved = std::move(retained)](bool enabled) {
            ++*saved;
            calls.push_back(enabled);
            return enabled ? Result::success : Result::retry_later;
        });
        lease.activate();
        context.run();
        for (int key = 0; key < 100; ++key) lease.activate();
        if (calls != std::vector<bool>{true, false, false, false, false}) return 19;
        if (weak_retained.expired()) return 20;
    }
    if (!weak_retained.expired() || calls.size() != 6 || calls.back()) return 21;

    // Throwing during restoration follows the same bounded retry path.
    context.restart();
    calls.clear();
    {
        GpuActivityLease lease(context, 0ms, 0ms);
        lease.set_backend([&](bool enabled) {
            calls.push_back(enabled);
            if (!enabled && calls.size() == 2) throw std::runtime_error("transient release");
            return Result::success;
        });
        lease.activate();
        context.run();
    }
    if (calls != std::vector<bool>{true, false, false}) return 22;

    // Replacing a backend during its retry cancels the old retry callback.
    context.restart();
    calls.clear();
    replacement.clear();
    {
        GpuActivityLease lease(context, 0ms, 0ms);
        lease.set_backend([&](bool enabled) {
            calls.push_back(enabled);
            return !enabled && calls.size() == 2 ? Result::retry_later : Result::success;
        });
        lease.activate();
        if (context.run_one() != 1) return 23;
        lease.set_backend([&](bool enabled) { replacement.push_back(enabled); return Result::success; });
        lease.activate();
        context.run();
    }
    if (calls != std::vector<bool>{true, false, false} ||
        replacement != std::vector<bool>{true, false}) return 24;
}
