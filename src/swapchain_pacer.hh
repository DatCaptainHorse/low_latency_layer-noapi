#ifndef SWAPCHAIN_PACER_HH_
#define SWAPCHAIN_PACER_HH_

#include "config.hh"
#include "display_feedback.hh"
#include "submission_span.hh"
#include "vblank_model.hh"

#include <vulkan/vulkan_core.h>

#include <cstdint>
#include <deque>
#include <memory>
#include <optional>
#include <utility>
#include <vector>

namespace low_latency {

class DeviceContext;

// Per-swapchain pacing.
//
// Everything here is a measured quantity or exact arithmetic on one. There is
// no adaptive filter, no gain, no convergence and nothing that has to settle,
// because the previous design had all four and they were the bug: it probed
// with a jitter sleep, asked whether the sleep had been "absorbed", and
// integrated the answer into an extra delay. The question was unanswerable as
// posed - the frame time it compared against is measured from our own release
// to the application's next present, so it never included the delay we had
// just imposed. Sleeping therefore always looked free, the integrator wound up
// until the frame period had roughly doubled, and the only thing that reset it
// was swapchain recreation.
//
// What remains is the part that never needed estimating: wait for the frame
// just presented to leave the GPU before letting the application start the
// next one. That bounds the render queue to one frame in flight, which is
// where the latency actually is, and it costs no throughput because the wait
// is exactly as long as the GPU needs and no longer.
//
// Everything runs on the application's present thread, after the real present
// has been issued - so the block delays the next frame's input rather than
// this frame's scanout.
class SwapchainPacer final {
  private:
    DeviceContext& device;
    const VkSwapchainKHR swapchain{};
    const VkPresentModeKHR present_mode{};

    // FIFO-like modes gate presentation on the display; the others hand the
    // frame over as soon as it is ready. Only the former has a deadline.
    const bool is_refresh_gated{};

    const SurfaceFeedbackSupport surface_support{};

    std::unique_ptr<DisplayFeedback> feedback{};
    VblankModel vblank{};

    // Time from releasing the application to that frame's work being finished
    // on the GPU, as measured on the previous frame. Not smoothed: it is last
    // frame's number used as this frame's estimate, so an error corrects
    // itself immediately instead of decaying over hundreds of frames.
    std::optional<DeviceClock::duration> last_work{};
    std::optional<DeviceClock::time_point> last_release{};

    // Frames whose GPU work has been submitted but not yet waited for. The
    // layer waits for the one `queue_depth` presents back, so the number of
    // frames the GPU may have outstanding is an exact count.
    struct PendingFrame final {
        DeviceClock::time_point release{};
        std::vector<std::unique_ptr<SubmissionSpan>> work{};
    };
    std::deque<PendingFrame> pending{};

    // End of the last frame we waited for, so the gap to the next frame's
    // start can be measured. Both come from GPU timestamp queries, so this is
    // how much the GPU actually sat idle - the direct check on whether the
    // queue-depth bound is starving it.
    std::optional<DeviceClock::time_point> last_gpu_end{};
    DeviceClock::duration last_gpu_gap{};

    std::uint64_t next_present_id{1};

    // Reused across frames so the steady state does not allocate.
    std::vector<DisplayFeedback::Sample> sample_scratch{};

    // Debug accounting.
    std::uint64_t frame_counter{};
    DeviceClock::time_point last_report{};
    DeviceClock::duration last_hold{};

    // How long await_work actually blocked the application. This is the
    // layer's whole effect when there is no refresh period to pace against,
    // so it is worth being able to see it directly.
    DeviceClock::duration last_gpu_wait{};

  private:
    const Config& config() const;

    // Blocks for the frame's GPU work and returns when it finished, or nullopt
    // if the frame contributed no measurable work.
    // Blocks for a frame's GPU work. Returns its measured start and end, or
    // nullopt if the frame contributed no measurable work.
    std::optional<std::pair<DeviceClock::time_point, DeviceClock::time_point>>
    await_work(const std::vector<std::unique_ptr<SubmissionSpan>>& work) const;

    // When to release so the next frame lands just before a refresh, or
    // nullopt when there is no measured period and phase to aim with.
    std::optional<DeviceClock::time_point> deadline_release() const;

    void report();

  public:
    explicit SwapchainPacer(DeviceContext& device,
                            const VkSwapchainKHR& swapchain,
                            const VkSwapchainCreateInfoKHR& info);
    SwapchainPacer(const SwapchainPacer&) = delete;
    SwapchainPacer(SwapchainPacer&&) = delete;
    SwapchainPacer& operator=(const SwapchainPacer&) = delete;
    SwapchainPacer& operator=(SwapchainPacer&&) = delete;
    ~SwapchainPacer();

  public:
    // Which policy this swapchain runs, after resolving Auto against the
    // present mode and the timing the driver actually provides.
    PacingMode effective_mode() const;

  public:
    bool wants_present_id() const;
    bool uses_present_id2() const;
    std::uint64_t reserve_present_id();
    void adopt_present_id(const std::uint64_t& present_id);
    bool fill_timing_info(VkPresentTimingInfoEXT& info) const;

  public:
    // Called after vkQueuePresentKHR returns. Blocks the calling thread.
    //
    // `presented` says whether the present went through. A failed one still
    // has to come through here: its GPU work needs collecting, and the display
    // feedback still needs draining - a driver whose timing queue is never
    // read starts failing presents, which is how a transient error becomes a
    // permanent one.
    void pace(const std::uint64_t& present_id,
              std::vector<std::unique_ptr<SubmissionSpan>> work,
              const bool presented);

    // Stops asking for display timings on this swapchain for good, used when
    // the request itself is what the driver is unhappy about.
    void disable_feedback();
};

} // namespace low_latency

#endif
