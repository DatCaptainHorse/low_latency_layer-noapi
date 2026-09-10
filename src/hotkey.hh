#ifndef HOTKEY_HH_
#define HOTKEY_HH_

#include <cstdint>
#include <functional>
#include <thread>
#include <vector>

// A global hotkey, for turning the layer's effect on and off while something
// is running.
//
// Linux only, and read straight from the kernel's evdev interface. The
// alternatives do not work where this is most needed: X11 key polling needs a
// reachable display, which an application forced onto Wayland does not have,
// and Wayland deliberately gives clients no way to observe keys they are not
// focused for. evdev is indifferent to both.
//
// Rather than consuming the event stream, this polls each device's current key
// state with EVIOCGKEY. Nothing is grabbed and no events are taken from
// whoever else is reading them, which matters for a layer sitting underneath a
// game that wants its own input.
//
// Reading /dev/input needs permission. Membership of the `input` group grants
// it, and so does logind, which puts an ACL on the input devices of whoever
// holds the active seat - which is why this usually works for a desktop user
// who is in no special group at all. Where it does not, the monitor simply
// finds no devices and stays inert.

namespace low_latency {

class HotkeyMonitor final {
  public:
    using Callback = std::function<void()>;

    // Modifier plus one key. Values are evdev key codes.
    struct Combination final {
        std::uint16_t trigger{};
        bool needs_shift{};
    };

  private:
    // Fast enough that a deliberate keypress is never missed, slow enough to
    // be free: a handful of ioctls per tick.
    static constexpr auto POLL_PERIOD = std::chrono::milliseconds{16};

  private:
    const Combination combination{};
    const Callback callback{};

    // Every readable device that can report the keys we care about. Plural
    // because "the keyboard" is not one device - a keyboard with media keys
    // presents several, and mice report keys too.
    std::vector<int> descriptors{};

    bool was_pressed{};

    void run(const std::stop_token stoken);
    bool is_pressed() const;

    // Declared last so it is destroyed - and therefore stopped and joined -
    // before the descriptors it reads from are closed.
    std::jthread worker{};

  public:
    explicit HotkeyMonitor(const Combination& combination, Callback callback);
    HotkeyMonitor(const HotkeyMonitor&) = delete;
    HotkeyMonitor(HotkeyMonitor&&) = delete;
    HotkeyMonitor& operator=(const HotkeyMonitor&) = delete;
    HotkeyMonitor& operator=(HotkeyMonitor&&) = delete;
    ~HotkeyMonitor();

  public:
    // False when no usable input device could be opened, which is not an
    // error - it just means no hotkey.
    bool watching() const { return !this->descriptors.empty(); }

    std::size_t device_count() const { return std::size(this->descriptors); }
};

} // namespace low_latency

#endif
