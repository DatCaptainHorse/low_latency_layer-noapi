#include "swapchain_pacer.hh"

#include "device_context.hh"
#include "helper.hh"
#include "layer_context.hh"

#include <algorithm>
#include <cstdio>

namespace low_latency {

static bool is_present_mode_refresh_gated(const VkPresentModeKHR& mode) {
    switch (mode) {
    case VK_PRESENT_MODE_FIFO_KHR:
    case VK_PRESENT_MODE_FIFO_RELAXED_KHR:
    case VK_PRESENT_MODE_FIFO_LATEST_READY_KHR:
        return true;
    default:
        // IMMEDIATE and MAILBOX hand the frame over as soon as it is ready, so
        // there is no refresh boundary to aim for - holding a frame back only
        // delays it.
        return false;
    }
}

SwapchainPacer::SwapchainPacer(DeviceContext& device,
                               const VkSwapchainKHR& swapchain,
                               const VkSwapchainCreateInfoKHR& info)
    : device(device), swapchain(swapchain), present_mode(info.presentMode),
      is_refresh_gated(is_present_mode_refresh_gated(info.presentMode)),
      surface_support(SurfaceFeedbackSupport::query(device, info.surface)),
      last_report(DeviceClock::now()) {

    // Display feedback is only worth setting up where we would act on it.
    if (this->is_refresh_gated && this->config().mode != PacingMode::Off &&
        this->config().mode != PacingMode::Drain) {

        this->feedback =
            DisplayFeedback::create(device, swapchain, this->surface_support);
    }

    if (this->config().debug) {
        std::fprintf(stderr,
                     "[low_latency] swapchain %p: present mode %d, "
                     "refresh gated %d, feedback %s\n",
                     static_cast<const void*>(swapchain),
                     static_cast<int>(this->present_mode),
                     static_cast<int>(this->is_refresh_gated),
                     this->feedback ? this->feedback->name() : "none");
    }
}

SwapchainPacer::~SwapchainPacer() {}

const Config& SwapchainPacer::config() const {
    return this->device.instance.layer.config;
}

PacingMode SwapchainPacer::effective_mode() const {
    switch (this->config().mode) {
    case PacingMode::Off:
        return PacingMode::Off;
    case PacingMode::Drain:
        return PacingMode::Drain;
    case PacingMode::Deadline:
    case PacingMode::Auto:
        break;
    }

    // Deadline pacing needs a refresh period and phase that were measured, not
    // guessed, plus a frame time to work back from. Missing any of them, the
    // honest answer is to do the part that needs none of it.
    if (!this->is_refresh_gated || !this->feedback ||
        !this->vblank.is_usable() || !this->last_work) {

        return PacingMode::Drain;
    }
    return PacingMode::Deadline;
}

bool SwapchainPacer::wants_present_id() const {
    return this->feedback != nullptr;
}

bool SwapchainPacer::uses_present_id2() const {
    return this->surface_support.present_id2;
}

std::uint64_t SwapchainPacer::reserve_present_id() {
    return this->next_present_id++;
}

void SwapchainPacer::adopt_present_id(const std::uint64_t& present_id) {
    // Present ids have to increase strictly, so never hand out one the
    // application has already used.
    this->next_present_id = std::max(this->next_present_id, present_id + 1);
}

bool SwapchainPacer::fill_timing_info(VkPresentTimingInfoEXT& info) const {
    return this->feedback && this->feedback->fill_timing_info(info);
}

void SwapchainPacer::disable_feedback() {
    if (!this->feedback) {
        return;
    }

    if (this->config().debug) {
        std::fprintf(stderr, "[low_latency] swapchain %p: dropping %s\n",
                     static_cast<const void*>(this->swapchain),
                     this->feedback->name());
    }

    this->feedback.reset();
    this->vblank = VblankModel{};
}

std::optional<std::pair<DeviceClock::time_point, DeviceClock::time_point>>
SwapchainPacer::await_work(
    const std::vector<std::unique_ptr<SubmissionSpan>>& work) const {

    auto earliest = std::optional<DeviceClock::time_point>{};
    auto latest = std::optional<DeviceClock::time_point>{};

    for (const auto& submission_span : work) {
        if (!submission_span) {
            continue;
        }

        // Both ends come from timestamp queries written into the application's
        // own submissions, so they are the GPU's own account of the frame.
        const auto [start, end] = submission_span->await_completed();
        earliest = earliest ? std::min(*earliest, start) : start;
        latest = latest ? std::max(*latest, end) : end;
    }

    if (!earliest || !latest) {
        return std::nullopt;
    }
    return std::pair{*earliest, *latest};
}

std::optional<DeviceClock::time_point>
SwapchainPacer::deadline_release() const {

    const auto interval = this->vblank.interval();
    if (!interval || !this->last_work) {
        return std::nullopt;
    }

    // What the previous frame took from release to being finished on the GPU,
    // plus slack. Undershooting a refresh costs a whole refresh interval, so
    // the slack is deliberately on the generous side.
    const auto need = *this->last_work + this->config().margin;

    const auto now = DeviceClock::now();

    // Aim at the first refresh we could still make if released right now.
    const auto boundary = this->vblank.boundary_at_or_after(now + need);
    if (!boundary) {
        return std::nullopt;
    }

    const auto release = *boundary - need;

    // Exact bound, not a heuristic: aligning to a grid of period P can never
    // require holding for as long as P. Anything beyond that means the phase
    // we measured has gone stale, so release instead of stalling.
    if (release > now + *interval) {
        return std::nullopt;
    }

    return release;
}

void SwapchainPacer::report() {
    using namespace std::chrono;

    ++this->frame_counter;

    const auto now = DeviceClock::now();
    if (now - this->last_report < seconds{1}) {
        return;
    }
    this->last_report = now;

    const auto to_ms = [](const auto& duration) {
        return static_cast<double>(
                   duration_cast<nanoseconds>(duration).count()) /
               1.0e6;
    };

    const auto mode = [&]() -> const char* {
        switch (this->effective_mode()) {
        case PacingMode::Off:
            return "off";
        case PacingMode::Drain:
            return "drain";
        case PacingMode::Deadline:
            return "deadline";
        case PacingMode::Auto:
            return "auto";
        }
        return "?";
    }();

    std::fprintf(stderr,
                 "[low_latency] %s frames=%llu work=%.2fms gpuwait=%.2fms "
                 "gpugap=%.2fms hold=%.2fms depth=%u period=%s phase=%s\n",
                 mode, static_cast<unsigned long long>(this->frame_counter),
                 this->last_work ? to_ms(*this->last_work) : 0.0,
                 to_ms(this->last_gpu_wait), to_ms(this->last_gpu_gap),
                 to_ms(this->last_hold), this->config().queue_depth,
                 this->vblank.interval()
                     ? std::to_string(to_ms(*this->vblank.interval())).c_str()
                     : "none",
                 this->vblank.is_usable() ? "locked" : "unlocked");
}

void SwapchainPacer::pace(const std::uint64_t& present_id,
                          std::vector<std::unique_ptr<SubmissionSpan>> work,
                          const bool presented) {

    const auto& config = this->config();

    // A present that did not go through says nothing about where the display
    // is, so the phase goes rather than being paced against.
    if (!presented) {
        this->vblank.reset_phase();
    }

    if (this->feedback) {
        if (presented) {
            this->feedback->notify_presented(present_id);
        }

        this->sample_scratch.clear();
        this->feedback->poll(this->sample_scratch);
        for (const auto& sample : this->sample_scratch) {
            this->vblank.add_sample(sample);
        }

        this->vblank.set_nominal_interval(this->feedback->refresh_interval());

        // Drop a source that has stopped working rather than carrying on
        // requesting data nobody reads.
        if (!this->feedback->healthy()) {
            this->disable_feedback();
        }
    }

    if (config.mode == PacingMode::Off) {
        return;
    }

    // Toggled off at runtime. The frame's spans are dropped rather than waited
    // for: destroying a timestamp handle hands it to the pool's reaper, which
    // does the waiting on its own thread, so this does not block. The
    // measurements go with them, because they would otherwise be stitched
    // across a gap in which the application ran unpaced.
    if (!this->device.instance.layer.effect_enabled.load(
            std::memory_order_relaxed)) {

        this->pending.clear();
        this->last_release.reset();
        this->last_work.reset();
        this->last_gpu_end.reset();
        this->last_gpu_wait = DeviceClock::duration::zero();
        this->last_hold = DeviceClock::duration::zero();
        return;
    }

    // The whole latency mechanism: allow the application at most
    // `queue_depth` frames of outstanding GPU work, by waiting for the frame
    // that many presents back. An exact count rather than a delay, so nothing
    // has to be predicted or tuned.
    this->pending.push_back(PendingFrame{
        .release = this->last_release.value_or(DeviceClock::now()),
        .work = std::move(work),
    });

    const auto wait_began = DeviceClock::now();

    while (std::size(this->pending) > config.queue_depth) {
        const auto frame = std::move(this->pending.front());
        this->pending.pop_front();

        const auto times = this->await_work(frame.work);
        if (!times) {
            continue;
        }
        const auto [gpu_start, gpu_end] = *times;

        // How long the GPU had nothing to do between this frame and the one
        // before it. Above zero means the bound is starving it.
        if (this->last_gpu_end) {
            this->last_gpu_gap =
                std::max(gpu_start - *this->last_gpu_end,
                         DeviceClock::duration::zero());
        }
        this->last_gpu_end = gpu_end;

        if (const auto elapsed = gpu_end - frame.release;
            elapsed > DeviceClock::duration::zero()) {

            this->last_work = elapsed;
        }
    }

    this->last_gpu_wait = DeviceClock::now() - wait_began;

    if (!presented) {
        this->last_release = DeviceClock::now();
        this->last_hold = DeviceClock::duration::zero();
        return;
    }

    const auto hold_began = DeviceClock::now();

    // Release as soon as the GPU is drained, unless something measured says
    // later. Both candidates below are exact, so whichever is later wins and
    // neither can creep.
    auto release_at = hold_began;

    if (const auto min_frametime = config.min_frametime();
        min_frametime > DeviceClock::duration::zero() && this->last_release) {

        release_at = std::max(release_at, *this->last_release + min_frametime);
    }

    if (this->effective_mode() == PacingMode::Deadline) {
        if (const auto deadline = this->deadline_release(); deadline) {
            release_at = std::max(release_at, *deadline);
        }
    }

    if (release_at > hold_began) {
        precise_wait_until(release_at);
    }

    this->last_release = DeviceClock::now();
    this->last_hold = *this->last_release - hold_began;

    if (config.debug) {
        this->report();
    }
}

} // namespace low_latency
