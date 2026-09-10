#include "frame_pacer.hh"

#include "device_context.hh"
#include "layer_context.hh"
#include "queue_context.hh"
#include "queue_tracker.hh"

#include <vulkan/utility/vk_struct_helper.hpp>

#include <algorithm>
#include <span>

namespace low_latency {

FramePacer::FramePacer(DeviceContext& device) : device(device) {}

FramePacer::~FramePacer() {}

void FramePacer::notify_create_swapchain(
    const VkSwapchainKHR& swapchain, const VkSwapchainCreateInfoKHR& info) {

    auto pacer = std::make_unique<SwapchainPacer>(this->device, swapchain, info);

    const auto lock = std::scoped_lock{this->mutex};
    this->pacers.insert_or_assign(swapchain, std::move(pacer));
}

void FramePacer::notify_destroy_swapchain(const VkSwapchainKHR& swapchain) {
    // Take the pacer out under the lock but destroy it outside, so a
    // destructor that talks to the driver never runs with our lock held.
    auto pacer = [&]() -> std::unique_ptr<SwapchainPacer> {
        const auto lock = std::scoped_lock{this->mutex};

        const auto iter = this->pacers.find(swapchain);
        if (iter == std::end(this->pacers)) {
            return nullptr;
        }

        auto pacer = std::move(iter->second);
        this->pacers.erase(iter);
        return pacer;
    }();
}

std::vector<std::unique_ptr<SubmissionSpan>> FramePacer::collect_work() const {
    auto work = std::vector<std::unique_ptr<SubmissionSpan>>{};

    const auto lock = std::shared_lock{this->device.mutex};
    for (const auto& [_, queue] : this->device.queues) {
        if (!queue->tracker) {
            continue;
        }
        if (auto span = queue->tracker->take(); span) {
            work.push_back(std::move(span));
        }
    }

    return work;
}

FramePacer::PresentScope::PresentScope(FramePacer& pacer,
                                       std::unique_lock<std::mutex> lock,
                                       const VkPresentInfoKHR* patched,
                                       const VkPresentInfoKHR* original)
    : pacer(pacer), lock(std::move(lock)), patched(patched),
      original(original) {}

void FramePacer::disable_feedback_for(const VkPresentInfoKHR& info) {
    const auto lock = std::shared_lock{this->mutex};

    for (const auto& swapchain :
         std::span{info.pSwapchains, info.swapchainCount}) {

        if (const auto iter = this->pacers.find(swapchain);
            iter != std::end(this->pacers)) {

            iter->second->disable_feedback();
        }
    }
}

FramePacer::PresentScope::~PresentScope() {}

FramePacer::PresentScope
FramePacer::begin_present(const VkPresentInfoKHR& info) {

    auto lock = std::unique_lock{this->present_mutex};

    const auto swapchains = std::span{info.pSwapchains, info.swapchainCount};

    this->in_flight_ids.assign(info.swapchainCount, 0);

    const auto pacers_lock = std::shared_lock{this->mutex};

    // If the application already labels its presents, use its ids rather than
    // fighting it - present ids have to increase strictly and there is only
    // one sequence per swapchain. Either spelling will do as a source of ids,
    // even though the layer only ever adds the newer one.
    const auto app_ids = [&]() -> const std::uint64_t* {
        if (const auto id2 =
                vku::FindStructInPNextChain<VkPresentId2KHR>(info.pNext);
            id2 && id2->pPresentIds) {

            return id2->pPresentIds;
        }
        if (const auto id =
                vku::FindStructInPNextChain<VkPresentIdKHR>(info.pNext);
            id && id->pPresentIds) {

            return id->pPresentIds;
        }
        return nullptr;
    }();

    auto wants_timings = false;
    auto wants_ids = false;
    this->patch.timing_infos.assign(info.swapchainCount,
                                    VkPresentTimingInfoEXT{
                                        .sType =
                                            VK_STRUCTURE_TYPE_PRESENT_TIMING_INFO_EXT,
                                    });

    for (auto i = std::uint32_t{0}; i < info.swapchainCount; ++i) {
        const auto iter = this->pacers.find(swapchains[i]);
        if (iter == std::end(this->pacers)) {
            continue;
        }
        auto& pacer = *iter->second;

        if (app_ids) {
            pacer.adopt_present_id(app_ids[i]);
            this->in_flight_ids[i] = app_ids[i];
        } else if (pacer.wants_present_id()) {
            this->in_flight_ids[i] = pacer.reserve_present_id();
            wants_ids = true;
        }

        wants_timings |= pacer.fill_timing_info(this->patch.timing_infos[i]);
    }

    // Nothing to add - hand the application's own structure straight through.
    const auto needs_ids = !app_ids && wants_ids;
    if (!needs_ids && !wants_timings) {
        return PresentScope{*this, std::move(lock), &info, &info};
    }

    this->patch.info = info;
    auto next = info.pNext;

    if (needs_ids) {
        this->patch.ids = this->in_flight_ids;

        this->patch.present_id2 = VkPresentId2KHR{
            .sType = VK_STRUCTURE_TYPE_PRESENT_ID_2_KHR,
            .pNext = next,
            .swapchainCount = info.swapchainCount,
            .pPresentIds = std::data(this->patch.ids),
        };
        next = &this->patch.present_id2;
    }

    if (wants_timings) {
        this->patch.timings = VkPresentTimingsInfoEXT{
            .sType = VK_STRUCTURE_TYPE_PRESENT_TIMINGS_INFO_EXT,
            .pNext = next,
            .swapchainCount = info.swapchainCount,
            .pTimingInfos = std::data(this->patch.timing_infos),
        };
        next = &this->patch.timings;
    }

    this->patch.info.pNext = next;

    return PresentScope{*this, std::move(lock), &this->patch.info, &info};
}

VkResult FramePacer::PresentScope::finish(const VkResult& result) {
    auto& pacer = this->pacer;

    const auto swapchains =
        std::span{this->original->pSwapchains, this->original->swapchainCount};
    const auto ids = pacer.in_flight_ids;

    // A present can only fail this way because we asked for timings on it, so
    // the application must never see it: it never enabled the extension that
    // defines this error and has no way to interpret it.
    //
    // Reissuing the present without our additions was the obvious recovery and
    // it does not work - a failed present appears to have already taken the
    // wait semaphores with it, so the replay waits on something that will
    // never signal and the application stalls. Instead we stop asking for
    // timings and report out of date, which every application already knows
    // how to handle. That costs one recreated swapchain rather than a hang,
    // and because we give up on timings first it does not repeat.
    const auto effective = [&]() -> VkResult {
        if (result != VK_ERROR_PRESENT_TIMING_QUEUE_FULL_EXT) {
            return result;
        }

        pacer.disable_feedback_for(*this->original);

        if (this->patched == this->original) {
            // Not ours after all - the application asked for timings itself.
            return result;
        }

        return VK_ERROR_OUT_OF_DATE_KHR;
    }();

    const auto presented =
        effective == VK_SUCCESS || effective == VK_SUBOPTIMAL_KHR;

    // Release the patch buffer before pacing - the down-call is done with it,
    // and holding it across a multi-millisecond block would serialise any
    // other thread presenting a different swapchain.
    this->lock.unlock();

    // Take the frame's work regardless of which swapchain we end up pacing on,
    // otherwise spans accumulate forever.
    auto work = pacer.collect_work();

    if (swapchains.empty()) {
        return effective;
    }

    const auto pacers_lock = std::shared_lock{pacer.mutex};

    // Pace on the first swapchain only. An application presenting several at
    // once has no single frame boundary to pace against, and picking one is
    // better than blocking once per swapchain.
    const auto iter = pacer.pacers.find(swapchains.front());
    if (iter == std::end(pacer.pacers)) {
        return effective;
    }

    iter->second->pace(ids.empty() ? 0 : ids.front(), std::move(work),
                       presented);

    return effective;
}

} // namespace low_latency
