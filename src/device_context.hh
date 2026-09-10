#ifndef DEVICE_CONTEXT_HH_
#define DEVICE_CONTEXT_HH_

#include <memory>
#include <shared_mutex>
#include <unordered_map>

#include <vulkan/utility/vk_dispatch_table.h>
#include <vulkan/vk_layer.h>
#include <vulkan/vulkan.hpp>
#include <vulkan/vulkan_core.h>

#include "context.hh"
#include "device_clock.hh"
#include "instance_context.hh"
#include "physical_device_context.hh"
#include "queue_context.hh"

namespace low_latency {

class FramePacer;

class DeviceContext final : public Context {
  public:
    InstanceContext& instance;
    PhysicalDeviceContext& physical_device;

    // Whether the layer is doing anything on this device. Previously this
    // meant "the application asked for our extension"; it now means "the
    // layer is enabled and the physical device can support it", because
    // nothing asks for us any more.
    const bool is_active{};

    const VkDevice device{};
    const VkuDeviceDispatchTable vtable{};

    // Which optional display extensions we actually managed to turn on, as
    // opposed to which ones the physical device merely supports.
    const DisplayExtensions display_extensions{};

    std::shared_mutex mutex{};
    std::unique_ptr<DeviceClock> clock{};
    std::unordered_map<VkQueue, std::shared_ptr<QueueContext>> queues{};
    std::unique_ptr<FramePacer> pacer{};

  public:
    explicit DeviceContext(InstanceContext& parent_instance,
                           PhysicalDeviceContext& parent_physical,
                           const VkDevice& device, const bool is_active,
                           const DisplayExtensions& display_extensions,
                           VkuDeviceDispatchTable&& vtable);
    virtual ~DeviceContext();
};

}; // namespace low_latency

#endif
