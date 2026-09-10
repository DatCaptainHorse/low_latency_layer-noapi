#ifndef CONFIG_HH_
#define CONFIG_HH_

#include <chrono>
#include <cstdint>

// Layer-wide configuration, parsed once from the environment.
//
// The layer used to key its behaviour off which vendor extension it was
// pretending to implement. It no longer implements any extension, so every
// knob that used to be inferred from VK_AMD_anti_lag / VK_NV_low_latency2 call
// arguments (enable/disable, FPS cap) now lives here instead.

namespace low_latency {

enum class PacingMode {
    // Loaded, hooks installed, but never blocks the application.
    Off,

    // Hold the application at present until the frame it just presented has
    // drained off the GPU, plus an adaptively probed extra delay. Works
    // anywhere - needs no display feedback at all.
    Drain,

    // Hold the application at present until just late enough that the next
    // frame's work is predicted to land immediately before a display refresh.
    // Requires display feedback (see DisplayFeedback); falls back to Drain
    // when that is unavailable.
    Deadline,

    // Deadline where the present mode is refresh-gated and display feedback is
    // available, Drain otherwise. Decided per swapchain.
    Auto,
};

class Config final {
  private:
    static constexpr auto MODE_ENV = "LOW_LATENCY_LAYER_MODE";
    static constexpr auto FPS_LIMIT_ENV = "LOW_LATENCY_LAYER_FPS_LIMIT";
    static constexpr auto MARGIN_ENV = "LOW_LATENCY_LAYER_MARGIN_US";
    // Opt-out ability incase something doesn't like swapchain interception.
    static constexpr auto NO_PRESENT_TIMING_ENV =
        "LOW_LATENCY_LAYER_NO_PRESENT_TIMING";
    static constexpr auto QUEUE_DEPTH_ENV = "LOW_LATENCY_LAYER_QUEUE_DEPTH";
    static constexpr auto NO_TIMESTAMPS_ENV =
        "LOW_LATENCY_LAYER_NO_TIMESTAMPS";
    static constexpr auto HOTKEY_ENV = "LOW_LATENCY_LAYER_HOTKEY";
    static constexpr auto START_DISABLED_ENV =
        "LOW_LATENCY_LAYER_START_DISABLED";
    static constexpr auto DEBUG_ENV = "LOW_LATENCY_LAYER_DEBUG";

    // Zero: wait for the frame just presented, so the application never has
    // GPU work outstanding when it starts the next one. This is what
    // VK_AMD_anti_lag and VK_NV_low_latency2 both do - they block at the top
    // of the frame until the previous frame's submissions have completed - and
    // it is the setting that measured well on real GPU-bound workloads.
    //
    // One is worth trying where the application's own frame time is close to
    // its GPU time, because zero then leaves the GPU idle for the whole of the
    // application's recording. On a deliberately CPU-heavy synthetic test that
    // cost a third of the frame rate, with observed frames in flight dropping
    // to zero; raising this to one recovered all of it.
    static constexpr auto DEFAULT_QUEUE_DEPTH = 0u;
    static constexpr auto MAX_QUEUE_DEPTH = 3u;

    // Deliberately generous. Undershooting a refresh costs a whole refresh
    // interval of latency plus a visible hitch, which is far worse than
    // leaving a millisecond of slack on the table.
    static constexpr auto DEFAULT_MARGIN_US = 1000u;

  public:
    const PacingMode mode{};

    // 0 means uncapped. Replaces VkAntiLagDataAMD::maxFPS and
    // VkLatencySleepModeInfoNV::minimumIntervalUs.
    const std::uint32_t fps_limit{};

    // How much slack to leave between the predicted completion of a frame and
    // the refresh it is meant to make. Only used by Deadline pacing.
    const std::chrono::microseconds margin{};

    // How many frames of GPU work the application may have outstanding. An
    // exact count, not a delay: the layer waits for the frame that many
    // presents back, so the bound holds regardless of how long anything takes.
    const std::uint32_t queue_depth{};

    // The only source of display timing the layer has, and therefore the only
    // way Deadline pacing can engage. It is very new: the implementation
    // tested against (Mesa 26.3.0-devel, ANV, Wayland) hangs inside
    // vkQueuePresentKHR as soon as a present carries a stage query, so it is
    // off unless asked for. Without it, pacing uses Drain.
    const bool allow_present_timing{};

    // Pace without measuring: no timestamp command buffers are injected into
    // submissions, so pacing loses its GPU-completion signal and degrades to
    // a plain frame limiter. Two uses: isolating the layer's pacing from its
    // measurement when something looks wrong, and working around the abort
    // that VK_LAYER_KHRONOS_validation raises on the injected timestamp
    // command buffers (reproducible with the upstream layer too - see
    // test/paced_present.cc).
    const bool inject_timestamps{};

    // Begin with the effect off, so the hotkey turns it on. Useful for an A/B
    // comparison that starts from the untouched behaviour.
    const bool start_disabled{};

    // Shift plus this function key toggles the layer's effect at runtime.
    // 0 means no hotkey. Defaults to F10 - MangoHud has taken shift+F12.
    const std::uint32_t hotkey_function_key{};

    const bool debug{};

  public:
    explicit Config();
    Config(const Config&) = delete;
    Config(Config&&) = delete;
    Config& operator=(const Config&) = delete;
    Config& operator=(Config&&) = delete;
    ~Config();

    // F-number the hotkey should use, or nothing if disabled.
    static constexpr auto DEFAULT_HOTKEY_FUNCTION_KEY = 10u;

  public:
    std::chrono::nanoseconds min_frametime() const;
};

} // namespace low_latency

#endif
