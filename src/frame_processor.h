#ifndef FRAME_PROCESSOR_H
#define FRAME_PROCESSOR_H

#include <stdint.h>
#include <atomic>
#include <condition_variable>
#include <deque>
#include <functional>
#include <mutex>

#include <rockchip/rk_mpi.h>

#include "mpp_encoder.h"
#include "frame_colorcorrect.h"
#include "reenc_pacing.h"

// ---------------------------------------------------------------------------
// FrameProcessor: feeds MppEncoder every decoded frame once, stamped with its
// camera capture time, so the recording holds the frames the link delivered
// at the camera's own pace, however unevenly they arrived. (A timer of its own
// used to take the newest processed frame at the recording's frame rate,
// which skipped one frame and repeated the next whenever two came in within
// a tick.)
//
// The processor thread (which has the GL context) copies / resizes /
// colour-corrects / blends the OSD into one of two buffers of its own and
// hands it to the encoder, then writes the next frame into the other one,
// once the encoder is done with it. No frame goes to the recording while the
// live picture is behind (ReencLiveGate). When no video comes for a while
// (link loss), the last frame is encoded again at a low rate, so the
// recording runs on and a recorded OSD keeps moving (ReencTimeline).
// ---------------------------------------------------------------------------

struct FrameProcFrame {
    MppBuffer      buffer     = nullptr;
    uint32_t       width      = 0;
    uint32_t       height     = 0;
    uint32_t       hor_stride = 0;
    uint32_t       ver_stride = 0;
    MppFrameFormat fmt        = MPP_FMT_YUV420SP;
    void release() {
        if (buffer) { mpp_buffer_put(buffer); buffer = nullptr; }
    }
};

class FrameProcessor {
public:
    // on_fatal_error is invoked (from the processor thread) the first time a
    // frame can't be reliably converted -- see process_loop() for why this
    // stops the reencode session instead of degrading through a fallback.
    FrameProcessor(MppEncoder *enc, int fps, EncResolution res = EncResolution::Res1080p,
                   int drm_fd = -1, std::function<void()> on_fatal_error = nullptr);
    ~FrameProcessor();

    // Re-arm after a fatal conversion failure (call when reencode recording
    // is (re)started, e.g. from Dvr::on_start_cb).
    void clear_fatal_error() { reenc_fatal_.store(false, std::memory_order_relaxed); }

    // Called from the frame thread for every decoded frame (the processor
    // takes its own reference). capture_us: its camera capture time on the
    // ground clock, 0 if unknown; decoded_us: when the decoder handed it out.
    // Both CLOCK_MONOTONIC, us.
    void push_latest(MppBuffer buf, uint32_t w, uint32_t h,
                     uint32_t hs, uint32_t vs, MppFrameFormat fmt,
                     uint64_t capture_us, uint64_t decoded_us);

    void shutdown();

    // Live-update the recording's frame rate (thread-safe): below the
    // camera's, one frame at most every 1/fps s.
    void set_fps(int fps) { interval_ns.store(1000000000L / fps, std::memory_order_relaxed); }

    // Set encoder output resolution (thread-safe).
    void set_resolution(EncResolution r) { target_res_.store((int)r, std::memory_order_relaxed); }

    // Enable GPU color correction using the DRM gamma formula y = clamp((x+offset)*gain, 0, 1).
    // Safe to call from any thread.  The GBM/EGL context is created lazily on
    // the processor thread from the DRM fd given at construction.
    void set_color_correction(float gain, float offset);

    // Enable/disable color correction at runtime without changing the stored params.
    // Thread-safe: safe to toggle from the UI thread while the pacer is running.
    void set_color_correction_enabled(bool on) {
        color_correct_.store(on, std::memory_order_relaxed);
    }

    // Called from the frame thread before freeing the decoder buffer group
    // (on resolution change).  Releases any pending decoder buffer ref and
    // waits for any in-flight copy to finish so the group can be freed safely.
    void drain_decoder_refs();

    // Called from the OSD thread each time a new OSD frame is ready.
    // prime_fd  — DMA-buf fd of the OSD modeset_buf (BGRA/ARGB8888)
    // w, h      — OSD pixel dimensions
    // stride_px — row stride in pixels (= buf->stride / 4)
    void set_osd_blend(int prime_fd, uint32_t w, uint32_t h, uint32_t stride_px);

    static void *__THREAD__(void *p);

private:
    struct Pending {
        FrameProcFrame frame;
        uint64_t ts_us = 0;   // the recording's timestamp
    };

    void process_loop();
    // Hands buf (with the reference taken for the encoder) to the encoder;
    // returns its sequence number.
    uint64_t submit(MppBuffer buf, const FrameProcFrame &meta, uint64_t ts_us);

    // Attempts CMA-backed (<4GB-guaranteed) allocation of proc_copy_ via a
    // raw DRM dumb buffer, bypassing MPP's buffer-group CONTIG/DMA32 flags
    // (see mem_info.h -- they don't reliably route to CMA on this
    // platform). Only called on large-RAM boards. On failure, proc_copy_
    // is left null; the caller does not fall back to hold_grp for this --
    // see process_loop().
    bool alloc_contig_proc_copy(size_t size);

    MppEncoder            *encoder;
    std::atomic<long>     interval_ns;
    std::atomic<int>      target_res_{1};  // 0=720p, 1=1080p
    std::atomic<bool>     running{true};
    std::atomic<bool>     reenc_fatal_{false};  // set once a frame conversion fails
    std::function<void()> on_fatal_error_;      // invoked once when reenc_fatal_ is set
    std::mutex              mtx;       // guards pending_, timeline_, stats_ (shared with the frame thread)
    std::condition_variable cv_;       // signalled by push_latest(); processor waits here
    std::mutex              copy_mtx_; // held by processor while it uses a decoder buffer
    std::deque<Pending>     pending_;  // decoded, not processed yet (references held)
    ReencTimeline           timeline_;
    ReencStats              stats_;

    // Only accessed from the processor thread — no mutex needed:
    MppBufferGroup    hold_grp  = nullptr;  // our own DRM buffer pool
    MppBuffer         proc_copy_  = nullptr;  // processor's working buffer: the next frame goes here
    FrameProcFrame     proc_meta_;              // metadata being built by processor
    uint64_t          proc_seq_   = 0;        // encoder sequence number of proc_copy_'s last use
    MppBuffer         last_copy   = nullptr;  // the frame handed to the encoder last
    FrameProcFrame     last_meta;              // geometry/format for last_copy
    uint64_t          last_seq_   = 0;        // its sequence number
    bool              last_fresh_ = false;    // last_copy is of the current recording
    ReencLiveGate     live_;

    // OSD blend — shared between OSD thread (writer) and processor thread (reader)
    struct OsdInfo {
        int      prime_fd{-1};
        uint32_t width{0}, height{0}, stride_px{0};
    };
    std::mutex  osd_mtx_;
    OsdInfo     osd_info_;      // latest OSD frame descriptor

    // GL pipeline — handles colour-correction and/or OSD blend in one GPU pass.
    // Lazy-initialised on the processor thread on the first frame that needs it.
    // Written by UI thread (set_color_correction / set_color_correction_enabled),
    // read by processor thread — must be atomic where shared.
    std::atomic<bool>  color_correct_{false};
    bool               gl_init_done_{false};       // processor thread only
    uint32_t           gl_out_w_{0}, gl_out_h_{0}; // processor thread only: dims at last GL init
    std::atomic<float> cc_gain_{1.f};
    std::atomic<float> cc_offset_{0.f};
    // DRM fd for GBM/EGL.  Immutable after construction: the processor thread
    // reads it on every frame, so a runtime write from the UI thread would be
    // a data race.  const enforces that rather than leaving it to convention.
    const int          drm_fd_;
    FrameColorCorrect  color_gl_;
};

#endif // FRAME_PROCESSOR_H
