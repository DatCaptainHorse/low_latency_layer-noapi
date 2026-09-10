#ifndef FRAME_PACER_HH_
#define FRAME_PACER_HH_

#include "swapchain_pacer.hh"

#include <vulkan/vulkan_core.h>

#include <memory>
#include <mutex>
#include <shared_mutex>
#include <unordered_map>
#include <vector>

namespace low_latency {

class DeviceContext;

// Device-level owner of the pacing machinery.
//
// A present can name several swapchains at once, and both VkPresentId2KHR and
// VkPresentTimingsInfoEXT are parallel arrays over that list, so the structures
// chained into a present have to be assembled here rather than per swapchain.
class FramePacer final {
  public:
    // Holds the patched present info alive for the duration of the down-call,
    // then paces once the present has been issued.
    class PresentScope final {
      private:
        friend class FramePacer;

      private:
        FramePacer& pacer;
        std::unique_lock<std::mutex> lock;
        const VkPresentInfoKHR* patched{};
        const VkPresentInfoKHR* original{};

      private:
        explicit PresentScope(FramePacer& pacer,
                              std::unique_lock<std::mutex> lock,
                              const VkPresentInfoKHR* patched,
                              const VkPresentInfoKHR* original);

      public:
        PresentScope(const PresentScope&) = delete;
        PresentScope(PresentScope&&) = delete;
        PresentScope& operator=(const PresentScope&) = delete;
        PresentScope& operator=(PresentScope&&) = delete;
        ~PresentScope();

      public:
        // What to actually hand to the driver.
        const VkPresentInfoKHR* info() const { return this->patched; }

        // Called with the result of the real present. Blocks for pacing, and
        // returns the result the application should see - which is not always
        // the one passed in, because a present can fail for reasons that are
        // purely the layer's doing.
        VkResult finish(const VkResult& result);
    };

  private:
    // Scratch for the patched present chain, only touched under present_mutex.
    struct PresentPatch final {
        VkPresentInfoKHR info{};
        VkPresentId2KHR present_id2{};
        VkPresentTimingsInfoEXT timings{};
        std::vector<std::uint64_t> ids{};
        std::vector<VkPresentTimingInfoEXT> timing_infos{};
    };

  private:
    DeviceContext& device;

    std::shared_mutex mutex{};
    std::unordered_map<VkSwapchainKHR, std::unique_ptr<SwapchainPacer>>
        pacers{};

    std::mutex present_mutex{};
    PresentPatch patch{};

    // The ids actually used by the present in flight, one per swapchain.
    std::vector<std::uint64_t> in_flight_ids{};

  private:
    // Everything the tracked queues have accumulated since the last present.
    std::vector<std::unique_ptr<SubmissionSpan>> collect_work() const;

    // Stops requesting display timings on every swapchain in a present.
    void disable_feedback_for(const VkPresentInfoKHR& info);

  public:
    explicit FramePacer(DeviceContext& device);
    FramePacer(const FramePacer&) = delete;
    FramePacer(FramePacer&&) = delete;
    FramePacer& operator=(const FramePacer&) = delete;
    FramePacer& operator=(FramePacer&&) = delete;
    ~FramePacer();

  public:
    void notify_create_swapchain(const VkSwapchainKHR& swapchain,
                                 const VkSwapchainCreateInfoKHR& info);
    void notify_destroy_swapchain(const VkSwapchainKHR& swapchain);

  public:
    PresentScope begin_present(const VkPresentInfoKHR& info);
};

} // namespace low_latency

#endif
