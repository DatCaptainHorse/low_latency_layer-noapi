#ifndef PHYSICAL_DEVICE_CONTEXT_HH_
#define PHYSICAL_DEVICE_CONTEXT_HH_

#include "instance_context.hh"

#include <vulkan/vulkan.hpp>
#include <vulkan/vulkan_core.h>

#include <string>
#include <unordered_set>

#include "context.hh"

namespace low_latency {

// Which of the extensions we would like to use are actually available. None of
// these are load-bearing on their own - the layer degrades to progressively
// simpler pacing as they drop away - so they are tracked separately from the
// hard requirements.
struct DisplayExtensions final {
    // present_id2 exists only because present_timing is built on top of it -
    // a frame has to be labelled before the driver can report a time for it.
    bool present_id2{};
    bool present_timing{};

    // Present timing splits scheduling out of the base feature. We only need
    // relative scheduling, and only to say "no delay" in a well-defined way -
    // supplying a target time with neither scheduling feature enabled is
    // invalid.
    bool present_at_relative_time{};
};

class PhysicalDeviceContext final : public Context {
  public:
    // Without these the layer cannot measure anything and stays inert:
    // timestamps in submissions, a host-side reset for their query pools, and
    // a way to put GPU and CPU times on the same axis.
    static constexpr auto required_extensions = {
        VK_KHR_SYNCHRONIZATION_2_EXTENSION_NAME,
        VK_EXT_HOST_QUERY_RESET_EXTENSION_NAME,
    };

    // VK_KHR_calibrated_timestamps was VK_EXT_calibrated_timestamps first and
    // plenty of drivers still only expose the older name. They are the same
    // extension, so either will do.
    static constexpr auto calibrated_timestamps_extensions = {
        VK_KHR_CALIBRATED_TIMESTAMPS_EXTENSION_NAME,
        VK_EXT_CALIBRATED_TIMESTAMPS_EXTENSION_NAME,
    };

  public:
    InstanceContext& instance;
    const VkPhysicalDevice physical_device{};
    const bool supports_required_extensions{};

    // Whichever spelling of calibrated timestamps this driver exposes.
    const char* const calibrated_timestamps_extension{};

    const DisplayExtensions display_extensions{};

    std::unique_ptr<VkPhysicalDeviceProperties> properties{};
    std::unique_ptr<std::vector<VkQueueFamilyProperties>> queue_properties{};

  private:
    // Delegated to so that a single extension enumeration can initialise
    // several const members.
    explicit PhysicalDeviceContext(
        InstanceContext& instance_context,
        const VkPhysicalDevice& physical_device,
        const std::unordered_set<std::string>& supported);

  public:
    explicit PhysicalDeviceContext(InstanceContext& instance_context,
                                   const VkPhysicalDevice& physical_device);
    virtual ~PhysicalDeviceContext();
};

} // namespace low_latency

#endif
