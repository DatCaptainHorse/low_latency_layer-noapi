#ifndef LAYER_CONTEXT_HH_
#define LAYER_CONTEXT_HH_

#include <atomic>
#include <memory>
#include <mutex>
#include <shared_mutex>
#include <unordered_map>
#include <vulkan/vulkan_core.h>

#include "config.hh"
#include "context.hh"
#include "hotkey.hh"
#include "device_context.hh"
#include "instance_context.hh"
#include "physical_device_context.hh"
#include "queue_context.hh"

// The purpose of this file is to provide a definition for the highest level
// entry point struct of our vulkan state.

namespace low_latency {

// All these templates do is make it so we can go from some DispatchableType
// to their respective context's with nice syntax. This lets us write something
// like this for all DispatchableTypes:
//
//     const auto device_context = get_context(some_vk_device);
//           ^ It was automatically deduced as DeviceContext, wow!

template <typename T>
concept DispatchableType =
    std::same_as<std::remove_cvref_t<T>, VkInstance> ||
    std::same_as<std::remove_cvref_t<T>, VkPhysicalDevice> ||
    std::same_as<std::remove_cvref_t<T>, VkDevice> ||
    std::same_as<std::remove_cvref_t<T>, VkQueue>;

template <class D> struct context_for_t;
template <> struct context_for_t<VkInstance> {
    using context = InstanceContext;
};
template <> struct context_for_t<VkPhysicalDevice> {
    using context = PhysicalDeviceContext;
};
template <> struct context_for_t<VkDevice> {
    using context = DeviceContext;
};
template <> struct context_for_t<VkQueue> {
    using context = QueueContext;
};
template <DispatchableType D>
using dispatch_context_t = typename context_for_t<D>::context;

class LayerContext final : public Context {
  public:
    const Config config{};

    // Whether the layer's effect is currently applied. Toggled by the hotkey
    // from another thread and read on every submit and present, so it is an
    // atomic rather than anything lock-shaped.
    std::atomic<bool> effect_enabled{true};

    std::shared_mutex mutex{};
    std::unordered_map<void*, std::shared_ptr<Context>> contexts{};

  public:
    explicit LayerContext();
    virtual ~LayerContext();

  private:
    std::once_flag hotkey_once{};
    std::unique_ptr<HotkeyMonitor> hotkey{};

  public:
    // Started on the first device the layer is active on, rather than at
    // library load: opening /dev/input is not something to do to every process
    // that happens to link Vulkan.
    void ensure_hotkey();

  public:
    template <DispatchableType DT> static void* get_key(const DT& dt) {
        return reinterpret_cast<void*>(dt);
    }

    template <DispatchableType DT>
    std::shared_ptr<dispatch_context_t<DT>> get_context(const DT& dt) {
        const auto key = get_key(dt);

        const auto lock = std::shared_lock{this->mutex};
        const auto it = this->contexts.find(key);
        assert(it != std::end(this->contexts));

        using context_t = dispatch_context_t<DT>;
        const auto ptr = std::dynamic_pointer_cast<context_t>(it->second);
        assert(ptr);
        return ptr;
    }
};

}; // namespace low_latency

#endif
