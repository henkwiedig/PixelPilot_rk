#ifndef V4L2_ENCODER_H
#define V4L2_ENCODER_H

// The DVR re-encoder on a V4L2 platform: a stateful V4L2 memory-to-memory
// encoder (wave5 on the Artosyn AR9311) that reads the decoder's frames
// straight from their dma-bufs, so a re-encoded frame is never copied --
// unless the OSD is recorded or the recording is 720p: then it reads frames
// made from them on the SoC's 2D engines (v4l2_frame_prep.h).
//
// There is no GPU, so the recording is not colour corrected; that setting is
// accepted and ignored.

#include <stdint.h>

#include <condition_variable>
#include <deque>
#include <functional>
#include <memory>
#include <mutex>
#include <queue>
#include <vector>

#include "encoder_params.h"
#include "reenc_pacing.h"
#include "v4l2_frame_prep.h"
#include "v4l2_decoder.h"

class V4l2Encoder {
public:
    // An encoded picture, with the timestamp its frame was pushed with
    // (CLOCK_MONOTONIC, us; wave5 copies it from the frame to the picture).
    using FrameCallback = std::function<void(std::shared_ptr<std::vector<uint8_t>>, uint64_t ts_us)>;

    V4l2Encoder(EncoderParams params, FrameCallback cb);
    ~V4l2Encoder();

    // Encodes a decoder frame, stamped ts_us (CLOCK_MONOTONIC, us). The
    // encoder takes a reference on it from the decoder and gives it back
    // once the frame is encoded.
    void push_frame(V4l2Decoder *dec, const V4l2Decoder::Frame &frame, uint64_t ts_us);
    void request_idr();
    void shutdown();
    // Gives back every frame the encoder holds and closes the device; the
    // next frame opens it again. Waits until that is done.
    void drop_frames();

    void set_bitrate(int kbps);   // live
    void set_codec(VideoCodec c); // reopens the device on the next frame
    void set_fps(int fps);        // live
    void set_resolution(EncResolution r);   // reopens the device on the next frame

    VideoCodec get_codec() const { return params.codec; }

    static void *__THREAD__(void *context);

private:
    struct Rpc {
        enum { FRAME, IDR, BITRATE, CODEC, FPS, RESOLUTION, DROP, SHUTDOWN } command;
        V4l2Decoder *dec = nullptr;
        V4l2Decoder::Frame frame;
        uint64_t ts_us = 0;
        int value = 0;
        VideoCodec codec = VideoCodec::UNKNOWN;
    };
    struct Source {
        bool busy = false;
        V4l2Decoder *dec = nullptr;
        int index = -1;
    };

    void enqueue(Rpc rpc);
    void loop();
    bool open_device(const EncFrame &f);
    void close_device();
    void encode(const Rpc &rpc);
    void collect_output();
    void return_sources();
    bool set_ctrl(uint32_t id, int32_t value, const char *name);
    void apply_fps();

    EncoderParams params;
    FrameCallback output_cb;

    int fd_ = -1;
    int wake_fd_ = -1;
    bool reopen_ = false;
    uint64_t last_frame_ms_ = 0;
    uint32_t width_ = 0, height_ = 0, stride_ = 0, plane_height_ = 0;
    std::vector<Source> sources_;   // dec == nullptr: one of prep_'s frames
    std::vector<std::pair<void *, size_t>> coded_;
    FramePrep prep_;

    std::queue<Rpc> queue;
    std::mutex mtx;
    std::condition_variable drop_cv_;
    uint64_t drops_requested_ = 0, drops_done_ = 0;
};

// Feeds the encoder every decoded frame once, stamped with its camera capture
// time, so the recording holds the frames the link delivered at the camera's
// own pace, however unevenly they arrived. (It used to sample the newest frame
// on a timer of its own, which skipped one frame and repeated the next
// whenever two came in within a tick.)
//
// The live picture comes first: the decoder shares the codec with the encoder,
// so a frame waits while the decoder is at work on a newer one, and none is
// encoded while the live picture is behind.
//
// When no video comes for a while (link loss), the last frame is encoded again
// at a low rate, so the recording runs on and a recorded OSD keeps moving.
// Frames the link held up that come in after their gap was filled so are left
// out.
class FrameProcessor {
public:
    FrameProcessor(V4l2Encoder *enc, int fps, EncResolution res = EncResolution::Res1080p,
                   int drm_fd = -1, std::function<void()> on_fatal_error = nullptr);
    ~FrameProcessor();

    void clear_fatal_error() {}

    // From the frame thread, for every decoded frame (the processor takes its
    // own references). capture_us: its camera capture time on the ground
    // clock, 0 if unknown; decoded_us: when the decoder handed it out. Both
    // CLOCK_MONOTONIC, us.
    void push_latest(V4l2Decoder *dec, const V4l2Decoder::Frame &frame, uint64_t capture_us,
                     uint64_t decoded_us);

    void shutdown();
    void set_fps(int fps);
    void set_resolution(EncResolution r);
    void set_color_correction(float gain, float offset);
    void set_color_correction_enabled(bool on) { (void)on; }
    // Before the decoder frees its frames: lets go of every one held here
    // and in the encoder.
    void drain_decoder_refs();
    void set_osd_blend(int prime_fd, uint32_t w, uint32_t h, uint32_t stride_px);

    static void *__THREAD__(void *p);

private:
    struct Pending {
        V4l2Decoder::Frame frame;
        uint64_t ts_us;        // the recording's timestamp
        uint64_t decoded_us;
    };

    void loop();
    void drop_all();                                        // with mtx held
    bool hand_over(const V4l2Decoder::Frame &f, uint64_t ts_us);  // with mtx held; false: yielded

    V4l2Encoder *encoder;
    std::mutex mtx;
    std::condition_variable cv;
    bool running = true;
    long interval_ns;
    V4l2Decoder *dec_ = nullptr;
    std::deque<Pending> pending_;    // decoded, not handed over yet (references held)
    V4l2Decoder::Frame latest_;      // the newest frame, for the no-video fill (index -1 = none)
    // All three with mtx held.
    ReencTimeline timeline_;
    ReencLiveGate live_;
    ReencStats stats_;
};

#endif // V4L2_ENCODER_H
