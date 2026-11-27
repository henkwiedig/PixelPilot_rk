#ifndef MPP_ENCODER_H
#define MPP_ENCODER_H

#include <stdint.h>
#include <queue>
#include <mutex>
#include <condition_variable>
#include <memory>
#include <vector>
#include <chrono>
#include <functional>

#include <rockchip/rk_mpi.h>

#include "gstrtpreceiver.h"
#include "encoder_params.h"

typedef EncoderParams MppEncoderParams;

struct EncRpc {
    enum {
        RPC_FRAME,
        RPC_IDR,
        RPC_SET_BITRATE,  // live bitrate change (no reinit)
        RPC_SET_CODEC,    // codec change (forces reinit on next frame)
        RPC_SET_FPS,      // fps change (forces reinit on next frame)
        RPC_SHUTDOWN
    } command;

    // For RPC_FRAME: decoded DRM buffer (caller has called mpp_buffer_inc_ref)
    MppBuffer buffer = nullptr;
    uint32_t width = 0;
    uint32_t height = 0;
    uint32_t hor_stride = 0;
    uint32_t ver_stride = 0;
    MppFrameFormat fmt = MPP_FMT_YUV420SP;
    uint64_t pts = 0;

    // For RPC_SET_BITRATE / RPC_SET_FPS
    int new_bitrate = 0;
    int new_fps     = 0;
    // For RPC_SET_CODEC
    VideoCodec new_codec = VideoCodec::UNKNOWN;
};

class MppEncoder {
public:
    // An encoded picture (or the parameter sets ahead of one), with the pts
    // its frame was pushed with (CLOCK_MONOTONIC, us).
    using FrameCallback = std::function<void(std::shared_ptr<std::vector<uint8_t>>, uint64_t ts_us)>;

    explicit MppEncoder(MppEncoderParams params, FrameCallback cb);
    ~MppEncoder();

    // Encodes the frame in buffer (the caller has taken the reference the
    // encoder puts back once done), pts in us. Returns its sequence number,
    // for wait_done(): frames are encoded one at a time, in order.
    uint64_t push_frame(MppBuffer buffer,
                        uint32_t width, uint32_t height,
                        uint32_t hor_stride, uint32_t ver_stride,
                        MppFrameFormat fmt, uint64_t pts);
    // Waits up to timeout_ms until frame seq is encoded (or dropped) and its
    // buffer is no longer read. False if it isn't by then.
    bool wait_done(uint64_t seq, int timeout_ms);
    void request_idr();
    void shutdown();

    // Live parameter changes — enqueued to the encoder thread (thread-safe).
    void set_bitrate(int kbps);   // update RC bitrate without reinit
    void set_codec(VideoCodec c); // change codec — forces reinit on next frame
    void set_fps(int fps);        // change fps — forces reinit on next frame

    VideoCodec get_codec() const { return params.codec; }

    static void *__THREAD__(void *context);

private:
    void loop();
    bool init_encoder(uint32_t width, uint32_t height,
                      uint32_t hor_stride, uint32_t ver_stride,
                      MppFrameFormat fmt);
    void cleanup_encoder();
    void encode_frame(EncRpc &rpc);
    void enqueue(EncRpc rpc);

private:
    MppEncoderParams params;
    FrameCallback output_cb;

    MppCtx ctx = nullptr;
    MppApi *mpi = nullptr;

    bool initialized = false;
    bool idr_pending = true;
    bool headers_sent = false;
    std::vector<uint8_t> extra_data; // VPS/SPS/PPS from MPP_ENC_GET_EXTRA_INFO
    uint32_t enc_width = 0;
    uint32_t enc_height = 0;
    uint32_t enc_hor_stride = 0;
    uint32_t enc_ver_stride = 0;

    std::queue<EncRpc> queue;
    std::mutex mtx;
    std::condition_variable cv;
    uint64_t submitted_ = 0;        // frames pushed, with mtx held
    uint64_t done_ = 0;             // frames through encode_frame(), with done_mtx_ held
    std::mutex done_mtx_;
    std::condition_variable done_cv_;
};

#endif // MPP_ENCODER_H
