#include "layer_context.hh"

#include <cstdio>

#if defined(__linux__)
#include <linux/input-event-codes.h>
#endif

namespace low_latency {

LayerContext::LayerContext()
    : effect_enabled(!this->config.start_disabled) {}

LayerContext::~LayerContext() {}

void LayerContext::ensure_hotkey() {
    std::call_once(this->hotkey_once, [this]() {
        if (!this->config.hotkey_function_key) {
            return;
        }

#if defined(__linux__)
        // KEY_F1..KEY_F10 are contiguous; F11 and F12 sit elsewhere.
        const auto code = [&]() -> std::uint16_t {
            const auto number = this->config.hotkey_function_key;
            if (number <= 10) {
                return static_cast<std::uint16_t>(KEY_F1 + (number - 1));
            }
            return number == 11 ? KEY_F11 : KEY_F12;
        }();

        this->hotkey = std::make_unique<HotkeyMonitor>(
            HotkeyMonitor::Combination{.trigger = code, .needs_shift = true},
            [this]() {
                // Only this thread ever writes it, so a load/store pair is
                // enough and reads more plainly than an exchange.
                const auto enabled =
                    !this->effect_enabled.load(std::memory_order_relaxed);
                this->effect_enabled.store(enabled, std::memory_order_relaxed);

                // The only feedback there is, so it is not gated behind the
                // debug switch.
                std::fprintf(stderr, "[low_latency] effect %s\n",
                             enabled ? "enabled" : "disabled");
            });

        if (this->config.debug) {
            std::fprintf(stderr,
                         "[low_latency] hotkey shift+F%u: %s (%zu devices)\n",
                         this->config.hotkey_function_key,
                         this->hotkey->watching() ? "watching"
                                                  : "no readable input devices",
                         this->hotkey->device_count());
        }
#endif
    });
}

} // namespace low_latency
