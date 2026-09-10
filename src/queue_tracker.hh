#ifndef QUEUE_TRACKER_HH_
#define QUEUE_TRACKER_HH_

#include "submission_span.hh"
#include "timestamp_pool.hh"

#include <memory>
#include <mutex>

namespace low_latency {

class QueueContext;

// Accumulates one queue's contribution to the frame currently being built.
//
// This replaces the per-extension queue strategies. Neither of them was doing
// anything vendor specific: the Anti-Lag one collected every graphics
// submission and the Reflex one keyed submissions off application-supplied
// present ids. With the frame boundary now defined by vkQueuePresentKHR there
// is only one sensible rule left - everything submitted since the last present
// belongs to this frame - so there is only one implementation.
class QueueTracker final {
  private:
    QueueContext& queue;

    std::mutex mutex{};
    std::unique_ptr<SubmissionSpan> span{};

  public:
    explicit QueueTracker(QueueContext& queue);
    QueueTracker(const QueueTracker&) = delete;
    QueueTracker(QueueTracker&&) = delete;
    QueueTracker& operator=(const QueueTracker&) = delete;
    QueueTracker& operator=(QueueTracker&&) = delete;
    ~QueueTracker();

  public:
    void notify_submit(std::shared_ptr<TimestampPool::Handle> handle);

    // Hands the accumulated span over and starts a new frame.
    std::unique_ptr<SubmissionSpan> take();

  public:
    bool should_track() const;
};

} // namespace low_latency

#endif
