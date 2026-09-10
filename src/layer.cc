#include "layer.hh"

#include <array>
#include <iterator>
#include <ranges>
#include <span>
#include <string_view>
#include <unordered_map>
#include <utility>
#include <vector>

#include <vulkan/utility/vk_dispatch_table.h>
#include <vulkan/utility/vk_safe_struct.hpp>
#include <vulkan/utility/vk_struct_helper.hpp>
#include <vulkan/vk_layer.h>
#include <vulkan/vk_platform.h>
#include <vulkan/vulkan.h>
#include <vulkan/vulkan.hpp>
#include <vulkan/vulkan_core.h>

#include "config.hh"
#include "device_clock.hh"
#include "device_context.hh"
#include "frame_pacer.hh"
#include "instance_context.hh"
#include "layer_context.hh"
#include "queue_context.hh"
#include "queue_tracker.hh"
#include "timestamp_pool.hh"

namespace low_latency {

namespace {

LayerContext layer_context;

} // namespace

static VKAPI_ATTR VkResult VKAPI_CALL CreateInstance(
    const VkInstanceCreateInfo* pCreateInfo,
    const VkAllocationCallbacks* pAllocator, VkInstance* pInstance) noexcept {

    const auto link_info = [&]() -> auto {
        for (auto i = static_cast<const VkBaseInStructure*>(pCreateInfo->pNext);
             i; i = i->pNext) {
            if (i->sType != VK_STRUCTURE_TYPE_LOADER_INSTANCE_CREATE_INFO) {
                continue;
            }

            const auto info =
                reinterpret_cast<const VkLayerInstanceCreateInfo*>(i);
            if (info->function != VK_LAYER_LINK_INFO) {
                continue;
            }
            return info;
        }
        return static_cast<const VkLayerInstanceCreateInfo*>(nullptr);
    }();

    if (!link_info || !link_info->u.pLayerInfo) {
        return VK_ERROR_INITIALIZATION_FAILED;
    }

    // Store our get instance proc addr function and pop it off our list +
    // advance the list so future layers know what to call.
    const auto gipa = link_info->u.pLayerInfo->pfnNextGetInstanceProcAddr;
    if (!gipa) {
        return VK_ERROR_INITIALIZATION_FAILED;
    }
    const_cast<VkLayerInstanceCreateInfo*>(link_info)->u.pLayerInfo =
        link_info->u.pLayerInfo->pNext;

    // Call our create instance func, and store vkDestroyInstance, and
    // vkCreateDevice as well.
    const auto create_instance = reinterpret_cast<PFN_vkCreateInstance>(
        gipa(VK_NULL_HANDLE, "vkCreateInstance"));
    if (!create_instance) {
        return VK_ERROR_INITIALIZATION_FAILED;
    }

    // present_id2 and present_timing are both advertised as surface
    // capabilities, which can only be read through
    // vkGetPhysicalDeviceSurfaceCapabilities2KHR. Applications that use none
    // of them have no reason to have enabled that instance extension, so add
    // it ourselves rather than querying it illegally later.
    //
    // We do not check availability first. Asking the next layer to enumerate
    // instance extensions means calling its vkGetInstanceProcAddr with a null
    // instance, which layers are not obliged to serve and which at least one
    // shipping layer segfaults on. Attempting the addition and retrying
    // without it is both safer and cheaper.
    const auto extra_extensions = [&]() -> std::vector<const char*> {
        if (layer_context.config.mode == PacingMode::Off) {
            return {};
        }

        const auto names = std::span{pCreateInfo->ppEnabledExtensionNames,
                                     pCreateInfo->enabledExtensionCount};

        // The second one only matters for pre-1.1 applications, where the
        // promoted vkGetPhysicalDeviceFeatures2 must not be called and the
        // KHR alias is the only legal way to read a feature bit.
        constexpr auto wanted = std::array{
            VK_KHR_GET_SURFACE_CAPABILITIES_2_EXTENSION_NAME,
            VK_KHR_GET_PHYSICAL_DEVICE_PROPERTIES_2_EXTENSION_NAME,
        };

        auto missing = std::vector<const char*>{};
        for (const auto& candidate : wanted) {
            const auto already =
                std::ranges::any_of(names, [&](const auto& name) {
                    return std::string_view{name} == candidate;
                });
            if (!already) {
                missing.push_back(candidate);
            }
        }
        return missing;
    }();

    const auto result = [&]() -> VkResult {
        if (extra_extensions.empty()) {
            return create_instance(pCreateInfo, pAllocator, pInstance);
        }

        auto names = std::vector(pCreateInfo->ppEnabledExtensionNames,
                                 pCreateInfo->ppEnabledExtensionNames +
                                     pCreateInfo->enabledExtensionCount);
        std::ranges::copy(extra_extensions, std::back_inserter(names));

        auto next_create_info = *pCreateInfo;
        next_create_info.ppEnabledExtensionNames = std::data(names);
        next_create_info.enabledExtensionCount =
            static_cast<std::uint32_t>(std::size(names));

        const auto result =
            create_instance(&next_create_info, pAllocator, pInstance);
        if (result != VK_ERROR_EXTENSION_NOT_PRESENT) {
            return result;
        }

        // Without these, deadline pacing has nothing to key off, but drain
        // pacing still works.
        return create_instance(pCreateInfo, pAllocator, pInstance);
    }();

    if (result != VK_SUCCESS) {
        return result;
    }

    const auto api_version =
        pCreateInfo->pApplicationInfo && pCreateInfo->pApplicationInfo->apiVersion
            ? pCreateInfo->pApplicationInfo->apiVersion
            : VK_API_VERSION_1_0;

    auto vtable = VkuInstanceDispatchTable{};
    vkuInitInstanceDispatchTable(*pInstance, &vtable, gipa);

    const auto key = layer_context.get_key(*pInstance);
    const auto lock = std::scoped_lock{layer_context.mutex};
    assert(!layer_context.contexts.contains(key));
    layer_context.contexts.try_emplace(
        key, std::make_shared<InstanceContext>(
                 layer_context, *pInstance, api_version, std::move(vtable)));

    return VK_SUCCESS;
}

static VKAPI_ATTR void VKAPI_CALL DestroyInstance(
    VkInstance instance, const VkAllocationCallbacks* allocator) noexcept {
    // These requires special care because multiple threads might create a race
    // condition by being given the same VkInstance dispatchable handle.
    const auto destroy_instance = [&]() {
        const auto lock = std::scoped_lock{layer_context.mutex};

        const auto key = layer_context.get_key(instance);
        const auto iter = layer_context.contexts.find(key);
        assert(iter != std::end(layer_context.contexts));
        auto context = std::dynamic_pointer_cast<InstanceContext>(iter->second);

        // Erase our physical devices owned by this instance from the global
        // context.
        for (const auto& [key, _] : context->physical_devices) {
            assert(layer_context.contexts.contains(key));
            layer_context.contexts.erase(key);
        }

        // Should be the last context here, so when we leave scope its
        // destructor is called.
        layer_context.contexts.erase(iter);
        assert(context.unique());
        return context->vtable.DestroyInstance;
    }();

    destroy_instance(instance, allocator);
}

static VKAPI_ATTR VkResult VKAPI_CALL
EnumeratePhysicalDevices(VkInstance instance, std::uint32_t* count,
                         VkPhysicalDevice* devices) noexcept {
    const auto context = layer_context.get_context(instance);

    if (const auto result =
            context->vtable.EnumeratePhysicalDevices(instance, count, devices);
        !devices || !count || result != VK_SUCCESS) {

        return result;
    }

    const auto lock = std::scoped_lock{layer_context.mutex};
    for (const auto& device : std::span{devices, *count}) {
        const auto key = layer_context.get_key(device);
        const auto [iter, inserted] =
            layer_context.contexts.try_emplace(key, nullptr);

        if (inserted) {
            iter->second =
                std::make_shared<PhysicalDeviceContext>(*context, device);
        }

        context->physical_devices.emplace(
            key, std::static_pointer_cast<PhysicalDeviceContext>(iter->second));
    }

    return VK_SUCCESS;
}

static VKAPI_ATTR VkResult VKAPI_CALL CreateDevice(
    VkPhysicalDevice physical_device, const VkDeviceCreateInfo* pCreateInfo,
    const VkAllocationCallbacks* pAllocator, VkDevice* pDevice) noexcept {

    const auto enabled_extensions =
        std::span{pCreateInfo->ppEnabledExtensionNames,
                  pCreateInfo->enabledExtensionCount};

    const auto requested = std::unordered_set<std::string_view>(
        std::begin(enabled_extensions), std::end(enabled_extensions));

    const auto context = layer_context.get_context(physical_device);

    // The layer used to switch itself on when the application enabled the
    // extension it was impersonating. Nothing impersonates anything now, so
    // being loaded at all is the opt-in and the only remaining question is
    // whether this physical device can carry us.
    const auto is_active = layer_context.config.mode != PacingMode::Off &&
                           context->supports_required_extensions;

    if (is_active) {
        // First device we are actually going to act on: this is where the
        // runtime toggle's input monitor gets started.
        layer_context.ensure_hotkey();
    }

    const auto create_info = [&]() -> auto {
        for (auto i = static_cast<const VkBaseInStructure*>(pCreateInfo->pNext);
             i; i = i->pNext) {
            if (i->sType != VK_STRUCTURE_TYPE_LOADER_DEVICE_CREATE_INFO) {
                continue;
            }

            const auto info =
                reinterpret_cast<const VkLayerDeviceCreateInfo*>(i);
            if (info->function != VK_LAYER_LINK_INFO) {
                continue;
            }
            return info;
        }
        return static_cast<const VkLayerDeviceCreateInfo*>(nullptr);
    }();
    if (!create_info || !create_info->u.pLayerInfo) {
        return VK_ERROR_INITIALIZATION_FAILED;
    }

    const auto gipa = create_info->u.pLayerInfo->pfnNextGetInstanceProcAddr;
    const auto gdpa = create_info->u.pLayerInfo->pfnNextGetDeviceProcAddr;
    if (!gdpa || !gipa) {
        return VK_ERROR_INITIALIZATION_FAILED;
    }

    const_cast<VkLayerDeviceCreateInfo*>(create_info)->u.pLayerInfo =
        create_info->u.pLayerInfo->pNext;

    // Which display extensions we will actually turn on. Availability alone
    // is not enough - present_wait needs present_id alongside it, and the
    // configuration can veto either.
    const auto display_extensions = [&]() -> DisplayExtensions {
        if (!is_active) {
            return {};
        }

        const auto& available = context->display_extensions;
        const auto& config = layer_context.config;

        auto wanted = DisplayExtensions{
            .present_id2 = available.present_id2,
            .present_timing =
                available.present_timing && config.allow_present_timing,
            .present_at_relative_time = available.present_at_relative_time &&
                                        config.allow_present_timing,
        };

        // Present timing is built on present_id2, and its relative scheduling
        // feature is only meaningful alongside it.
        if (!wanted.present_id2) {
            wanted.present_timing = false;
        }
        if (!wanted.present_timing) {
            wanted.present_at_relative_time = false;

            // Labelling presents is only worth the trouble so that timings
            // can be asked about them.
            wanted.present_id2 = false;
        }

        return wanted;
    }();

    // Build a next extensions vector from what they have requested.
    const auto next_extensions = [&]() -> std::vector<const char*> {
        auto next_extensions = std::vector(std::begin(enabled_extensions),
                                           std::end(enabled_extensions));

        const auto append = [&](const char* const name) {
            if (name && !requested.contains(name)) {
                next_extensions.push_back(name);
            }
        };

        if (!is_active) {
            return next_extensions;
        }

        for (const auto& name : PhysicalDeviceContext::required_extensions) {
            append(name);
        }
        append(context->calibrated_timestamps_extension);

        if (display_extensions.present_id2) {
            append(VK_KHR_PRESENT_ID_2_EXTENSION_NAME);
        }
        if (display_extensions.present_timing) {
            append(VK_EXT_PRESENT_TIMING_EXTENSION_NAME);
        }

        return next_extensions;
    }();

    const auto next_create_info = [&]() -> auto {
        // Give vku's CreateInfo a patched copy so its own deep copy is correct.
        auto create_info_copy = *pCreateInfo;
        create_info_copy.ppEnabledExtensionNames = std::data(next_extensions);
        create_info_copy.enabledExtensionCount =
            static_cast<std::uint32_t>(std::size(next_extensions));

        auto next_create_info = vku::safe_VkDeviceCreateInfo{&create_info_copy};
        if (!is_active) {
            return next_create_info;
        }

        auto* const pnext = const_cast<void*>(next_create_info.pNext);

        // Sync2 lives in 1.3 features first. If that doesn't exist look for
        // sync2 features or append it.
        if (const auto vk13 =
                vku::FindStructInPNextChain<VkPhysicalDeviceVulkan13Features>(
                    pnext);
            vk13) {

            vk13->synchronization2 = VK_TRUE;
        } else if (const auto s2f = vku::FindStructInPNextChain<
                       VkPhysicalDeviceSynchronization2Features>(pnext);
                   s2f) {

            s2f->synchronization2 = VK_TRUE;
        } else {
            vku::AddToPnext(
                next_create_info,
                VkPhysicalDeviceSynchronization2Features{
                    .sType =
                        VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_SYNCHRONIZATION_2_FEATURES,
                    .synchronization2 = VK_TRUE,
                });
        }

        // HQR is in 1.2 features first - then same idea as sync2.
        if (const auto vk12 =
                vku::FindStructInPNextChain<VkPhysicalDeviceVulkan12Features>(
                    pnext);
            vk12) {

            vk12->hostQueryReset = VK_TRUE;
        } else if (const auto hqrf = vku::FindStructInPNextChain<
                       VkPhysicalDeviceHostQueryResetFeatures>(pnext);
                   hqrf) {

            hqrf->hostQueryReset = VK_TRUE;
        } else {
            vku::AddToPnext(
                next_create_info,
                VkPhysicalDeviceHostQueryResetFeatures{
                    .sType =
                        VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_HOST_QUERY_RESET_FEATURES,
                    .hostQueryReset = VK_TRUE,
                });
        }

        // The display extensions all gate themselves behind a feature bit.
        // None of them are promoted into a core feature struct, so unlike
        // sync2 and host query reset there is only ever one place to look.
        const auto enable_feature = [&]<typename T>(const bool wanted, T&& base,
                                                    VkBool32 T::* member) {
            if (!wanted) {
                return;
            }
            if (const auto existing =
                    vku::FindStructInPNextChain<std::remove_cvref_t<T>>(pnext);
                existing) {

                existing->*member = VK_TRUE;
                return;
            }
            base.*member = VK_TRUE;
            vku::AddToPnext(next_create_info, base);
        };

        enable_feature(display_extensions.present_id2,
                       VkPhysicalDevicePresentId2FeaturesKHR{
                           .sType =
                               VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_PRESENT_ID_2_FEATURES_KHR,
                       },
                       &VkPhysicalDevicePresentId2FeaturesKHR::presentId2);

        enable_feature(display_extensions.present_timing,
                       VkPhysicalDevicePresentTimingFeaturesEXT{
                           .sType =
                               VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_PRESENT_TIMING_FEATURES_EXT,
                       },
                       &VkPhysicalDevicePresentTimingFeaturesEXT::presentTiming);

        // Chained onto the same structure the call above just added, so it
        // will be found rather than duplicated.
        enable_feature(
            display_extensions.present_at_relative_time,
            VkPhysicalDevicePresentTimingFeaturesEXT{
                .sType =
                    VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_PRESENT_TIMING_FEATURES_EXT,
            },
            &VkPhysicalDevicePresentTimingFeaturesEXT::presentAtRelativeTime);

        return next_create_info;
    }();

    const auto create_device = reinterpret_cast<PFN_vkCreateDevice>(
        gipa(VK_NULL_HANDLE, "vkCreateDevice"));
    if (!create_device) {
        return VK_ERROR_INITIALIZATION_FAILED;
    }

    if (const auto result = create_device(
            physical_device, next_create_info.ptr(), pAllocator, pDevice);
        result != VK_SUCCESS) {

        return result;
    }

    auto vtable = VkuDeviceDispatchTable{};
    vkuInitDeviceDispatchTable(*pDevice, &vtable, gdpa);

    const auto key = layer_context.get_key(*pDevice);
    const auto lock = std::scoped_lock{layer_context.mutex};
    assert(!layer_context.contexts.contains(key));
    layer_context.contexts.try_emplace(
        key, std::make_shared<DeviceContext>(context->instance, *context,
                                             *pDevice, is_active,
                                             display_extensions,
                                             std::move(vtable)));

    return VK_SUCCESS;
}

static VKAPI_ATTR void VKAPI_CALL DestroyDevice(
    VkDevice device, const VkAllocationCallbacks* allocator) noexcept {
    // Similarly to DestroyInstance, this needs to be done carefully to avoid a
    // race.
    const auto destroy_device = [&]() -> auto {
        const auto lock = std::scoped_lock{layer_context.mutex};

        const auto key = layer_context.get_key(device);
        const auto iter = layer_context.contexts.find(key);
        assert(iter != std::end(layer_context.contexts));
        auto context = std::dynamic_pointer_cast<DeviceContext>(iter->second);

        // Remove all owned queues from our global context pool.
        for (const auto& [queue, _] : context->queues) {
            const auto key = layer_context.get_key(queue);
            assert(layer_context.contexts.contains(key));
            layer_context.contexts.erase(key);
        }

        // Should be the last shared ptr now, similar to DestroyInstance.
        layer_context.contexts.erase(iter);
        assert(context.unique());
        return context->vtable.DestroyDevice;
    }();

    destroy_device(device, allocator);
}

static VKAPI_ATTR void VKAPI_CALL
GetDeviceQueue(VkDevice device, std::uint32_t queue_family_index,
               std::uint32_t queue_index, VkQueue* queue) noexcept {
    const auto context = layer_context.get_context(device);

    // Get device queue, unlike CreateDevice or CreateInstance, can be
    // called multiple times to return the same queue object. Our insertion
    // handling has to be a little different where we account for this.
    context->vtable.GetDeviceQueue(device, queue_family_index, queue_index,
                                   queue);
    if (!queue || !*queue) {
        return;
    }

    // Look in our layer context, which has everything. If we were able to
    // insert a nullptr key, then it didn't already exist so we should
    // construct a new one.
    const auto key = layer_context.get_key(*queue);
    const auto layer_lock = std::scoped_lock{layer_context.mutex};
    const auto [it, inserted] = layer_context.contexts.try_emplace(key);
    if (inserted) {
        it->second = std::make_shared<QueueContext>(*context, *queue,
                                                    queue_family_index);
    }

    // it->second should be QueueContext, also it might already be there.
    const auto ptr = std::dynamic_pointer_cast<QueueContext>(it->second);
    assert(ptr);
    const auto device_lock = std::scoped_lock{context->mutex};
    context->queues.emplace(*queue, ptr);
}

// Identical logic to gdq1.
static VKAPI_ATTR void VKAPI_CALL GetDeviceQueue2(
    VkDevice device, const VkDeviceQueueInfo2* info, VkQueue* queue) noexcept {

    const auto context = layer_context.get_context(device);

    context->vtable.GetDeviceQueue2(device, info, queue);
    if (!queue || !*queue) {
        return;
    }

    const auto key = layer_context.get_key(*queue);
    const auto lock = std::scoped_lock{layer_context.mutex};
    const auto [it, inserted] = layer_context.contexts.try_emplace(key);
    if (inserted) {
        it->second = std::make_shared<QueueContext>(*context, *queue,
                                                    info->queueFamilyIndex);
    }

    const auto ptr = std::dynamic_pointer_cast<QueueContext>(it->second);
    assert(ptr);
    const auto device_lock = std::scoped_lock{context->mutex};
    context->queues.emplace(*queue, ptr);
}

static VKAPI_ATTR VkResult VKAPI_CALL
QueueSubmit(VkQueue queue, std::uint32_t submit_count,
            const VkSubmitInfo* submit_infos, VkFence fence) noexcept {
    const auto context = layer_context.get_context(queue);
    const auto& vtable = context->device.vtable;

    if (!submit_count || !context->should_inject_timestamps()) {
        return vtable.QueueSubmit(queue, submit_count, submit_infos, fence);
    }

    // We are making a modest modification to all vkQueueSubmits where we inject
    // a start and end timestamp query command buffer that writes when the GPU
    // started and finished work for each submission. We can wait on the
    // completion of these queue submissions through this mechanism.

    using cbs_t = std::vector<VkCommandBuffer>;
    auto next_submits = std::vector<VkSubmitInfo>{};

    // We're making modifications to multiple vkQueueSubmits. These have raw
    // pointers to our command buffer arrays - of which the position in memory
    // of can change on vector reallocation. So we use unique_ptrs here.
    auto next_cbs = std::vector<std::unique_ptr<cbs_t>>{};
    auto handles = std::vector<std::shared_ptr<TimestampPool::Handle>>{};

    const auto submit_span = std::span{submit_infos, submit_count};

    std::ranges::transform(
        submit_span, std::back_inserter(next_submits), [&](const auto& submit) {
            const auto handle = context->timestamp_pool->acquire();
            handles.push_back(handle);

            next_cbs.emplace_back([&]() -> auto {
                auto cbs = std::make_unique<cbs_t>();
                cbs->push_back(handle->get_start_buffer());
                std::ranges::copy(std::span{submit.pCommandBuffers,
                                            submit.commandBufferCount},
                                  std::back_inserter(*cbs));
                cbs->push_back(handle->get_end_buffer());
                return cbs;
            }());

            auto next_submit = submit;
            next_submit.pCommandBuffers = std::data(*next_cbs.back());
            next_submit.commandBufferCount =
                static_cast<std::uint32_t>(std::size(*next_cbs.back()));
            return next_submit;
        });

    if (const auto result = vtable.QueueSubmit(
            queue, static_cast<std::uint32_t>(std::size(next_submits)),
            std::data(next_submits), fence);
        result != VK_SUCCESS) {

        return result;
    }

    // We have to notify after we submit - otherwise we have a race where we
    // wait for work that wasn't submitted.
    for (auto&& handle : handles) {
        handle->was_submitted.store(true, std::memory_order_relaxed);
        context->tracker->notify_submit(std::move(handle));
    }

    return VK_SUCCESS;
}

// The logic for this function is identical to vkSubmitInfo.
static VKAPI_ATTR VkResult VKAPI_CALL
QueueSubmit2Impl(VkQueue queue, std::uint32_t submit_count,
                 const VkSubmitInfo2* submit_infos, VkFence fence,
                 const bool should_use_khr) noexcept {
    const auto context = layer_context.get_context(queue);
    const auto& vtable = context->device.vtable;
    const auto& queue_submit_func =
        should_use_khr ? vtable.QueueSubmit2KHR : vtable.QueueSubmit2;

    if (!submit_count || !context->should_inject_timestamps()) {
        return queue_submit_func(queue, submit_count, submit_infos, fence);
    }

    using cbs_t = std::vector<VkCommandBufferSubmitInfo>;
    auto next_submits = std::vector<VkSubmitInfo2>{};
    auto next_cbs = std::vector<std::unique_ptr<cbs_t>>{};
    auto handles = std::vector<std::shared_ptr<TimestampPool::Handle>>{};

    const auto submit_span = std::span{submit_infos, submit_count};

    std::ranges::transform(
        submit_span, std::back_inserter(next_submits), [&](const auto& submit) {
            const auto handle = context->timestamp_pool->acquire();
            handles.push_back(handle);

            next_cbs.emplace_back([&]() -> auto {
                auto cbs = std::make_unique<cbs_t>();
                cbs->push_back(VkCommandBufferSubmitInfo{
                    .sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_SUBMIT_INFO,
                    .commandBuffer = handle->get_start_buffer(),
                });
                std::ranges::copy(std::span{submit.pCommandBufferInfos,
                                            submit.commandBufferInfoCount},
                                  std::back_inserter(*cbs));
                cbs->push_back(VkCommandBufferSubmitInfo{
                    .sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_SUBMIT_INFO,
                    .commandBuffer = handle->get_end_buffer(),
                });
                return cbs;
            }());

            auto next_submit = submit;
            next_submit.pCommandBufferInfos = std::data(*next_cbs.back());
            next_submit.commandBufferInfoCount =
                static_cast<std::uint32_t>(std::size(*next_cbs.back()));
            return next_submit;
        });

    if (const auto result = queue_submit_func(
            queue, static_cast<std::uint32_t>(std::size(next_submits)),
            std::data(next_submits), fence);
        result != VK_SUCCESS) {

        return result;
    }

    for (auto&& handle : handles) {
        handle->was_submitted.store(true, std::memory_order_relaxed);
        context->tracker->notify_submit(std::move(handle));
    }

    return VK_SUCCESS;
}

static VKAPI_ATTR VkResult VKAPI_CALL
QueueSubmit2(VkQueue queue, std::uint32_t submit_count,
             const VkSubmitInfo2* submit_info, VkFence fence) noexcept {
    return QueueSubmit2Impl(queue, submit_count, submit_info, fence, false);
}

static VKAPI_ATTR VkResult VKAPI_CALL
QueueSubmit2KHR(VkQueue queue, std::uint32_t submit_count,
                const VkSubmitInfo2* submit_info, VkFence fence) noexcept {
    return QueueSubmit2Impl(queue, submit_count, submit_info, fence, true);
}

static VKAPI_ATTR VkResult VKAPI_CALL
QueuePresentKHR(VkQueue queue, const VkPresentInfoKHR* present_info) noexcept {
    const auto context = layer_context.get_context(queue);
    const auto& vtable = context->device.vtable;

    assert(present_info);
    if (!context->device.pacer) {
        return vtable.QueuePresentKHR(queue, present_info);
    }

    // The pacing block goes *after* the real present, not before it. The frame
    // being presented is already finished, so delaying its hand-off would only
    // add latency to it; what we want to delay is the application picking up
    // the next frame, which is what it does when this call returns.
    auto scope = context->device.pacer->begin_present(*present_info);

    const auto result = vtable.QueuePresentKHR(queue, scope.info());

    // finish() can substitute a different result: a present that failed only
    // because of what the layer added is retried without it.
    return scope.finish(result);
}

static VKAPI_ATTR VkResult VKAPI_CALL
CreateSwapchainKHR(VkDevice device, VkSwapchainCreateInfoKHR* pCreateInfo,
                   const VkAllocationCallbacks* pAllocator,
                   VkSwapchainKHR* pSwapchain) noexcept {

    if (!(pCreateInfo->flags & VK_SWAPCHAIN_CREATE_PRESENT_TIMING_BIT_EXT))
        pCreateInfo->flags |= VK_SWAPCHAIN_CREATE_PRESENT_TIMING_BIT_EXT;

    const auto context = layer_context.get_context(device);
    if (const auto result = context->vtable.CreateSwapchainKHR(
            device, pCreateInfo, pAllocator, pSwapchain);
        result != VK_SUCCESS) {

        return result;
    }

    if (context->pacer) {
        assert(pCreateInfo);
        context->pacer->notify_create_swapchain(*pSwapchain, *pCreateInfo);
    }

    return VK_SUCCESS;
}

static VKAPI_ATTR void VKAPI_CALL
DestroySwapchainKHR(VkDevice device, VkSwapchainKHR swapchain,
                    const VkAllocationCallbacks* pAllocator) noexcept {
    const auto context = layer_context.get_context(device);

    // Tear our state down first - the pacer holds swapchain-scoped driver
    // objects and must not outlive the swapchain itself.
    if (context->pacer) {
        context->pacer->notify_destroy_swapchain(swapchain);
    }

    context->vtable.DestroySwapchainKHR(device, swapchain, pAllocator);
}

} // namespace low_latency

#define HOOK_ENTRY(vk_name_literal, fn_sym)                                    \
    {vk_name_literal, reinterpret_cast<PFN_vkVoidFunction>(fn_sym)}

using func_map_t = std::unordered_map<std::string_view, PFN_vkVoidFunction>;
static const auto instance_functions = func_map_t{
    HOOK_ENTRY("vkCreateDevice", low_latency::CreateDevice),

    HOOK_ENTRY("vkGetInstanceProcAddr", LowLatency_GetInstanceProcAddr),
    HOOK_ENTRY("vkGetDeviceProcAddr", LowLatency_GetDeviceProcAddr),

    HOOK_ENTRY("vkEnumeratePhysicalDevices",
               low_latency::EnumeratePhysicalDevices),

    HOOK_ENTRY("vkCreateInstance", low_latency::CreateInstance),
    HOOK_ENTRY("vkDestroyInstance", low_latency::DestroyInstance),

};

static const auto device_functions = func_map_t{
    HOOK_ENTRY("vkGetDeviceProcAddr", LowLatency_GetDeviceProcAddr),

    HOOK_ENTRY("vkDestroyDevice", low_latency::DestroyDevice),

    HOOK_ENTRY("vkGetDeviceQueue", low_latency::GetDeviceQueue),
    HOOK_ENTRY("vkGetDeviceQueue2", low_latency::GetDeviceQueue2),

    HOOK_ENTRY("vkQueueSubmit", low_latency::QueueSubmit),
    HOOK_ENTRY("vkQueueSubmit2", low_latency::QueueSubmit2),
    HOOK_ENTRY("vkQueueSubmit2KHR", low_latency::QueueSubmit2KHR),

    HOOK_ENTRY("vkQueuePresentKHR", low_latency::QueuePresentKHR),

    HOOK_ENTRY("vkCreateSwapchainKHR", low_latency::CreateSwapchainKHR),
    HOOK_ENTRY("vkDestroySwapchainKHR", low_latency::DestroySwapchainKHR),
};
#undef HOOK_ENTRY

VKAPI_ATTR PFN_vkVoidFunction VKAPI_CALL LowLatency_GetDeviceProcAddr(
    VkDevice device, const char* const pName) noexcept {

    if (!pName || !device) {
        return nullptr;
    }

    if (const auto it = device_functions.find(pName);
        it != std::end(device_functions)) {

        return it->second;
    }

    const auto context = low_latency::layer_context.get_context(device);
    return context->vtable.GetDeviceProcAddr(device, pName);
}

VKAPI_ATTR PFN_vkVoidFunction VKAPI_CALL LowLatency_GetInstanceProcAddr(
    VkInstance instance, const char* const pName) noexcept {

    if (!pName) {
        return nullptr;
    }

    if (const auto it = instance_functions.find(pName);
        it != std::end(instance_functions)) {

        return it->second;
    }

    if (!instance) {
        return nullptr;
    }

    const auto context = low_latency::layer_context.get_context(instance);
    return context->vtable.GetInstanceProcAddr(instance, pName);
}
