#include "service/gpu_activity_lease.hpp"

#include <stdexcept>
#include <vector>

using llavon::service::GpuActivityLease;
using namespace std::chrono_literals;

int main() {
    // Repeated keys renew one lease; stopping the service releases it even if
    // its io_context was stopped before the idle timer could run.
    asio::io_context context;
    std::vector<bool> calls;
    const auto backend = [&](bool enabled) { calls.push_back(enabled); return true; };
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
        lease.set_backend([&](bool enabled) { replacement.push_back(enabled); return true; });
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
        lease.set_backend([&](bool) { ++attempts; return false; });
        lease.activate();
        lease.activate();
        if (attempts != 1) return 7;
        lease.set_backend([](bool) -> bool { throw std::runtime_error("driver failure"); });
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
}
