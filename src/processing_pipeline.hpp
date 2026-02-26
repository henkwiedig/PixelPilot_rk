#pragma once

#include <mutex>
#include <string>
#include <atomic>
#include <queue>
#include <thread>
#include <condition_variable>
#include <memory>
#include <optional>
#include <rockchip/rk_mpi.h>
#include "gstrtpreceiver.h"
#include "processing_gl.hpp"

class Dvr;
#if defined(HAVE_RGA) && HAVE_RGA
class RgaRgbToNv12;
struct RgaRgbToNv12Deleter {
    void operator()(RgaRgbToNv12* ptr) const;
};
#endif

// Command-line/configuration options for the post-decode processing path.
struct ProcessingOptions {
    bool shader_enabled{false};
    std::string shader_name{"none"};
    std::string shader_dir{"shaders"};
    bool shader_for_display{true};
    bool shader_for_encode{true};
    bool encode_processed{false};
    int encode_bitrate_kbps{0}; // 0 means "use default"
    int encode_fps{30}; // target processed encode fps cap
    bool record_colortrans{false};
    std::string record_colortrans_mode{"auto"}; // auto|rga|cpu|off
    std::string record_colortrans_csc{"601l-709f"}; // rga CSC mode
    std::string record_rgb2yuv_mode{"709f"}; // RGB->NV12 for shader output
    std::string record_rgb_format{"xrgb"}; // xrgb|xbgr|rgbx|bgrx|auto
    bool encode_use_mpp_buffer{true}; // prefer MPP-allocated buffers for re-encode
};

enum class RecordColorTransMode {
    Off,
    Auto,
    RgaCsc,
    Cpu
};

// Lightweight placeholder for a future zero-copy shader + encoder tee.
// Currently keeps track of requested options and validates shader files,
// while passing frames through unchanged.
class ProcessingPipeline {
public:
    ProcessingPipeline() = default;
    ~ProcessingPipeline();

    void configure(const ProcessingOptions& opts);
    void set_drm_fd(int fd) { drm_fd_ = fd; }
    void set_video_info(uint32_t width, uint32_t height, uint32_t hor_stride, uint32_t ver_stride, MppFrameFormat fmt);
    void set_dvr(Dvr* dvr, int framerate);
    void update_dvr_fps(int framerate);
    void set_codec(VideoCodec codec) { codec_ = codec; }
    void shutdown();

    // Returns the frame that should be displayed. Today this is the same frame,
    // but the signature allows swapping in a processed buffer later.
    MppFrame process_frame(MppFrame frame, uint32_t& out_fb_id, bool& out_is_processed);

    // Entry point for the processed encode branch; no-op until encoder is wired.
    void submit_for_encoding(MppFrame frame, uint64_t pts_ms);

    const ProcessingOptions& options() const { return opts_; }
    std::string current_shader_path() const;

private:
    void refresh_shader_if_needed();
    bool file_exists(const std::string& path) const;
    bool ensure_gl_ready_for_encode();
    std::optional<ProcessedFrame> process_shader_frame(MppBuffer buffer,
                                                       uint32_t width,
                                                       uint32_t height,
                                                       uint32_t hor_stride,
                                                       uint32_t ver_stride,
                                                       MppFrameFormat fmt,
                                                       uint64_t pts);

    void encoder_loop();
    void maybe_init_record_colortrans();

    ProcessingOptions opts_;
    uint32_t video_width_{0};
    uint32_t video_height_{0};
    uint32_t hor_stride_{0};
    uint32_t ver_stride_{0};
    uint32_t encode_hor_stride_{0};
    uint32_t encode_ver_stride_{0};
    MppFrameFormat format_{MPP_FMT_BUTT};
    int drm_fd_{-1};

    std::string shader_path_cache_;
    std::atomic<bool> shader_checked_{false};
    std::atomic<bool> shader_available_{false};
    std::atomic<bool> encode_warned_{false};
    mutable std::mutex mutex_;
    std::mutex gl_mutex_;

    GLProcessor gl_;
    std::atomic<bool> gl_ready_{false};
    std::atomic<bool> gl_warned_{false};
    bool shader_warned_{false};
    std::atomic<bool> processed_logged_{false};
    uint64_t frame_total_{0};
    std::atomic<uint64_t> gl_ok_{0};
    std::atomic<uint64_t> gl_fail_{0};
    uint64_t last_log_ms_{0};
    std::atomic<uint64_t> last_render_ms_{0};
    std::atomic<uint64_t> enc_drop_{0};
    std::atomic<uint64_t> enc_throttle_drop_{0};
    std::atomic<uint64_t> enc_rgb_fail_{0};
    std::atomic<uint64_t> enc_rgb_ok_{0};

    RecordColorTransMode record_colortrans_mode_{RecordColorTransMode::Off};
    int record_colortrans_csc_mode_{0};
    int record_rgb2yuv_mode_{0};
    int record_rgb_format_{0};
    bool record_colortrans_ready_{false};
    bool record_colortrans_warned_{false};
#if defined(HAVE_RGA) && HAVE_RGA
    std::unique_ptr<RgaRgbToNv12, RgaRgbToNv12Deleter> rga_rgb_to_nv12_;
#endif
    struct PendingRgb {
        int fd{-1};
        uint32_t width{0};
        uint32_t height{0};
        uint32_t stride{0};
        uint32_t format{0};
        bool valid{false};
    };
    PendingRgb pending_rgb_;

    Dvr* dvr_{nullptr};
    int dvr_fps_{-1};
    VideoCodec codec_{VideoCodec::H265};

    struct EncJob {
        MppBuffer buffer{nullptr};
        bool recording{false};
        uint32_t width{0};
        uint32_t height{0};
        uint32_t hor_stride{0};
        uint32_t ver_stride{0};
        MppFrameFormat fmt{MPP_FMT_BUTT};
        uint64_t pts{0};
        int pool_index{-1};
        int rgb_fd{-1};
        uint32_t rgb_width{0};
        uint32_t rgb_height{0};
        uint32_t rgb_stride{0};
        uint32_t rgb_format{0};
    };
    std::queue<EncJob> enc_queue_;
    std::mutex enc_mutex_;
    std::condition_variable enc_cv_;
    std::thread enc_thread_;
    std::atomic<bool> enc_run_{false};
    std::atomic<bool> enc_warned_{false};
};
