#include "vblank_model.hh"

#include <algorithm>
#include <limits>
#include <utility>

namespace low_latency {

VblankModel::VblankModel() {}

VblankModel::~VblankModel() {}

void VblankModel::set_nominal_interval(
    const std::optional<DeviceClock::duration>& in) {

    if (in && (*in < MIN_INTERVAL || *in > MAX_INTERVAL)) {
        this->nominal_interval.reset();
        return;
    }
    this->nominal_interval = in;
}

void VblankModel::add_sample(const DisplayFeedback::Sample& sample) {
    const auto previous_displayed =
        std::exchange(this->last_displayed, sample.displayed);
    const auto previous_id =
        std::exchange(this->last_present_id, sample.present_id);

    if (!previous_displayed || !previous_id) {
        return;
    }

    // Samples can arrive out of order, and a frame that was never displayed
    // leaves a gap. Neither tells us anything about the interval.
    if (sample.present_id <= *previous_id) {
        return;
    }
    const auto frames = sample.present_id - *previous_id;
    if (frames != 1) {
        return;
    }

    const auto delta = sample.displayed - *previous_displayed;
    this->last_delta = delta;
    if (delta < MIN_INTERVAL || delta > MAX_INTERVAL) {
        ++this->rejected_count;
        this->sample_count = 0;
        return;
    }

    // Saturate rather than wrap - this only gates trust.
    this->sample_count =
        std::min(this->sample_count + 1, std::numeric_limits<
                                             decltype(this->sample_count)>::max());
}

void VblankModel::reset_phase() {
    this->last_displayed.reset();
    this->last_present_id.reset();
}

std::optional<DeviceClock::duration> VblankModel::interval() const {
    return this->nominal_interval;
}

bool VblankModel::is_usable() const {
    // The period comes from the driver, but the phase still has to be observed.
    return this->last_displayed.has_value() && this->interval().has_value() &&
           this->sample_count >= MIN_SAMPLES;
}

std::optional<DeviceClock::time_point>
VblankModel::boundary_at_or_after(const DeviceClock::time_point& point) const {

    const auto interval = this->interval();
    if (!interval || !this->last_displayed) {
        return std::nullopt;
    }

    const auto since = point - *this->last_displayed;
    if (since <= DeviceClock::duration::zero()) {
        return this->last_displayed;
    }

    // Round up to the next whole interval past the last one we observed.
    const auto elapsed = since.count();
    const auto step = interval->count();
    const auto cycles = (elapsed + step - 1) / step;

    return *this->last_displayed + DeviceClock::duration{cycles * step};
}

} // namespace low_latency
