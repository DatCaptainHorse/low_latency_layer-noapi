#include "queue_tracker.hh"

#include "device_context.hh"
#include "queue_context.hh"

namespace low_latency {

QueueTracker::QueueTracker(QueueContext& queue) : queue(queue) {}

QueueTracker::~QueueTracker() {}

bool QueueTracker::should_track() const {
    // IMPORTANT: exclude non-graphics queues. Async compute and transfer work
    // is not on the frame's critical path, and pulling it into our timings
    // measurably hurts - this was true when the layer was told which queues
    // were out of band by vkQueueNotifyOutOfBandNV, and it is more important
    // now that nothing tells us.
    return this->queue.properties.queueFlags & VK_QUEUE_GRAPHICS_BIT;
}

void QueueTracker::notify_submit(
    std::shared_ptr<TimestampPool::Handle> handle) {

    if (!this->should_track()) {
        return;
    }

    const auto lock = std::scoped_lock{this->mutex};
    if (this->span) {
        this->span->update(std::move(handle));
    } else {
        this->span = std::make_unique<SubmissionSpan>(std::move(handle));
    }
}

std::unique_ptr<SubmissionSpan> QueueTracker::take() {
    const auto lock = std::scoped_lock{this->mutex};
    return std::move(this->span);
}

} // namespace low_latency
