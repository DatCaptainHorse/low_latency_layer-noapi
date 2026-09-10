#include "physical_device_context.hh"
#include "helper.hh"

#include <vulkan/vulkan_core.h>

#include <ranges>
#include <string>
#include <unordered_set>
#include <vector>

namespace low_latency {

using extension_set_t = std::unordered_set<std::string>;

// Owned strings, not views: VkExtensionProperties::extensionName is a
// fixed-size array living in the vector we enumerate into, and that vector dies
// with this function.
static extension_set_t
enumerate_extensions(const VkuInstanceDispatchTable& vtable,
                     const VkPhysicalDevice& physical_device) {

    auto count = std::uint32_t{};
    THROW_NOT_VKSUCCESS(vtable.EnumerateDeviceExtensionProperties(
        physical_device, nullptr, &count, nullptr));

    auto properties = std::vector<VkExtensionProperties>(count);
    THROW_NOT_VKSUCCESS(vtable.EnumerateDeviceExtensionProperties(
        physical_device, nullptr, &count, std::data(properties)));

    return properties | std::views::transform([](const auto& property) {
               return std::string{property.extensionName};
           }) |
           std::ranges::to<extension_set_t>();
}

static const char*
pick_calibrated_timestamps(const extension_set_t& supported) {
    for (const auto& name :
         PhysicalDeviceContext::calibrated_timestamps_extensions) {

        if (supported.contains(name)) {
            return name;
        }
    }
    return nullptr;
}

static bool has_required_extensions(const extension_set_t& supported) {
    return std::ranges::all_of(PhysicalDeviceContext::required_extensions,
                               [&](const auto& required) {
                                   return supported.contains(required);
                               }) &&
           pick_calibrated_timestamps(supported);
}

// Each of these extensions gates itself behind a feature bit, so the extension
// being present says nothing about whether it can be used. Chain only the
// structures whose extension is actually there - querying a feature struct for
// an absent extension is not valid.
static DisplayExtensions
find_display_extensions(const VkuInstanceDispatchTable& vtable,
                        const std::uint32_t& api_version,
                        const VkPhysicalDevice& physical_device,
                        const extension_set_t& supported) {

    const auto present = [&](const char* const name) {
        return supported.contains(name);
    };

    auto found = DisplayExtensions{
        .present_id2 = present(VK_KHR_PRESENT_ID_2_EXTENSION_NAME),
        .present_timing = present(VK_EXT_PRESENT_TIMING_EXTENSION_NAME),
    };

    // vkGetPhysicalDeviceFeatures2 was promoted in 1.1 and must not be called
    // on an instance that asked for 1.0, however capable the driver is. Such
    // applications get the KHR alias, which the layer adds the instance
    // extension for at creation time.
    const auto get_features2 =
        api_version >= VK_API_VERSION_1_1
            ? vtable.GetPhysicalDeviceFeatures2
            : vtable.GetPhysicalDeviceFeatures2KHR;

    if (!get_features2) {
        // Without a features query we cannot honestly claim any of them.
        return {};
    }

    auto present_id2 = VkPhysicalDevicePresentId2FeaturesKHR{
        .sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_PRESENT_ID_2_FEATURES_KHR,
    };
    auto present_timing = VkPhysicalDevicePresentTimingFeaturesEXT{
        .sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_PRESENT_TIMING_FEATURES_EXT,
    };

    auto features = VkPhysicalDeviceFeatures2{
        .sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_FEATURES_2,
    };

    auto** tail = &features.pNext;
    const auto chain = [&](auto& structure, const bool wanted) {
        if (!wanted) {
            return;
        }
        *tail = &structure;
        tail = &structure.pNext;
    };
    chain(present_id2, found.present_id2);
    chain(present_timing, found.present_timing);

    get_features2(physical_device, &features);

    return {
        .present_id2 = found.present_id2 && present_id2.presentId2,
        .present_timing = found.present_timing && present_timing.presentTiming,
        .present_at_relative_time =
            found.present_timing && present_timing.presentAtRelativeTime,
    };
}

PhysicalDeviceContext::PhysicalDeviceContext(
    InstanceContext& instance_context, const VkPhysicalDevice& physical_device,
    const extension_set_t& supported)
    : instance(instance_context), physical_device(physical_device),
      supports_required_extensions(has_required_extensions(supported)),
      calibrated_timestamps_extension(pick_calibrated_timestamps(supported)),
      display_extensions(find_display_extensions(
          instance_context.vtable, instance_context.api_version,
          physical_device, supported)) {

    const auto& vtable = instance_context.vtable;

    this->properties = [&]() {
        auto props = VkPhysicalDeviceProperties{};
        vtable.GetPhysicalDeviceProperties(physical_device, &props);
        return std::make_unique<VkPhysicalDeviceProperties>(std::move(props));
    }();

    this->queue_properties = [&]() {
        auto count = std::uint32_t{};
        vtable.GetPhysicalDeviceQueueFamilyProperties(physical_device, &count,
                                                      nullptr);

        auto result = std::vector<VkQueueFamilyProperties>(
            count, VkQueueFamilyProperties{});
        vtable.GetPhysicalDeviceQueueFamilyProperties(physical_device, &count,
                                                      std::data(result));
        return std::make_unique<std::vector<VkQueueFamilyProperties>>(
            std::move(result));
    }();
}

PhysicalDeviceContext::PhysicalDeviceContext(
    InstanceContext& instance_context, const VkPhysicalDevice& physical_device)
    : PhysicalDeviceContext(instance_context, physical_device,
                            enumerate_extensions(instance_context.vtable,
                                                 physical_device)) {}

PhysicalDeviceContext::~PhysicalDeviceContext() {}

} // namespace low_latency
