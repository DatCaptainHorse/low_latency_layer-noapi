#include "device_context.hh"

#include "frame_pacer.hh"
#include "layer_context.hh"

#include <cstdio>
#include <utility>
#include <vulkan/vulkan_core.h>

namespace low_latency {

DeviceContext::DeviceContext(InstanceContext& parent_instance,
                             PhysicalDeviceContext& parent_physical_device,
                             const VkDevice& device, const bool is_active,
                             const DisplayExtensions& display_extensions,
                             VkuDeviceDispatchTable&& vtable)
    : instance(parent_instance), physical_device(parent_physical_device),
      is_active(is_active), device(device), vtable(std::move(vtable)),
      display_extensions(display_extensions) {

    if (this->instance.layer.config.debug) {
        const auto& available = this->physical_device.display_extensions;
        std::fprintf(stderr,
                     "[low_latency] device active=%d | present_id2/timing "
                     "available=%d%d enabled=%d%d\n",
                     static_cast<int>(this->is_active), available.present_id2,
                     available.present_timing,
                     this->display_extensions.present_id2,
                     this->display_extensions.present_timing);
    }

    if (!this->is_active) {
        return;
    }

    this->clock = std::make_unique<DeviceClock>(*this);
    this->pacer = std::make_unique<FramePacer>(*this);
}

DeviceContext::~DeviceContext() {}

} // namespace low_latency
