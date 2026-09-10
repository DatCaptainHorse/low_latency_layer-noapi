#include "hotkey.hh"

#if defined(__linux__)
#include <fcntl.h>
#include <linux/input.h>
#include <sys/ioctl.h>
#include <unistd.h>
#endif

#include <array>
#include <filesystem>
#include <utility>

namespace low_latency {

#if defined(__linux__)

namespace {

using key_bitmap_t = std::array<std::uint8_t, (KEY_MAX + 8) / 8>;

bool test_bit(const key_bitmap_t& bitmap, const std::uint16_t code) {
    if (code / 8 >= std::size(bitmap)) {
        return false;
    }
    return (bitmap[code / 8] >> (code % 8)) & 1;
}

// A device is worth watching if it can report every key in the combination.
// Anything else - the volume knob, the mouse's motion device - is skipped.
bool reports_keys(const int descriptor,
                  const HotkeyMonitor::Combination& combination) {

    auto capabilities = key_bitmap_t{};
    if (ioctl(descriptor, EVIOCGBIT(EV_KEY, std::size(capabilities)),
              std::data(capabilities)) < 0) {
        return false;
    }

    if (!test_bit(capabilities, combination.trigger)) {
        return false;
    }

    if (combination.needs_shift &&
        !test_bit(capabilities, KEY_LEFTSHIFT) &&
        !test_bit(capabilities, KEY_RIGHTSHIFT)) {
        return false;
    }

    return true;
}

} // namespace

HotkeyMonitor::HotkeyMonitor(const Combination& combination, Callback callback)
    : combination(combination), callback(std::move(callback)) {

    auto error = std::error_code{};
    for (const auto& entry :
         std::filesystem::directory_iterator{"/dev/input", error}) {

        if (entry.path().filename().string().rfind("event", 0) != 0) {
            continue;
        }

        const auto descriptor =
            open(entry.path().c_str(), O_RDONLY | O_NONBLOCK | O_CLOEXEC);
        if (descriptor < 0) {
            // Almost always a permissions problem, and not worth complaining
            // about per device.
            continue;
        }

        if (!reports_keys(descriptor, this->combination)) {
            close(descriptor);
            continue;
        }

        this->descriptors.push_back(descriptor);
    }

    if (this->descriptors.empty()) {
        return;
    }

    this->worker = std::jthread{std::bind_front(&HotkeyMonitor::run, this)};
}

HotkeyMonitor::~HotkeyMonitor() {
    // The worker is a member declared after the descriptors, so it has already
    // been stopped and joined by the time this runs.
    for (const auto& descriptor : this->descriptors) {
        close(descriptor);
    }
}

bool HotkeyMonitor::is_pressed() const {
    // Held across all watched devices: the trigger and the modifier can come
    // from different ones.
    auto trigger_down = false;
    auto shift_down = false;

    for (const auto& descriptor : this->descriptors) {
        auto state = key_bitmap_t{};
        if (ioctl(descriptor, EVIOCGKEY(std::size(state)), std::data(state)) <
            0) {
            continue;
        }

        trigger_down =
            trigger_down || test_bit(state, this->combination.trigger);
        shift_down = shift_down || test_bit(state, KEY_LEFTSHIFT) ||
                     test_bit(state, KEY_RIGHTSHIFT);
    }

    if (this->combination.needs_shift && !shift_down) {
        return false;
    }
    return trigger_down;
}

void HotkeyMonitor::run(const std::stop_token stoken) {
    while (!stoken.stop_requested()) {
        const auto pressed = this->is_pressed();

        // Edge triggered, so holding the combination toggles once.
        if (pressed && !this->was_pressed && this->callback) {
            this->callback();
        }
        this->was_pressed = pressed;

        std::this_thread::sleep_for(POLL_PERIOD);
    }
}

#else

// No evdev, no hotkey. The layer is Linux-only in practice - DeviceClock reads
// CLOCK_MONOTONIC directly - but this keeps the dependency explicit.
HotkeyMonitor::HotkeyMonitor(const Combination& combination, Callback callback)
    : combination(combination), callback(std::move(callback)) {}

HotkeyMonitor::~HotkeyMonitor() {}

bool HotkeyMonitor::is_pressed() const { return false; }

void HotkeyMonitor::run(const std::stop_token) {}

#endif

} // namespace low_latency
