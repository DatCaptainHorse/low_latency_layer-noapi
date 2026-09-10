#include "display_feedback.hh"

#include "config.hh"
#include "device_context.hh"
#include "layer_context.hh"

#include <cstdio>
#include <string>
#include <utility>

#include <vulkan/utility/vk_struct_helper.hpp>

#include <algorithm>
#include <array>
#include <ranges>
#include <span>

namespace low_latency {

DisplayFeedback::DisplayFeedback(const DeviceContext& device,
                                 const VkSwapchainKHR& swapchain)
    : device(device), swapchain(swapchain) {}

DisplayFeedback::~DisplayFeedback() {}

bool DisplayFeedback::fill_timing_info(VkPresentTimingInfoEXT&) const {
    return false;
}

std::optional<DeviceClock::duration> DisplayFeedback::refresh_interval() {
    return std::nullopt;
}

SurfaceFeedbackSupport
SurfaceFeedbackSupport::query(const DeviceContext& device,
                              const VkSurfaceKHR& surface) {

    const auto& vtable = device.instance.vtable;

    // The layer appends VK_KHR_get_surface_capabilities2 at instance creation
    // so this is normally present, but an application that created its
    // instance before us, or a loader that refused the addition, would leave
    // it null.
    if (!vtable.GetPhysicalDeviceSurfaceCapabilities2KHR || !surface) {
        return {};
    }

    const auto& enabled = device.display_extensions;

    auto timing = VkPresentTimingSurfaceCapabilitiesEXT{
        .sType = VK_STRUCTURE_TYPE_PRESENT_TIMING_SURFACE_CAPABILITIES_EXT,
    };
    auto id2 = VkSurfaceCapabilitiesPresentId2KHR{
        .sType = VK_STRUCTURE_TYPE_SURFACE_CAPABILITIES_PRESENT_ID_2_KHR,
    };

    auto caps = VkSurfaceCapabilities2KHR{
        .sType = VK_STRUCTURE_TYPE_SURFACE_CAPABILITIES_2_KHR,
    };

    // Only chain what we actually enabled - querying a capability whose
    // extension is not enabled is itself invalid.
    auto** tail = &caps.pNext;
    const auto chain = [&](auto& structure, const bool wanted) {
        if (!wanted) {
            return;
        }
        *tail = &structure;
        tail = const_cast<void**>(&structure.pNext);
    };
    chain(id2, enabled.present_id2);
    chain(timing, enabled.present_timing);

    const auto surface_info = VkPhysicalDeviceSurfaceInfo2KHR{
        .sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_SURFACE_INFO_2_KHR,
        .surface = surface,
    };

    if (vtable.GetPhysicalDeviceSurfaceCapabilities2KHR(
            device.physical_device.physical_device, &surface_info, &caps) !=
        VK_SUCCESS) {

        return {};
    }

    return {
        .present_id2 = enabled.present_id2 &&
                       static_cast<bool>(id2.presentId2Supported),
        .present_timing = enabled.present_timing &&
                          static_cast<bool>(timing.presentTimingSupported),
        .present_stages = timing.presentStageQueries,
    };
}

std::unique_ptr<DisplayFeedback>
DisplayFeedback::create(const DeviceContext& device,
                        const VkSwapchainKHR& swapchain,
                        const SurfaceFeedbackSupport& surface_support) {

    if (!surface_support.present_timing) {
        return nullptr;
    }

    return PresentTimingFeedback::create(device, swapchain, surface_support);
}

// ---------------------------------------------------------------------------
// VK_EXT_present_timing
// ---------------------------------------------------------------------------

PresentTimingFeedback::PresentTimingFeedback(
    const DeviceContext& device, const VkSwapchainKHR& swapchain,
    const VkPresentStageFlagsEXT& stage, const VkTimeDomainKHR& time_domain,
    const std::uint64_t& time_domain_id)
    : DisplayFeedback(device, swapchain), stage(stage),
      time_domain(time_domain), time_domain_id(time_domain_id) {}

PresentTimingFeedback::~PresentTimingFeedback() {}

const char* PresentTimingFeedback::name() const {
    return VK_EXT_PRESENT_TIMING_EXTENSION_NAME;
}

std::unique_ptr<PresentTimingFeedback>
PresentTimingFeedback::create(const DeviceContext& device,
                              const VkSwapchainKHR& swapchain,
                              const SurfaceFeedbackSupport& surface_support) {

    const auto& vtable = device.vtable;

    // Says why we could not use present timing, since it is the difference
    // between pacing off real display timestamps and pacing off "we noticed
    // the present completed at some point before now".
    const auto reject = [&](const char* const why) {
        if (device.instance.layer.config.debug) {
            std::fprintf(stderr, "[low_latency] present timing unavailable: %s\n",
                         why);
        }
        return nullptr;
    };

    if (!vtable.GetSwapchainTimeDomainPropertiesEXT ||
        !vtable.GetPastPresentationTimingEXT) {

        return reject("entry points missing");
    }

    // Requesting stage queries means supplying a VkPresentTimingInfoEXT, and
    // the only safe way to fill in its target time needs this.
    if (!device.display_extensions.present_at_relative_time) {
        return reject("relative present scheduling unavailable");
    }

    // Take the most meaningful stage the surface can actually report.
    const auto stage = [&]() -> VkPresentStageFlagsEXT {
        for (const auto& candidate : STAGE_PREFERENCE) {
            if (surface_support.present_stages & candidate) {
                return candidate;
            }
        }
        return 0;
    }();
    if (!stage) {
        return reject("surface reports no usable present stage");
    }

    // Find a time domain we can convert into DeviceClock's base. Monotonic is
    // a straight reinterpretation; the device domain goes through the same
    // calibration our GPU timestamps use. The swapchain-local domains carry no
    // relation to either, so they are no use for pacing.
    const auto domain = [&]() -> std::optional<
                                  std::pair<VkTimeDomainKHR, std::uint64_t>> {
        auto counter = std::uint64_t{};
        auto properties = VkSwapchainTimeDomainPropertiesEXT{
            .sType = VK_STRUCTURE_TYPE_SWAPCHAIN_TIME_DOMAIN_PROPERTIES_EXT,
        };
        if (vtable.GetSwapchainTimeDomainPropertiesEXT(
                device.device, swapchain, &properties, &counter) !=
            VK_SUCCESS) {

            return std::nullopt;
        }

        auto domains = std::vector<VkTimeDomainKHR>(properties.timeDomainCount);
        auto ids = std::vector<std::uint64_t>(properties.timeDomainCount);
        properties.pTimeDomains = std::data(domains);
        properties.pTimeDomainIds = std::data(ids);
        if (vtable.GetSwapchainTimeDomainPropertiesEXT(
                device.device, swapchain, &properties, &counter) !=
            VK_SUCCESS) {

            return std::nullopt;
        }

        for (const auto& wanted : DOMAIN_PREFERENCE) {
            for (const auto& [candidate, id] : std::views::zip(domains, ids)) {
                if (candidate == wanted) {
                    return std::pair{candidate, id};
                }
            }
        }
        return std::nullopt;
    }();
    if (!domain) {
        return reject("no time domain we can place on our clock");
    }

    // Best effort - the driver has a default queue size and a failure here
    // only costs us dropped samples.
    if (vtable.SetSwapchainPresentTimingQueueSizeEXT) {
        vtable.SetSwapchainPresentTimingQueueSizeEXT(device.device, swapchain,
                                                     TIMING_QUEUE_SIZE);
    }

    auto feedback = std::make_unique<PresentTimingFeedback>(
        device, swapchain, stage, domain->first, domain->second);

    // Worth logging separately from the sample stream: the refresh period is
    // the half of the puzzle that nothing else on a compositor session can
    // supply, so whether the driver answers this at all decides whether
    // deadline pacing is reachable.
    if (device.instance.layer.config.debug) {
        const auto interval = feedback->refresh_interval();
        std::fprintf(stderr,
                     "[low_latency] present timing: stage=0x%x domain=%d "
                     "refreshDuration=%s\n",
                     stage, static_cast<int>(domain->first),
                     interval ? std::to_string(
                                    static_cast<double>(interval->count()) /
                                    1.0e6)
                                    .append(" ms")
                                    .c_str()
                              : "unavailable");
    }

    return feedback;
}

// Asking only for the stage we intend to read back keeps the driver from
// having to record everything.
bool PresentTimingFeedback::fill_timing_info(
    VkPresentTimingInfoEXT& info) const {

    // We want the stage queries and nothing else. There is no "do not
    // schedule" flag, so say it as a relative target of zero - meaning "no
    // earlier bound than now" - which is well defined and needs only the
    // relative scheduling feature. Leaving targetTime unqualified would be an
    // absolute time in a domain whose epoch we have no business guessing.
    info = VkPresentTimingInfoEXT{
        .sType = VK_STRUCTURE_TYPE_PRESENT_TIMING_INFO_EXT,
        .flags = VK_PRESENT_TIMING_INFO_PRESENT_AT_RELATIVE_TIME_BIT_EXT,
        .targetTime = 0,
        .timeDomainId = this->time_domain_id,
        .presentStageQueries = this->stage,
        .targetTimeDomainPresentStage = this->stage,
    };
    return true;
}

// Nothing to record - the driver keeps the queue for us and hands back present
// ids with the results.
void PresentTimingFeedback::notify_presented(const std::uint64_t&) {}

bool PresentTimingFeedback::needs_calibration() const {
    switch (this->time_domain) {
    case VK_TIME_DOMAIN_PRESENT_STAGE_LOCAL_EXT:
    case VK_TIME_DOMAIN_SWAPCHAIN_LOCAL_EXT:
        return true;
    default:
        return false;
    }
}

// Puts a present-stage-local domain on our axis. These domains report
// nanoseconds against an unspecified epoch, so a single calibrated pair of
// (local, monotonic) readings is enough to place every later sample - which is
// the same trick DeviceClock uses for the device domain.
void PresentTimingFeedback::calibrate() {
    const auto& vtable = this->device.vtable;

    const auto get_calibrated_timestamps =
        vtable.GetCalibratedTimestampsKHR ? vtable.GetCalibratedTimestampsKHR
                                          : vtable.GetCalibratedTimestampsEXT;
    if (!get_calibrated_timestamps) {
        return;
    }

    const auto swapchain_info = VkSwapchainCalibratedTimestampInfoEXT{
        .sType = VK_STRUCTURE_TYPE_SWAPCHAIN_CALIBRATED_TIMESTAMP_INFO_EXT,
        .swapchain = this->swapchain,
        .presentStage = this->stage,
        .timeDomainId = this->time_domain_id,
    };

    const auto infos = std::array{
        VkCalibratedTimestampInfoKHR{
            .sType = VK_STRUCTURE_TYPE_CALIBRATED_TIMESTAMP_INFO_KHR,
            .pNext = &swapchain_info,
            .timeDomain = this->time_domain,
        },
        VkCalibratedTimestampInfoKHR{
            .sType = VK_STRUCTURE_TYPE_CALIBRATED_TIMESTAMP_INFO_KHR,
            .timeDomain = VK_TIME_DOMAIN_CLOCK_MONOTONIC_KHR,
        },
    };

    auto timestamps = std::array<std::uint64_t, 2>{};
    auto deviation = std::uint64_t{};
    if (get_calibrated_timestamps(this->device.device,
                                  static_cast<std::uint32_t>(std::size(infos)),
                                  std::data(infos), std::data(timestamps),
                                  &deviation) != VK_SUCCESS) {
        return;
    }

    using namespace std::chrono;
    this->calibration_offset =
        nanoseconds{timestamps[1]} - nanoseconds{timestamps[0]};
    this->last_calibration = DeviceClock::now();
    this->has_calibration = true;
}

std::optional<DeviceClock::time_point>
PresentTimingFeedback::to_time_point(
    const VkPastPresentationTimingEXT& timing) const {

    const auto stages =
        std::span{timing.pPresentStages, timing.presentStageCount};
    const auto iter = std::ranges::find_if(stages, [&](const auto& entry) {
        return entry.stage & this->stage;
    });
    if (iter == std::end(stages)) {
        return std::nullopt;
    }

    switch (timing.timeDomain) {
    case VK_TIME_DOMAIN_CLOCK_MONOTONIC_KHR:
        return DeviceClock::time_point{std::chrono::nanoseconds{iter->time}};
    case VK_TIME_DOMAIN_DEVICE_KHR:
        assert(this->device.clock);
        return this->device.clock->ticks_to_time(iter->time);
    case VK_TIME_DOMAIN_PRESENT_STAGE_LOCAL_EXT:
    case VK_TIME_DOMAIN_SWAPCHAIN_LOCAL_EXT:
        if (!this->has_calibration) {
            return std::nullopt;
        }
        return DeviceClock::time_point{std::chrono::nanoseconds{iter->time} +
                                       this->calibration_offset};
    default:
        // A domain we did not ask for and cannot place on our axis.
        return std::nullopt;
    }
}

bool PresentTimingFeedback::healthy() const {
    return this->poll_failures < MAX_POLL_FAILURES;
}

void PresentTimingFeedback::note_poll_failure(const VkResult& result) {
    ++this->poll_failures;

    if (this->device.instance.layer.config.debug &&
        this->poll_failures == MAX_POLL_FAILURES) {

        std::fprintf(stderr,
                     "[low_latency] giving up on present timing: "
                     "vkGetPastPresentationTimingEXT returned %d\n",
                     static_cast<int>(result));
    }
}

void PresentTimingFeedback::poll(std::vector<Sample>& out) {
    const auto& vtable = this->device.vtable;

    if (!this->healthy()) {
        return;
    }

    if (this->needs_calibration() &&
        (!this->has_calibration ||
         DeviceClock::now() - this->last_calibration > CALIBRATION_PERIOD)) {

        this->calibrate();
    }

    const auto info = VkPastPresentationTimingInfoEXT{
        .sType = VK_STRUCTURE_TYPE_PAST_PRESENTATION_TIMING_INFO_EXT,
        .swapchain = this->swapchain,
    };

    auto properties = VkPastPresentationTimingPropertiesEXT{
        .sType = VK_STRUCTURE_TYPE_PAST_PRESENTATION_TIMING_PROPERTIES_EXT,
    };
    if (const auto result = vtable.GetPastPresentationTimingEXT(
            this->device.device, &info, &properties);
        result != VK_SUCCESS) {

        this->note_poll_failure(result);
        return;
    }

    const auto count = properties.presentationTimingCount;
    if (!count) {
        // Still worth noticing a mode change that invalidated our cache.
        if (properties.timingPropertiesCounter !=
            this->timing_properties_counter) {

            this->cached_refresh_interval.reset();
        }
        return;
    }

    // One VkPresentStageTimeEXT slot per requested stage, and we only ever
    // request one.
    this->timing_scratch.assign(count, VkPastPresentationTimingEXT{});
    this->stage_scratch.assign(count, VkPresentStageTimeEXT{});
    for (auto i = std::uint32_t{0}; i < count; ++i) {
        this->timing_scratch[i].sType =
            VK_STRUCTURE_TYPE_PAST_PRESENTATION_TIMING_EXT;
        this->timing_scratch[i].presentStageCount = 1;
        this->timing_scratch[i].pPresentStages = &this->stage_scratch[i];
    }
    properties.pPresentationTimings = std::data(this->timing_scratch);

    if (const auto result = vtable.GetPastPresentationTimingEXT(
            this->device.device, &info, &properties);
        result != VK_SUCCESS) {

        this->note_poll_failure(result);
        return;
    }

    this->poll_failures = 0;

    if (properties.timingPropertiesCounter != this->timing_properties_counter) {
        this->cached_refresh_interval.reset();
    }

    for (const auto& timing :
         std::span{std::data(this->timing_scratch),
                   properties.presentationTimingCount}) {

        if (!timing.reportComplete) {
            continue;
        }
        if (const auto displayed = this->to_time_point(timing); displayed) {
            out.push_back(Sample{
                .present_id = timing.presentId,
                .displayed = *displayed,
            });
        }
    }
}

std::optional<DeviceClock::duration>
PresentTimingFeedback::refresh_interval() {
    if (this->cached_refresh_interval) {
        return this->cached_refresh_interval;
    }

    const auto& vtable = this->device.vtable;
    if (!vtable.GetSwapchainTimingPropertiesEXT) {
        return std::nullopt;
    }

    auto properties = VkSwapchainTimingPropertiesEXT{
        .sType = VK_STRUCTURE_TYPE_SWAPCHAIN_TIMING_PROPERTIES_EXT,
    };
    const auto result = vtable.GetSwapchainTimingPropertiesEXT(
        this->device.device, this->swapchain, &properties,
        &this->timing_properties_counter);

    if (this->device.instance.layer.config.debug &&
        !std::exchange(this->reported_timing_properties, true)) {

        std::fprintf(stderr,
                     "[low_latency] GetSwapchainTimingPropertiesEXT -> %d, "
                     "refreshDuration=%llu refreshInterval=%llu\n",
                     static_cast<int>(result),
                     static_cast<unsigned long long>(properties.refreshDuration),
                     static_cast<unsigned long long>(properties.refreshInterval));
    }

    if (result != VK_SUCCESS || !properties.refreshDuration) {
        return std::nullopt;
    }

    this->cached_refresh_interval =
        std::chrono::nanoseconds{properties.refreshDuration};
    return this->cached_refresh_interval;
}

} // namespace low_latency
