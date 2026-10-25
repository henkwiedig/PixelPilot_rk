#ifndef V4L2_ENCODER_H
#define V4L2_ENCODER_H

// The DVR re-encoder on a V4L2 platform: a stateful V4L2 memory-to-memory
// encoder (wave5 on the Artosyn AR9311) that reads the decoder's frames
// straight from their dma-bufs, so a re-encoded frame is never copied.
//
// There is no scaler and no GPU, so the recording has the decoded stream's
// resolution, without the OSD and without colour correction; the settings
// for those are accepted and ignored.

#include <stdint.h>

#include <condition_variable>
#include <functional>
#include <memory>
#include <mutex>
#include <queue>
#include <vector>

#include "encoder_params.h"
#include "v4l2_decoder.h"

class V4l2Encoder {
public:
    using FrameCallback = std::function<void(std::shared_ptr<std::vector<uint8_t>>)>;

    V4l2Encoder(EncoderParams params, FrameCallback cb);
    ~V4l2Encoder();

    // Encodes a decoder frame. The encoder takes a reference on it from the
    // decoder and gives it back once the frame is encoded.
    void push_frame(V4l2Decoder *dec, const V4l2Decoder::Frame &frame);
    void request_idr();
    void shutdown();
    // Gives back every frame the encoder holds and closes the device; the
    // next frame opens it again. Waits until that is done.
    void drop_frames();

    void set_bitrate(int kbps);   // live
    void set_codec(VideoCodec c); // reopens the device on the next frame
    void set_fps(int fps);        // live

    VideoCodec get_codec() const { return params.codec; }

    static void *__THREAD__(void *context);

private:
    struct Rpc {
        enum { FRAME, IDR, BITRATE, CODEC, FPS, DROP, SHUTDOWN } command;
        V4l2Decoder *dec = nullptr;
        V4l2Decoder::Frame frame;
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
    bool open_device(const V4l2Decoder::Frame &f);
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
    uint32_t width_ = 0, height_ = 0, stride_ = 0, plane_height_ = 0;
    std::vector<Source> sources_;
    std::vector<std::pair<void *, size_t>> coded_;

    std::queue<Rpc> queue;
    std::mutex mtx;
    std::condition_variable drop_cv_;
    uint64_t drops_requested_ = 0, drops_done_ = 0;
};

// Paces the encoder: at the recording's frame rate it hands the newest
// decoded frame over, or the previous one again if none came in since, so
// the recording keeps its rate whatever the link delivers.
class FrameProcessor {
public:
    FrameProcessor(V4l2Encoder *enc, int fps, EncResolution res = EncResolution::Res1080p,
                   int drm_fd = -1, std::function<void()> on_fatal_error = nullptr);
    ~FrameProcessor();

    void clear_fatal_error() {}

    // From the frame thread: the newest frame (the processor takes its own
    // reference on it).
    void push_latest(V4l2Decoder *dec, const V4l2Decoder::Frame &frame);

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
    void loop();
    void drop_latest();   // with mtx held

    V4l2Encoder *encoder;
    std::mutex mtx;
    std::condition_variable cv;
    bool running = true;
    long interval_ns;
    V4l2Decoder *dec_ = nullptr;
    V4l2Decoder::Frame latest_;   // index -1 = none
};

#endif // V4L2_ENCODER_H
