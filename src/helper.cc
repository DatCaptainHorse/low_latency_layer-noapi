#include "helper.hh"

#include <thread>

namespace low_latency {

void precise_wait_until(const DeviceClock::time_point& target) {
    using namespace std::chrono;

    // Roughly the worst case the scheduler will overshoot a sleep by. Sleeping
    // any closer to the target than this risks missing it outright, which for
    // deadline pacing costs a whole refresh.
    constexpr auto SPIN_WINDOW = microseconds{300};

    if (const auto sleep_until = target - SPIN_WINDOW;
        DeviceClock::now() < sleep_until) {

        // DeviceClock and steady_clock are both CLOCK_MONOTONIC underneath but
        // are deliberately distinct types, so convert through a relative
        // duration rather than pretending the time points are interchangeable.
        const auto remaining = sleep_until - DeviceClock::now();
        if (remaining > DeviceClock::duration::zero()) {
            std::this_thread::sleep_for(remaining);
        }
    }

    while (DeviceClock::now() < target) {
        std::this_thread::yield();
    }
}


} // namespace low_latency
