#ifndef REENC_PACING_H
#define REENC_PACING_H

// The DVR re-encode's bookkeeping, shared by both platforms' FrameProcessor
// (v4l2_encoder.h, frame_processor.h): every decoded frame goes to the encoder
// once, stamped with when the camera took it, so the recording keeps the
// camera's pace however unevenly the link delivered the frames.

#include <stdint.h>

#include <algorithm>
#include <atomic>

#include "spdlog/spdlog.h"

#include "live_backlog.h"

// The recording's timestamps (CLOCK_MONOTONIC, us). Not thread-safe.
class ReencTimeline {
public:
    // After this long without a new frame, the last one is recorded again
    // every kFillIntervalUs, so the recording runs on through a link loss.
    // Frames that are only late (a link queue) still go in at their capture
    // times: a filled gap leaves them out.
    static constexpr uint64_t kFillAfterUs = 500000;
    static constexpr uint64_t kFillIntervalUs = 100000;

    // A decoded frame's timestamp: when the camera took it, or without that,
    // when it was decoded less the last known difference, so the timeline
    // doesn't jump where capture times come and go. False: older than what
    // the recording already holds -- a frame the link held up, coming in
    // after its gap was filled with the last frame; it is left out rather
    // than squeezed in.
    bool stamp(uint64_t capture_us, uint64_t decoded_us, uint64_t *ts_us) {
        const int64_t lag = (int64_t)(decoded_us - capture_us);
        uint64_t ts;
        if (capture_us && lag > -kMaxCaptureLagUs && lag < kMaxCaptureLagUs) {
            ts = capture_us;
            lag_us_ = lag;
            lag_known_ = true;
        } else {
            ts = decoded_us - (lag_known_ ? lag_us_ : 0);
        }
        latest_decoded_us_ = decoded_us;
        if (last_us_ && ts + kMinStepUs <= last_us_)
            return false;
        *ts_us = next(ts);
        return true;
    }

    // When the last frame is due again for lack of new ones (0: no frame yet).
    uint64_t fill_at() const {
        if (!latest_decoded_us_)
            return 0;
        return std::max(latest_decoded_us_ + kFillAfterUs, last_fill_us_ + kFillIntervalUs);
    }
    // The timestamp of the last frame recorded again at now_us.
    uint64_t fill_stamp(uint64_t now_us) {
        last_fill_us_ = now_us;
        return next(now_us - (lag_known_ ? lag_us_ : 0));
    }

    // At a recording frame rate below the camera's: whether a frame stamped
    // ts_us is due, one at most every interval_us.
    bool due(uint64_t ts_us, uint64_t interval_us) const {
        return !last_encoded_us_ || ts_us + interval_us / 4 >= last_encoded_us_ + interval_us;
    }
    // A decoded frame (not a fill) went to the encoder.
    void encoded(uint64_t ts_us) {
        last_encoded_us_ = ts_us;
        last_fill_us_ = 0;
    }

private:
    // The timestamps grow by at least this much (the muxer needs them to
    // grow; frames come at most a few hundred a second).
    static constexpr uint64_t kMinStepUs = 1000;
    // A capture time further than this from the decode time is not believed.
    static constexpr int64_t kMaxCaptureLagUs = 5000000;

    uint64_t next(uint64_t ts) {
        if (last_us_ && ts < last_us_ + kMinStepUs)
            ts = last_us_ + kMinStepUs;
        last_us_ = ts;
        return ts;
    }

    uint64_t last_us_ = 0;           // the newest timestamp given out
    uint64_t last_encoded_us_ = 0;
    uint64_t latest_decoded_us_ = 0;
    uint64_t last_fill_us_ = 0;
    int64_t lag_us_ = 0;             // decoded - capture, from the last frame with both
    bool lag_known_ = false;
};

// The live picture first: while the live decoder is behind (frame-ring
// entries waiting for it), the recording gets no frames. Not thread-safe.
class ReencLiveGate {
public:
    // Behind once an entry has waited this long for the decoder (a frame at
    // 30 fps; the ring queue drops the backlog at 200 ms), caught up again
    // once the wait is below kCaughtUpUs. Yielding until then, not just until
    // it is under kBehindUs, lets the backlog drain instead of settling at
    // that much extra latency.
    static constexpr uint32_t kBehindUs = 33000;
    static constexpr uint32_t kCaughtUpUs = 8000;

    bool live_behind() {
        const uint32_t backlog = g_live_backlog_us.load(std::memory_order_relaxed);
        if (backlog > kBehindUs)
            behind_ = true;
        else if (backlog < kCaughtUpUs)
            behind_ = false;
        return behind_;
    }

private:
    bool behind_ = false;
};

// What the recording lost or repeated, for the log every 5 s at most. Not
// thread-safe.
struct ReencStats {
    uint64_t yielded = 0;     // skipped while the live picture was behind
    uint64_t overflowed = 0;  // more frames waiting than the queue holds
    uint64_t fills = 0;       // the last frame again, no video
    uint64_t late = 0;        // came after their gap was filled
    uint64_t waited = 0;      // waited for something before going on
    const char *waited_for = "the decoder";

    void log(uint64_t now_us, size_t max_pending) {
        const uint64_t now_ms = now_us / 1000;
        if (now_ms - log_ms_ < 5000)
            return;
        if (yielded)
            spdlog::warn("DVR re-encode: {} frames skipped for the live picture (its decoder was behind)", yielded);
        if (overflowed)
            spdlog::warn("DVR re-encode: {} frames dropped, more than {} were waiting", overflowed, max_pending);
        if (fills)
            spdlog::info("DVR re-encode: no video, the last frame recorded again {} times", fills);
        if (late)
            spdlog::info("DVR re-encode: {} frames left out, they came after their gap was filled", late);
        if (waited)
            spdlog::debug("DVR re-encode: {} times a frame waited for {}", waited, waited_for);
        if (yielded || overflowed || fills || late || waited)
            log_ms_ = now_ms;
        yielded = overflowed = fills = late = waited = 0;
    }

private:
    uint64_t log_ms_ = 0;
};

#endif // REENC_PACING_H
