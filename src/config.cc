#include "config.hh"

#include <algorithm>
#include <charconv>
#include <cstdlib>
#include <string_view>

namespace low_latency {

static std::string_view read_env(const char* const name) {
    const auto env = std::getenv(name);
    return env ? std::string_view{env} : std::string_view{};
}

static bool parse_bool_env(const char* const name) {
    return read_env(name) == "1";
}

static std::uint32_t parse_uint_env(const char* const name,
                                    const std::uint32_t fallback) {
    const auto env = read_env(name);
    if (env.empty()) {
        return fallback;
    }

    auto value = std::uint32_t{};
    const auto [_, ec] =
        std::from_chars(std::begin(env), std::end(env), value);
    return ec == std::errc{} ? value : fallback;
}

// Accepts "F10", "f10", "10" or "off". Only function keys: they are the keys a
// game is least likely to want for itself, and it keeps parsing to one number
// rather than a keyboard layout table.
static std::uint32_t parse_hotkey_env(const char* const name) {
    const auto env = read_env(name);
    if (env.empty()) {
        return Config::DEFAULT_HOTKEY_FUNCTION_KEY;
    }
    if (env == "off" || env == "0" || env == "none") {
        return 0;
    }

    const auto digits =
        env.starts_with("F") || env.starts_with("f") ? env.substr(1) : env;

    auto value = std::uint32_t{};
    const auto [_, ec] =
        std::from_chars(std::begin(digits), std::end(digits), value);
    if (ec != std::errc{} || value < 1 || value > 12) {
        return Config::DEFAULT_HOTKEY_FUNCTION_KEY;
    }
    return value;
}

static PacingMode parse_mode_env(const char* const name) {
    const auto env = read_env(name);
    if (env == "off") {
        return PacingMode::Off;
    }
    if (env == "drain") {
        return PacingMode::Drain;
    }
    if (env == "deadline") {
        return PacingMode::Deadline;
    }
    return PacingMode::Auto;
}

Config::Config()
    : mode(parse_mode_env(MODE_ENV)),
      fps_limit(parse_uint_env(FPS_LIMIT_ENV, 0)),
      margin(parse_uint_env(MARGIN_ENV, DEFAULT_MARGIN_US)),
      queue_depth(std::min(parse_uint_env(QUEUE_DEPTH_ENV,
                                          DEFAULT_QUEUE_DEPTH),
                           MAX_QUEUE_DEPTH)),
      allow_present_timing(!parse_bool_env(NO_PRESENT_TIMING_ENV)),
      inject_timestamps(!parse_bool_env(NO_TIMESTAMPS_ENV)),
      start_disabled(parse_bool_env(START_DISABLED_ENV)),
      hotkey_function_key(parse_hotkey_env(HOTKEY_ENV)),
      debug(parse_bool_env(DEBUG_ENV)) {}

Config::~Config() {}

std::chrono::nanoseconds Config::min_frametime() const {
    using namespace std::chrono;
    if (!this->fps_limit) {
        return 0ns;
    }
    return duration_cast<nanoseconds>(1s) / this->fps_limit;
}

} // namespace low_latency
