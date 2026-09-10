#ifndef HELPER_HH_
#define HELPER_HH_

#include "device_clock.hh"

#include <vulkan/vk_layer.h>
#include <vulkan/vulkan.h>

namespace low_latency {

#define THROW_NOT_VKSUCCESS(x)                                                 \
    do {                                                                       \
        if (const auto result = x; result != VK_SUCCESS) {                     \
            throw result;                                                      \
        }                                                                      \
    } while (0)

// Block until `target`.
//
// Pacing waits can be several milliseconds now that we hold frames against a
// refresh deadline rather than just against GPU drain, and spinning all of
// that costs a core. Sleep through the bulk of it and spin only the last
// stretch, where the scheduler's granularity would otherwise overshoot.
void precise_wait_until(const DeviceClock::time_point& target);


} // namespace low_latency

#endif
