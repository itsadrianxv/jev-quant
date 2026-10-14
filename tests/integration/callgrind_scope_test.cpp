#include "trading/performance/measurement.h"
#include <atomic>
#include <thread>

// A collection-semantics test, not a performance benchmark. The background
// marker must receive zero collected instructions while the owner is enabled.
__attribute__((noinline)) void excludedMarker() {
    volatile int value = 0;
    for (int i = 0; i < 777; ++i) { value = i * 2; }
    (void)value;
}
__attribute__((noinline)) void includedMarker() {
    volatile int value = 0;
    for (int i = 0; i < 1000; ++i) { value = i; }
    (void)value;
}
int main() {
    Trading::Measurement measurement(1, 60000000000ULL, 1000000000ULL, true);
    measurement.arm();
    std::atomic<bool> enabled{false}, done{false};
    std::jthread background([&] {
        while (!enabled.load()) std::this_thread::yield();
        excludedMarker();
        done.store(true);
    });
    {
        Trading::InstructionScope scope(&measurement);
        enabled.store(true);
        while (!done.load()) std::this_thread::yield();
        includedMarker();
    }
    background.join();
    excludedMarker();
}
