#ifndef VBLANK_MODEL_HH_
#define VBLANK_MODEL_HH_

#include "device_clock.hh"
#include "display_feedback.hh"

#include <cstdint>
#include <optional>

// Where the display's refresh boundaries are.
//
// Both halves of this come from the driver by way of VK_EXT_present_timing:
// the period from vkGetSwapchainTimingPropertiesEXT, the phase from real
// per-present timestamps.
//
// Nothing here is estimated from the spacing of presents any more, and that is
// deliberate. The layer used to fall back to measuring how far apart presents
// landed and calling that the refresh period. It is not: polling present
// completion once per frame records the poll time, so the spacing measured is
// the application's own cadence. An application running at 20fps on a 120Hz
// panel produced a 50ms "refresh period", and pacing against a grid six times
// too coarse moved the release by a whole coarse step whenever GPU time
// wandered - a slow oscillation in frame rate rather than a latency win. The
// information needed to tell 50ms-with-misses from a 50Hz panel is simply not
// present in those samples, so the period is now only ever taken from a driver
// that actually knows it.

namespace low_latency {

class VblankModel final {
  private:
    // Anything outside this is a dropped frame, a mode change or a stall
    // rather than a refresh interval.
    static constexpr auto MIN_INTERVAL = std::chrono::microseconds{1000};
    static constexpr auto MAX_INTERVAL = std::chrono::milliseconds{100};

    // How many presents we want to have seen before trusting the phase.
    static constexpr auto MIN_SAMPLES = 8u;

  private:
    std::optional<DeviceClock::duration> nominal_interval{};

    std::optional<DeviceClock::time_point> last_displayed{};
    std::optional<std::uint64_t> last_present_id{};

    std::uint32_t sample_count{};

    // Diagnostics only.
    DeviceClock::duration last_delta{};
    std::uint32_t rejected_count{};

  public:
    explicit VblankModel();
    ~VblankModel();

  public:
    // The driver's refresh interval. Without it there is no period, and
    // therefore no deadline to pace against.
    void set_nominal_interval(const std::optional<DeviceClock::duration>& in);

    void add_sample(const DisplayFeedback::Sample& sample);

    // Discards phase but keeps the interval estimate - used when the
    // swapchain's present stream is interrupted.
    void reset_phase();

  public:
    std::optional<DeviceClock::duration> interval() const;

    bool is_usable() const;

    // The first refresh boundary at or after `point`.
    std::optional<DeviceClock::time_point>
    boundary_at_or_after(const DeviceClock::time_point& point) const;

  public:
    DeviceClock::duration debug_last_delta() const { return this->last_delta; }
    std::uint32_t debug_samples() const { return this->sample_count; }
    std::uint32_t debug_rejected() const { return this->rejected_count; }
    bool debug_has_nominal() const {
        return this->nominal_interval.has_value();
    }
};

} // namespace low_latency

#endif
