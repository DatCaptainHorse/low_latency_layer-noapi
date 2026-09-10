#ifndef DISPLAY_FEEDBACK_HH_
#define DISPLAY_FEEDBACK_HH_

#include "device_clock.hh"

#include <vulkan/vulkan_core.h>

#include <cstdint>
#include <memory>
#include <optional>
#include <vector>

// Where a frame actually ended up on the display.
//
// This is the information the layer previously did not have and did not need:
// with VK_NV_low_latency2 or VK_AMD_anti_lag the application told us when to
// release a frame, so we only ever had to answer "has the GPU drained yet".
// Pacing to a refresh deadline instead of to GPU drain needs to know when
// previous frames were actually scanned out, which is what these implementations
// provide.
//
// The only implementation is VK_EXT_present_timing. VK_KHR_present_wait was
// tried and removed: polling it once per frame records the poll time rather
// than the moment of presentation, so it reports the application's own cadence
// and cannot place a refresh boundary. Making it useful would need a worker
// thread blocking in vkWaitForPresent2KHR, whose swapchain parameter is
// externally synchronised against vkAcquireNextImageKHR - so it would also
// need the acquire path locked against that thread.
//
// Implementations must be non-blocking in poll() and are only ever called from
// the application's present thread.

namespace low_latency {

class DeviceContext;

// What a particular surface will actually let us do. Device support is not
// enough: present_id2 and present_timing are both specified as surface
// capabilities, and using them on a surface that does not advertise them is
// invalid regardless of what the physical device says.
struct SurfaceFeedbackSupport final {
    bool present_id2{};
    bool present_timing{};

    // Which present stages the surface can report times for.
    VkPresentStageFlagsEXT present_stages{};

    static SurfaceFeedbackSupport query(const DeviceContext& device,
                                        const VkSurfaceKHR& surface);
};

class DisplayFeedback {
  public:
    struct Sample final {
        std::uint64_t present_id{};

        // When the frame became visible, in the DeviceClock time base.
        DeviceClock::time_point displayed{};
    };

  protected:
    const DeviceContext& device;
    const VkSwapchainKHR swapchain{};

  public:
    explicit DisplayFeedback(const DeviceContext& device,
                             const VkSwapchainKHR& swapchain);
    DisplayFeedback(const DisplayFeedback&) = delete;
    DisplayFeedback(DisplayFeedback&&) = delete;
    DisplayFeedback& operator=(const DisplayFeedback&) = delete;
    DisplayFeedback& operator=(DisplayFeedback&&) = delete;
    virtual ~DisplayFeedback();

  public:
    // Fills this swapchain's slot in a VkPresentTimingsInfoEXT array.
    // Returns false when the implementation needs nothing chained into the
    // present, which leaves the slot as an inert zeroed entry.
    virtual bool fill_timing_info(VkPresentTimingInfoEXT& info) const;

    // Told about every present we issue, so implementations that have to ask
    // about a specific frame know which ones are outstanding.
    virtual void notify_presented(const std::uint64_t& present_id) = 0;

    // Append any newly available samples. Must not block.
    virtual void poll(std::vector<Sample>& out) = 0;

    // The nominal refresh interval if the implementation can report one.
    // Implementations that cannot leave this to the VblankModel to estimate.
    virtual std::optional<DeviceClock::duration> refresh_interval();

    virtual const char* name() const = 0;

    // False once the implementation has decided it cannot do its job. The
    // pacer then drops it and carries on without display feedback rather than
    // keeping a broken source alive.
    virtual bool healthy() const { return true; }

  public:
    // Builds the best available implementation for a swapchain, or nullptr if
    // the device supports none of them.
    static std::unique_ptr<DisplayFeedback>
    create(const DeviceContext& device, const VkSwapchainKHR& swapchain,
           const SurfaceFeedbackSupport& surface_support);
};

// VK_EXT_present_timing. The preferred implementation: it reports real
// per-present stage times in a calibrated time domain, so samples are directly
// comparable with our GPU timestamps, and it never blocks.
class PresentTimingFeedback final : public DisplayFeedback {
  private:
    // How many past-presentation results the driver should keep for us. Only
    // needs to cover the few frames between our polls.
    static constexpr auto TIMING_QUEUE_SIZE = 8u;

    // Consecutive vkGetPastPresentationTimingEXT failures tolerated before
    // giving up. Requesting timings we never read is actively harmful: the
    // driver's queue fills and vkQueuePresentKHR starts failing.
    static constexpr auto MAX_POLL_FAILURES = 4u;

    // How often to re-align a present-stage-local domain with our clock.
    static constexpr auto CALIBRATION_PERIOD = std::chrono::seconds{1};

    // Preference order. FIRST_PIXEL_VISIBLE is what click-to-photon latency
    // actually cares about; the others are progressively earlier proxies.
    static constexpr auto STAGE_PREFERENCE = {
        VK_PRESENT_STAGE_IMAGE_FIRST_PIXEL_VISIBLE_BIT_EXT,
        VK_PRESENT_STAGE_IMAGE_FIRST_PIXEL_OUT_BIT_EXT,
        VK_PRESENT_STAGE_REQUEST_DEQUEUED_BIT_EXT,
        VK_PRESENT_STAGE_QUEUE_OPERATIONS_END_BIT_EXT,
    };

    // Time domains in preference order. The first two are directly usable;
    // the local ones carry no epoch of their own and have to be calibrated
    // against our clock before they mean anything.
    static constexpr auto DOMAIN_PREFERENCE = {
        VK_TIME_DOMAIN_CLOCK_MONOTONIC_KHR,
        VK_TIME_DOMAIN_DEVICE_KHR,
        VK_TIME_DOMAIN_PRESENT_STAGE_LOCAL_EXT,
        VK_TIME_DOMAIN_SWAPCHAIN_LOCAL_EXT,
    };

  private:
    const VkPresentStageFlagsEXT stage{};
    const VkTimeDomainKHR time_domain{};
    const std::uint64_t time_domain_id{};

    // Only meaningful for the local domains: how far their epoch sits from
    // ours, refreshed periodically because the two can drift.
    DeviceClock::duration calibration_offset{};
    DeviceClock::time_point last_calibration{};
    bool has_calibration{};

    std::uint32_t poll_failures{};

    // refresh_interval() re-queries whenever it has no cached answer, which is
    // every frame while the driver keeps refusing - so the diagnostic for it
    // is reported once rather than per frame.
    bool reported_timing_properties{};

    std::uint64_t timing_properties_counter{};
    std::optional<DeviceClock::duration> cached_refresh_interval{};

    // Scratch buffers reused across polls so the steady state allocates
    // nothing on the present thread.
    std::vector<VkPastPresentationTimingEXT> timing_scratch{};
    std::vector<VkPresentStageTimeEXT> stage_scratch{};

  private:
    void note_poll_failure(const VkResult& result);
    bool needs_calibration() const;
    void calibrate();

    std::optional<DeviceClock::time_point>
    to_time_point(const VkPastPresentationTimingEXT& timing) const;

  public:
    explicit PresentTimingFeedback(const DeviceContext& device,
                                   const VkSwapchainKHR& swapchain,
                                   const VkPresentStageFlagsEXT& stage,
                                   const VkTimeDomainKHR& time_domain,
                                   const std::uint64_t& time_domain_id);
    virtual ~PresentTimingFeedback();

  public:
    virtual bool
    fill_timing_info(VkPresentTimingInfoEXT& info) const override;
    virtual void notify_presented(const std::uint64_t& present_id) override;
    virtual void poll(std::vector<Sample>& out) override;
    virtual std::optional<DeviceClock::duration> refresh_interval() override;
    virtual const char* name() const override;
    virtual bool healthy() const override;

  public:
    // Negotiates a stage and time domain for a swapchain. Returns nullptr when
    // the surface or device cannot support the extension.
    static std::unique_ptr<PresentTimingFeedback>
    create(const DeviceContext& device, const VkSwapchainKHR& swapchain,
           const SurfaceFeedbackSupport& surface_support);
};

} // namespace low_latency

#endif
