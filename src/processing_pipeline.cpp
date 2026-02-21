#include "processing_pipeline.hpp"

#include <filesystem>
#include <cstring>
#include <unistd.h>
#include <sys/ioctl.h>
#include <sys/mman.h>
#include <fcntl.h>
#include <cmath>
#include <spdlog/spdlog.h>
#include <rockchip/rk_venc_cfg.h>
#include <chrono>
#include <rockchip/rk_venc_rc.h>
#include "dvr.h"

namespace {
uint64_t now_ms() {
    using namespace std::chrono;
    return duration_cast<milliseconds>(steady_clock::now().time_since_epoch()).count();
}

uint32_t align_up(uint32_t value, uint32_t align) {
    if (!align) return value;
    return (value + align - 1) & ~(align - 1);
}
} // namespace

#if defined(HAVE_RGA) && HAVE_RGA
#include <xf86drm.h>
#include <drm/drm_mode.h>
#include <rga/rga.h>
#include <rga/im2d.h>
#include <rga/RgaApi.h>
#include <gbm.h>
#endif

namespace {
bool has_start_code(const std::vector<uint8_t>& data) {
    if (data.size() < 3) return false;
    if (data[0] == 0x00 && data[1] == 0x00 && data[2] == 0x01) return true;
    if (data.size() >= 4 && data[0] == 0x00 && data[1] == 0x00 && data[2] == 0x00 && data[3] == 0x01) return true;
    return false;
}

std::vector<uint8_t> avcc_to_annexb(const std::vector<uint8_t>& data) {
    std::vector<uint8_t> out;
    size_t offset = 0;
    while (offset + 4 <= data.size()) {
        uint32_t nal_len = (data[offset] << 24) | (data[offset + 1] << 16) | (data[offset + 2] << 8) | data[offset + 3];
        offset += 4;
        if (nal_len == 0 || offset + nal_len > data.size()) {
            break;
        }
        out.push_back(0x00);
        out.push_back(0x00);
        out.push_back(0x00);
        out.push_back(0x01);
        out.insert(out.end(), data.begin() + offset, data.begin() + offset + nal_len);
        offset += nal_len;
    }
    return out.empty() ? data : out;
}

std::vector<uint8_t> ensure_annexb(std::vector<uint8_t> data);

std::vector<uint8_t> avcc_config_to_annexb(const std::vector<uint8_t>& data) {
    if (data.size() < 7 || data[0] != 1) {
        return data;
    }
    size_t offset = 5;
    if (offset >= data.size()) return data;
    int num_sps = data[offset] & 0x1f;
    offset += 1;
    std::vector<uint8_t> out;
    for (int i = 0; i < num_sps; i++) {
        if (offset + 2 > data.size()) return data;
        uint16_t len = (data[offset] << 8) | data[offset + 1];
        offset += 2;
        if (offset + len > data.size()) return data;
        out.insert(out.end(), {0x00, 0x00, 0x00, 0x01});
        out.insert(out.end(), data.begin() + offset, data.begin() + offset + len);
        offset += len;
    }
    if (offset >= data.size()) return out.empty() ? data : out;
    int num_pps = data[offset];
    offset += 1;
    for (int i = 0; i < num_pps; i++) {
        if (offset + 2 > data.size()) return out.empty() ? data : out;
        uint16_t len = (data[offset] << 8) | data[offset + 1];
        offset += 2;
        if (offset + len > data.size()) return out.empty() ? data : out;
        out.insert(out.end(), {0x00, 0x00, 0x00, 0x01});
        out.insert(out.end(), data.begin() + offset, data.begin() + offset + len);
        offset += len;
    }
    return out.empty() ? data : out;
}

std::vector<uint8_t> hvcc_config_to_annexb(const std::vector<uint8_t>& data) {
    if (data.size() < 23 || data[0] != 1) {
        return data;
    }
    size_t offset = 22;
    if (offset >= data.size()) return data;
    uint8_t num_arrays = data[offset++];
    std::vector<uint8_t> out;
    for (uint8_t i = 0; i < num_arrays; i++) {
        if (offset + 3 > data.size()) return out.empty() ? data : out;
        uint8_t nal_unit_type = data[offset] & 0x3f;
        offset += 1;
        uint16_t num_nalus = (data[offset] << 8) | data[offset + 1];
        offset += 2;
        for (uint16_t j = 0; j < num_nalus; j++) {
            if (offset + 2 > data.size()) return out.empty() ? data : out;
            uint16_t len = (data[offset] << 8) | data[offset + 1];
            offset += 2;
            if (offset + len > data.size()) return out.empty() ? data : out;
            if (nal_unit_type == 32 || nal_unit_type == 33 || nal_unit_type == 34) {
                out.insert(out.end(), {0x00, 0x00, 0x00, 0x01});
                out.insert(out.end(), data.begin() + offset, data.begin() + offset + len);
            }
            offset += len;
        }
    }
    return out.empty() ? data : out;
}

std::vector<uint8_t> ensure_annexb_config(std::vector<uint8_t> data, VideoCodec codec) {
    if (data.empty()) return data;
    if (has_start_code(data)) return data;
    if (codec == VideoCodec::H264) {
        auto converted = avcc_config_to_annexb(data);
        if (has_start_code(converted)) return converted;
    } else if (codec == VideoCodec::H265) {
        auto converted = hvcc_config_to_annexb(data);
        if (has_start_code(converted)) return converted;
    }
    return ensure_annexb(std::move(data));
}

std::vector<uint8_t> ensure_annexb(std::vector<uint8_t> data) {
    if (data.empty()) return data;
    if (has_start_code(data)) return data;
    auto converted = avcc_to_annexb(data);
    if (has_start_code(converted)) return converted;
    std::vector<uint8_t> out;
    out.reserve(data.size() + 4);
    out.push_back(0x00);
    out.push_back(0x00);
    out.push_back(0x00);
    out.push_back(0x01);
    out.insert(out.end(), data.begin(), data.end());
    return out;
}

std::vector<std::vector<uint8_t>> split_annexb(const std::vector<uint8_t>& data) {
    std::vector<std::vector<uint8_t>> out;
    size_t i = 0;
    auto find_start = [&](size_t pos) -> size_t {
        for (size_t j = pos; j + 3 <= data.size(); j++) {
            if (data[j] == 0x00 && data[j + 1] == 0x00 && data[j + 2] == 0x01) return j;
            if (j + 4 <= data.size() && data[j] == 0x00 && data[j + 1] == 0x00 && data[j + 2] == 0x00 && data[j + 3] == 0x01) return j;
        }
        return data.size();
    };
    size_t start = find_start(0);
    while (start < data.size()) {
        size_t next = find_start(start + 3);
        if (next == start) break;
        size_t end = (next < data.size()) ? next : data.size();
        if (end > start) {
            out.emplace_back(data.begin() + start, data.begin() + end);
        }
        start = next;
    }
    return out;
}

RecordColorTransMode parse_record_colortrans_mode(const std::string& mode) {
    if (mode == "off") return RecordColorTransMode::Off;
    if (mode == "auto") return RecordColorTransMode::Auto;
    if (mode == "rga") return RecordColorTransMode::RgaCsc;
    if (mode == "cpu") return RecordColorTransMode::Cpu;
    return RecordColorTransMode::Auto;
}

#if defined(HAVE_RGA) && HAVE_RGA
int parse_record_rgb2yuv_mode(const std::string& mode) {
    if (mode == "709f") return rgb2yuv_709_full;
    if (mode == "709l") return rgb2yuv_709_full;
    if (mode == "601f") return rgb2yuv_601_full;
    if (mode == "601l") return rgb2yuv_601_full;
    return rgb2yuv_709_full;
}

int parse_record_rgb_format(const std::string& mode) {
    if (mode == "xrgb") return RK_FORMAT_XRGB_8888;
    if (mode == "xbgr") return RK_FORMAT_XBGR_8888;
    if (mode == "rgbx") return RK_FORMAT_RGBX_8888;
    if (mode == "bgrx") return RK_FORMAT_BGRX_8888;
    if (mode == "auto") return 0;
    return RK_FORMAT_XRGB_8888;
}

int parse_record_colortrans_csc(const std::string& mode) {
    if (mode == "none" || mode == "off") return 0;
    if (mode == "601l-709l") return yuv2yuv_601_limit_2_709_limit;
    if (mode == "601l-709f") return yuv2yuv_601_limit_2_709_full;
    if (mode == "709l-601l") return yuv2yuv_709_limit_2_601_limit;
    if (mode == "601f-709l") return yuv2yuv_601_full_2_709_limit;
    return 0;
}
#endif

class MppEncoder {
public:
    bool init(uint32_t width, uint32_t height, uint32_t hor_stride, uint32_t ver_stride, MppFrameFormat fmt, VideoCodec codec, int fps, int bitrate_kbps) {
        if (ready_) return true;
        if (fps <= 0) {
            spdlog::warn("Encoder init requires valid FPS; got {}", fps);
            return false;
        }
        codec_ = codec;
        MppCodingType type = (codec == VideoCodec::H264) ? MPP_VIDEO_CodingAVC : MPP_VIDEO_CodingHEVC;
        if (mpp_create(&ctx_, &mpi_) != MPP_OK) {
            spdlog::warn("mpp_create for encoder failed");
            return false;
        }
        if (mpp_init(ctx_, MPP_CTX_ENC, type) != MPP_OK) {
            spdlog::warn("mpp_init for encoder failed");
            return false;
        }
        RK_S64 timeout = 0;
        mpi_->control(ctx_, MPP_SET_OUTPUT_TIMEOUT, &timeout);
        if (mpp_enc_cfg_init(&cfg_) != MPP_OK) {
            spdlog::warn("mpp_enc_cfg_init failed");
            return false;
        }
        if (mpi_->control(ctx_, MPP_ENC_GET_CFG, cfg_) != MPP_OK) {
            spdlog::warn("MPP_ENC_GET_CFG failed");
            return false;
        }

        RK_S32 bps = bitrate_kbps > 0 ? bitrate_kbps * 1000 : 4000000;
        RK_S32 bps_max = bps * 12 / 10;
        RK_S32 bps_min = bps * 8 / 10;

        auto cfg_set = [&](const char* key, RK_S32 val) {
            MPP_RET ret = mpp_enc_cfg_set_s32(cfg_, key, val);
            if (ret) {
                spdlog::warn("mpp_enc_cfg_set_s32 failed for {} ({}): {}", key, val, static_cast<int>(ret));
                return false;
            }
            return true;
        };
        if (!cfg_set("prep:width", width) ||
            !cfg_set("prep:height", height) ||
            !cfg_set("prep:hor_stride", hor_stride) ||
            !cfg_set("prep:ver_stride", ver_stride) ||
            !cfg_set("prep:format", fmt) ||
            !cfg_set("rc:mode", MPP_ENC_RC_MODE_CBR) ||
            !cfg_set("rc:bps_target", bps) ||
            !cfg_set("rc:bps_max", bps_max) ||
            !cfg_set("rc:bps_min", bps_min) ||
            !cfg_set("rc:fps_in_num", fps) ||
            !cfg_set("rc:fps_in_denorm", 1) ||
            !cfg_set("rc:fps_out_num", fps) ||
            !cfg_set("rc:fps_out_denorm", 1) ||
            !cfg_set("rc:gop", fps * 2)) {
            return false;
        }

        if (mpi_->control(ctx_, MPP_ENC_SET_CFG, cfg_) != MPP_OK) {
            spdlog::warn("MPP_ENC_SET_CFG failed (check config keys)");
            return false;
        }

        MppPacket pkt = nullptr;
        if (mpi_->control(ctx_, MPP_ENC_GET_EXTRA_INFO, &pkt) == MPP_OK && pkt) {
            void* ptr = mpp_packet_get_pos(pkt);
            size_t len = mpp_packet_get_length(pkt);
            if (ptr && len > 0) {
                extra_.assign(static_cast<uint8_t*>(ptr), static_cast<uint8_t*>(ptr) + len);
                extra_ = ensure_annexb_config(std::move(extra_), codec_);
            }
            mpp_packet_deinit(&pkt);
        }

        width_ = width;
        height_ = height;
        fmt_ = fmt;
        ready_ = true;
        sent_headers_ = false;
        spdlog::info("MPP encoder initialized {}x{} bps={} fps={}", width, height, bps, fps);
        return true;
    }

    void deinit() {
        if (!ready_) return;
        if (cfg_) {
            mpp_enc_cfg_deinit(cfg_);
            cfg_ = nullptr;
        }
        if (mpi_ && ctx_) {
            mpi_->reset(ctx_);
            mpp_destroy(ctx_);
        }
        mpi_ = nullptr;
        ctx_ = nullptr;
        ready_ = false;
        extra_.clear();
    }

    bool ready() const { return ready_; }
    bool has_headers() const { return !extra_.empty(); }
    bool headers_sent() const { return sent_headers_; }
    void mark_headers_sent() { sent_headers_ = true; }
    void reset_headers_sent() { sent_headers_ = false; }
    const std::vector<uint8_t>& headers() const { return extra_; }
    bool request_idr() {
        if (!ready_) return false;
        RK_U32 force_idr = 1;
        if (mpi_->control(ctx_, MPP_ENC_SET_IDR_FRAME, &force_idr) != MPP_OK) {
            spdlog::warn("MPP_ENC_SET_IDR_FRAME failed");
            return false;
        }
        return true;
    }

    bool encode_frame(MppFrame src_frame, std::vector<uint8_t>& out) {
        if (!ready_) return false;
        MppFrame enc_frame = nullptr;
        if (mpp_frame_init(&enc_frame) != MPP_OK) {
            return false;
        }
        const uint32_t width = mpp_frame_get_width(src_frame);
        const uint32_t height = mpp_frame_get_height(src_frame);
        const uint32_t hor_stride = mpp_frame_get_hor_stride(src_frame);
        const uint32_t ver_stride = mpp_frame_get_ver_stride(src_frame);
        const MppFrameFormat fmt = mpp_frame_get_fmt(src_frame);
        mpp_frame_set_width(enc_frame, width);
        mpp_frame_set_height(enc_frame, height);
        mpp_frame_set_hor_stride(enc_frame, hor_stride);
        mpp_frame_set_ver_stride(enc_frame, ver_stride);
        mpp_frame_set_fmt(enc_frame, fmt);
        mpp_frame_set_pts(enc_frame, mpp_frame_get_pts(src_frame));
        mpp_frame_set_buffer(enc_frame, mpp_frame_get_buffer(src_frame));
        size_t buf_size = mpp_buffer_get_size(mpp_frame_get_buffer(src_frame));
        if (buf_size > 0) {
            size_t expected = buf_size;
            if (fmt == MPP_FMT_YUV420SP && hor_stride > 0 && ver_stride > 0) {
                expected = static_cast<size_t>(hor_stride) * static_cast<size_t>(ver_stride) * 3 / 2;
            }
            size_t final_size = (expected > 0 && expected <= buf_size) ? expected : buf_size;
            mpp_frame_set_buf_size(enc_frame, static_cast<RK_U32>(final_size));
        }

        if (mpi_->encode_put_frame(ctx_, enc_frame) != MPP_OK) {
            mpp_frame_deinit(&enc_frame);
            return false;
        }
        mpp_frame_deinit(&enc_frame);

        bool got = false;
        out.clear();
        for (int i = 0; i < 32; i++) {
            MppPacket pkt = nullptr;
            if (mpi_->encode_get_packet(ctx_, &pkt) != MPP_OK || !pkt) {
                break;
            }
            void* ptr = mpp_packet_get_pos(pkt);
            size_t len = mpp_packet_get_length(pkt);
            if (ptr && len > 0) {
                size_t offset = out.size();
                out.resize(offset + len);
                std::memcpy(out.data() + offset, ptr, len);
                got = true;
            }
            mpp_packet_deinit(&pkt);
        }
        return got;
    }

private:
    MppCtx ctx_{nullptr};
    MppApi* mpi_{nullptr};
    MppEncCfg cfg_{nullptr};
    bool ready_{false};
    bool sent_headers_{false};
    uint32_t width_{0};
    uint32_t height_{0};
    MppFrameFormat fmt_{MPP_FMT_BUTT};
    VideoCodec codec_{VideoCodec::H265};
    std::vector<uint8_t> extra_;
};
} // namespace

#if defined(HAVE_RGA) && HAVE_RGA
struct RgaDumbBuffer {
    uint32_t handle{0};
    int prime_fd{-1};
    size_t size{0};
};

static bool create_dumb_buffer(int drm_fd, uint32_t width, uint32_t height, uint32_t bpp, RgaDumbBuffer& out) {
    drm_mode_create_dumb dmcd{};
    dmcd.width = width;
    dmcd.height = height;
    dmcd.bpp = bpp;
    if (ioctl(drm_fd, DRM_IOCTL_MODE_CREATE_DUMB, &dmcd) < 0) {
        return false;
    }
    drm_prime_handle dph{};
    dph.handle = dmcd.handle;
    dph.flags = DRM_CLOEXEC | DRM_RDWR;
    if (ioctl(drm_fd, DRM_IOCTL_PRIME_HANDLE_TO_FD, &dph) < 0) {
        drm_mode_destroy_dumb dmd{};
        dmd.handle = dmcd.handle;
        ioctl(drm_fd, DRM_IOCTL_MODE_DESTROY_DUMB, &dmd);
        return false;
    }
    out.handle = dmcd.handle;
    out.prime_fd = dph.fd;
    out.size = dmcd.size;
    return true;
}

static void destroy_dumb_buffer(int drm_fd, RgaDumbBuffer& buf) {
    if (buf.prime_fd >= 0) {
        close(buf.prime_fd);
        buf.prime_fd = -1;
    }
    if (buf.handle) {
        drm_mode_destroy_dumb dmd{};
        dmd.handle = buf.handle;
        ioctl(drm_fd, DRM_IOCTL_MODE_DESTROY_DUMB, &dmd);
        buf.handle = 0;
    }
    buf.size = 0;
}

class RgaColorTrans {
public:
    bool init(int drm_fd,
              uint32_t width,
              uint32_t height,
              uint32_t hor_stride,
              uint32_t ver_stride,
              MppFrameFormat fmt,
              bool apply_cpu_lut,
              int csc_mode) {
        if (ready_) {
            if (width_ == width &&
                height_ == height &&
                hor_stride_ == hor_stride &&
                ver_stride_ == ver_stride &&
                apply_cpu_lut_ == apply_cpu_lut &&
                csc_mode_ == csc_mode) {
                return true;
            }
            cleanup();
        }
        if (drm_fd < 0) return false;
        if (fmt != MPP_FMT_YUV420SP) return false;

        static std::atomic<bool> rga_inited{false};
        if (!rga_inited.exchange(true)) {
            if (c_RkRgaInit() != 0) {
                spdlog::warn("RGA init failed; colortrans may not work.");
            }
        }

        drm_fd_ = drm_fd;
        width_ = width;
        height_ = height;
        hor_stride_ = hor_stride;
        ver_stride_ = ver_stride;
        apply_cpu_lut_ = apply_cpu_lut;
        csc_mode_ = csc_mode;

        if (mpp_buffer_group_get_external(&out_group_, MPP_BUFFER_TYPE_DRM) != MPP_OK) {
            cleanup();
            return false;
        }

        for (int i = 0; i < pool_size_; i++) {
            RgaDumbBuffer buf;
            if (!create_dumb_buffer(drm_fd_, hor_stride_, ver_stride_ * 2, 8, buf)) {
                cleanup();
                return false;
            }
            MppBufferInfo info{};
            info.type = MPP_BUFFER_TYPE_DRM;
            info.size = buf.size;
            info.fd = buf.prime_fd;
            if (mpp_buffer_commit(out_group_, &info) != MPP_OK) {
                destroy_dumb_buffer(drm_fd_, buf);
                cleanup();
                return false;
            }
            MppBuffer mpp_buf = nullptr;
            if (mpp_buffer_get(out_group_, &mpp_buf, info.size) != MPP_OK || !mpp_buf) {
                destroy_dumb_buffer(drm_fd_, buf);
                cleanup();
                return false;
            }
            PoolBuf pool{};
            pool.drm = buf;
            pool.mpp = mpp_buf;
            pool.rga = wrapbuffer_fd(buf.prime_fd, width_, height_, RK_FORMAT_YCbCr_420_SP,
                                     static_cast<int>(hor_stride_), static_cast<int>(ver_stride_));
            pool_.push_back(pool);
            free_queue_.push(i);
        }

        ready_ = true;
        return true;
    }

    bool ready() const { return ready_; }

    bool process(MppBuffer src, MppBuffer& out, int& out_index) {
        if (!ready_) return false;
        if (!src) return false;
        if (free_queue_.empty()) {
            return false;
        }

        MppBufferInfo info{};
        if (mpp_buffer_info_get(src, &info) != MPP_OK) {
            return false;
        }
        const int src_fd = info.fd;
        if (src_fd < 0) {
            return false;
        }

        int index = free_queue_.front();
        free_queue_.pop();
        PoolBuf& pool = pool_[index];

        rga_buffer_t src_wrap = wrapbuffer_fd(src_fd, width_, height_, RK_FORMAT_YCbCr_420_SP,
                                              static_cast<int>(hor_stride_), static_cast<int>(ver_stride_));
        bool copy_ok = false;
        if (csc_mode_ != 0) {
            src_wrap.color_space_mode = csc_mode_;
            pool.rga.color_space_mode = csc_mode_;
        }

        IM_STATUS ret = imcopy(src_wrap, pool.rga);
        if (ret == IM_STATUS_SUCCESS) {
            copy_ok = true;
        } else if (csc_mode_ != 0) {
            IM_STATUS cvt_ret = imcvtcolor_t(src_wrap, pool.rga, RK_FORMAT_YCbCr_420_SP,
                                             RK_FORMAT_YCbCr_420_SP, csc_mode_, 1);
            if (cvt_ret == IM_STATUS_SUCCESS) {
                copy_ok = true;
            } else {
                rga_info_t src_info{};
                rga_info_t dst_info{};
                src_info.fd = src_fd;
                src_info.mmuFlag = 1;
                src_info.sync_mode = 1;
                src_info.format = RK_FORMAT_YCbCr_420_SP;
                src_info.color_space_mode = csc_mode_;
                src_info.rect.xoffset = 0;
                src_info.rect.yoffset = 0;
                src_info.rect.width = static_cast<int>(width_);
                src_info.rect.height = static_cast<int>(height_);
                src_info.rect.wstride = static_cast<int>(hor_stride_);
                src_info.rect.hstride = static_cast<int>(ver_stride_);
                src_info.rect.format = RK_FORMAT_YCbCr_420_SP;

                dst_info.fd = pool.drm.prime_fd;
                dst_info.mmuFlag = 1;
                dst_info.sync_mode = 1;
                dst_info.format = RK_FORMAT_YCbCr_420_SP;
                dst_info.color_space_mode = csc_mode_;
                dst_info.rect.xoffset = 0;
                dst_info.rect.yoffset = 0;
                dst_info.rect.width = static_cast<int>(width_);
                dst_info.rect.height = static_cast<int>(height_);
                dst_info.rect.wstride = static_cast<int>(hor_stride_);
                dst_info.rect.hstride = static_cast<int>(ver_stride_);
                dst_info.rect.format = RK_FORMAT_YCbCr_420_SP;

                int blit_ret = c_RkRgaBlit(&src_info, &dst_info, nullptr);
                if (blit_ret == 0) {
                    copy_ok = true;
                } else {
                    spdlog::warn("RGA CSC failed: imcopy={}, imcvtcolor={}, RgaBlit={}", 
                        static_cast<int>(ret), static_cast<int>(cvt_ret), static_cast<int>(blit_ret));
                }
            }
        } else {
            spdlog::warn("RGA imcopy failed: {}", static_cast<int>(ret));
        }
        if (!copy_ok) {
            free_queue_.push(index);
            return false;
        }

        if (apply_cpu_lut_) {
            void* map = mmap(nullptr, pool.drm.size, PROT_READ | PROT_WRITE, MAP_SHARED, pool.drm.prime_fd, 0);
            if (map == MAP_FAILED) {
                spdlog::warn("Failed to mmap NV12 buffer for luma adjustment");
                free_queue_.push(index);
                return false;
            }
            static uint8_t y_lut[256];
            static uint8_t uv_lut[256];
            static bool lut_init = false;
            if (!lut_init) {
                const float offset = -0.15f;
                const float gain = 2.5f;
                for (int i = 0; i < 256; i++) {
                    float v = (float)i / 255.0f;
                    float y = (v + offset) * gain;
                    if (y < 0.0f) y = 0.0f;
                    if (y > 1.0f) y = 1.0f;
                    y_lut[i] = (uint8_t)std::lround(y * 255.0f);

                    float uv = ((float)i - 128.0f) * gain + 128.0f;
                    if (uv < 0.0f) uv = 0.0f;
                    if (uv > 255.0f) uv = 255.0f;
                    uv_lut[i] = (uint8_t)std::lround(uv);
                }
                lut_init = true;
            }
            uint8_t* y_plane = static_cast<uint8_t*>(map);
            const size_t y_size = static_cast<size_t>(hor_stride_) * static_cast<size_t>(ver_stride_);
            for (size_t i = 0; i < y_size; i++) {
                y_plane[i] = y_lut[y_plane[i]];
            }
            uint8_t* uv_plane = y_plane + y_size;
            const size_t uv_size = y_size / 2;
            for (size_t i = 0; i < uv_size; i++) {
                uv_plane[i] = uv_lut[uv_plane[i]];
            }
            munmap(map, pool.drm.size);
        }

        mpp_buffer_inc_ref(pool.mpp);
        out = pool.mpp;
        out_index = index;
        return true;
    }

    void release(int index) {
        if (!ready_) return;
        if (index < 0 || index >= static_cast<int>(pool_.size())) return;
        free_queue_.push(index);
    }

    void cleanup() {
        for (auto& pool : pool_) {
            if (pool.mpp) {
                mpp_buffer_put(pool.mpp);
                pool.mpp = nullptr;
            }
            destroy_dumb_buffer(drm_fd_, pool.drm);
        }
        pool_.clear();
        while (!free_queue_.empty()) free_queue_.pop();
        if (out_group_) {
            mpp_buffer_group_put(out_group_);
            out_group_ = nullptr;
        }
        ready_ = false;
    }

private:
    struct PoolBuf {
        RgaDumbBuffer drm{};
        MppBuffer mpp{nullptr};
        rga_buffer_t rga{};
    };

    const int pool_size_{12};
    int drm_fd_{-1};
    uint32_t width_{0};
    uint32_t height_{0};
    uint32_t hor_stride_{0};
    uint32_t ver_stride_{0};
    bool apply_cpu_lut_{false};
    int csc_mode_{0};
    bool ready_{false};

    MppBufferGroup out_group_{nullptr};
    std::vector<PoolBuf> pool_;
    std::queue<int> free_queue_;
};

void RgaColorTransDeleter::operator()(RgaColorTrans* ptr) const {
    delete ptr;
}

class RgaRgbToNv12 {
public:
    void set_mode(int mode) { rgb2yuv_mode_ = mode; }
    void set_src_format(int fmt) { rgb_format_ = fmt; }
    void set_use_mpp_buffer(bool use) { use_mpp_buffer_ = use; }

    bool init(int drm_fd, uint32_t width, uint32_t height, uint32_t hor_stride, uint32_t ver_stride) {
        if (ready_) {
            if (width_ == width &&
                height_ == height &&
                hor_stride_ == hor_stride &&
                ver_stride_ == ver_stride &&
                use_mpp_buffer_active_ == use_mpp_buffer_) {
                return true;
            }
            cleanup();
        }
        drm_fd_ = drm_fd;
        width_ = width;
        height_ = height;
        hor_stride_ = hor_stride;
        ver_stride_ = ver_stride;

        if (use_mpp_buffer_) {
            if (mpp_buffer_group_get_internal(&out_group_, MPP_BUFFER_TYPE_ION) != MPP_OK) {
                spdlog::warn("MPP internal buffer group init failed; falling back to DRM buffers.");
                use_mpp_buffer_ = false;
            }
        }
        if (!use_mpp_buffer_) {
            if (drm_fd_ < 0) return false;
            if (mpp_buffer_group_get_external(&out_group_, MPP_BUFFER_TYPE_DRM) != MPP_OK) {
                cleanup();
                return false;
            }
        }

        for (int i = 0; i < pool_size_; i++) {
            RgaDumbBuffer buf;
            MppBufferInfo info{};
            MppBuffer mpp_buf = nullptr;
            if (use_mpp_buffer_) {
                const size_t buf_size = static_cast<size_t>(hor_stride_) * static_cast<size_t>(ver_stride_) * 3 / 2;
                if (mpp_buffer_get(out_group_, &mpp_buf, buf_size) != MPP_OK || !mpp_buf) {
                    cleanup();
                    return false;
                }
                int fd = mpp_buffer_get_fd(mpp_buf);
                if (fd < 0) {
                    mpp_buffer_put(mpp_buf);
                    cleanup();
                    return false;
                }
                buf.prime_fd = fd;
            } else {
                if (!create_dumb_buffer(drm_fd_, hor_stride_, ver_stride_ * 2, 8, buf)) {
                    cleanup();
                    return false;
                }
                info.type = MPP_BUFFER_TYPE_DRM;
                info.size = buf.size;
                info.fd = buf.prime_fd;
                if (mpp_buffer_commit(out_group_, &info) != MPP_OK) {
                    destroy_dumb_buffer(drm_fd_, buf);
                    cleanup();
                    return false;
                }
                if (mpp_buffer_get(out_group_, &mpp_buf, info.size) != MPP_OK || !mpp_buf) {
                    destroy_dumb_buffer(drm_fd_, buf);
                    cleanup();
                    return false;
                }
            }
            PoolBuf pool{};
            pool.drm = buf;
            pool.mpp = mpp_buf;
            pool.owns_drm = !use_mpp_buffer_;
            pool.rga = wrapbuffer_fd(buf.prime_fd, width_, height_, RK_FORMAT_YCbCr_420_SP,
                                     static_cast<int>(hor_stride_), static_cast<int>(ver_stride_));
            pool_.push_back(pool);
            free_queue_.push(i);
        }

        ready_ = true;
        use_mpp_buffer_active_ = use_mpp_buffer_;
        return true;
    }

    bool process(int rgb_fd, uint32_t width, uint32_t height, uint32_t stride, MppBuffer& out, int& out_index) {
        if (!ready_) return false;
        if (rgb_fd < 0) return false;
        if (free_queue_.empty()) return false;

        int index = free_queue_.front();
        free_queue_.pop();
        PoolBuf& pool = pool_[index];

        const uint32_t wstride = stride / 4;
        const int formats[] = {
            rgb_format_ ? rgb_format_ : RK_FORMAT_XRGB_8888,
            rgb_format_ ? rgb_format_ : RK_FORMAT_XBGR_8888,
            rgb_format_ ? rgb_format_ : RK_FORMAT_BGRX_8888,
            rgb_format_ ? rgb_format_ : RK_FORMAT_RGBX_8888
        };
        const int modes[] = {rgb2yuv_mode_, 0};
        bool converted = false;
        IM_STATUS last_ret = IM_STATUS_FAILED;
        for (int fmt : formats) {
            if (rgb_format_ && fmt != rgb_format_) {
                continue;
            }
            rga_buffer_t src_wrap = wrapbuffer_fd(rgb_fd, width, height, fmt,
                                                  static_cast<int>(wstride), static_cast<int>(height));
            for (int mode : modes) {
                IM_STATUS ret = imcvtcolor_t(src_wrap, pool.rga, fmt,
                                             RK_FORMAT_YCbCr_420_SP, mode, 1);
                if (ret == IM_STATUS_SUCCESS) {
                    converted = true;
                    break;
                }
                last_ret = ret;
            }
            if (converted) {
                break;
            }
        }
        if (!converted) {
            spdlog::warn("RGA RGB->NV12 failed: {}", static_cast<int>(last_ret));
            free_queue_.push(index);
            return false;
        }
        static std::atomic<bool> first_ok{false};
        if (!first_ok.exchange(true)) {
            spdlog::info("Record colortrans (shader) RGB->NV12 conversion OK (mode {}, fmt {})", rgb2yuv_mode_, rgb_format_);
        }

        mpp_buffer_inc_ref(pool.mpp);
        out = pool.mpp;
        out_index = index;
        return true;
    }

    void release(int index) {
        if (!ready_) return;
        if (index < 0 || index >= static_cast<int>(pool_.size())) return;
        free_queue_.push(index);
    }

    void cleanup() {
        for (auto& pool : pool_) {
            if (pool.mpp) {
                mpp_buffer_put(pool.mpp);
                pool.mpp = nullptr;
            }
            if (pool.owns_drm) {
                destroy_dumb_buffer(drm_fd_, pool.drm);
            }
        }
        pool_.clear();
        while (!free_queue_.empty()) free_queue_.pop();
        if (out_group_) {
            mpp_buffer_group_put(out_group_);
            out_group_ = nullptr;
        }
        ready_ = false;
    }

private:
    struct PoolBuf {
        RgaDumbBuffer drm{};
        MppBuffer mpp{nullptr};
        rga_buffer_t rga{};
        bool owns_drm{true};
    };

    const int pool_size_{12};
    int drm_fd_{-1};
    uint32_t width_{0};
    uint32_t height_{0};
    uint32_t hor_stride_{0};
    uint32_t ver_stride_{0};
    int rgb2yuv_mode_{rgb2yuv_709_full};
    int rgb_format_{0};
    bool ready_{false};
    bool use_mpp_buffer_{true};
    bool use_mpp_buffer_active_{true};

    MppBufferGroup out_group_{nullptr};
    std::vector<PoolBuf> pool_;
    std::queue<int> free_queue_;
};

void RgaRgbToNv12Deleter::operator()(RgaRgbToNv12* ptr) const {
    delete ptr;
}
#endif

ProcessingPipeline::~ProcessingPipeline() = default;

void ProcessingPipeline::configure(const ProcessingOptions& opts) {
    std::lock_guard<std::mutex> lock(mutex_);
    opts_ = opts;
    shader_checked_.store(false);
    shader_available_.store(false);
    encode_warned_.store(false);
    gl_ready_.store(false);
    gl_warned_.store(false);
    shader_warned_ = false;
    processed_logged_.store(false);
    gl_ok_.store(0);
    gl_fail_.store(0);
    record_colortrans_ready_ = false;
    record_colortrans_warned_ = false;
    record_colortrans_mode_ = parse_record_colortrans_mode(opts_.record_colortrans_mode);
#if defined(HAVE_RGA) && HAVE_RGA
    record_colortrans_csc_mode_ = parse_record_colortrans_csc(opts_.record_colortrans_csc);
    record_rgb2yuv_mode_ = parse_record_rgb2yuv_mode(opts_.record_rgb2yuv_mode);
    record_rgb_format_ = parse_record_rgb_format(opts_.record_rgb_format);
    if (record_colortrans_mode_ == RecordColorTransMode::RgaCsc && record_colortrans_csc_mode_ == 0) {
        spdlog::warn("Record colortrans CSC mode '{}' is invalid or unsupported; disabling record colortrans.", opts_.record_colortrans_csc);
        record_colortrans_mode_ = RecordColorTransMode::Off;
    }
#else
    record_colortrans_csc_mode_ = 0;
    record_rgb2yuv_mode_ = 0;
    record_rgb_format_ = 0;
#endif
    if (opts_.record_colortrans &&
        opts_.record_colortrans_mode != "auto" &&
        opts_.record_colortrans_mode != "rga" &&
        opts_.record_colortrans_mode != "cpu" &&
        opts_.record_colortrans_mode != "off") {
        spdlog::warn("Unknown record colortrans mode {}; defaulting to auto.", opts_.record_colortrans_mode);
    }
    if (opts_.record_rgb2yuv_mode == "709l" || opts_.record_rgb2yuv_mode == "601l") {
        spdlog::warn("Record rgb2yuv mode {} not supported by librga; using full range.", opts_.record_rgb2yuv_mode);
    } else if (opts_.record_rgb2yuv_mode != "709f" && opts_.record_rgb2yuv_mode != "601f") {
        spdlog::warn("Unknown record rgb2yuv mode {}; defaulting to 709f.", opts_.record_rgb2yuv_mode);
    }
    if (opts_.record_rgb_format != "xrgb" &&
        opts_.record_rgb_format != "xbgr" &&
        opts_.record_rgb_format != "rgbx" &&
        opts_.record_rgb_format != "bgrx" &&
        opts_.record_rgb_format != "auto") {
        spdlog::warn("Unknown record rgb format {}; defaulting to xrgb.", opts_.record_rgb_format);
    }

    if (opts_.encode_processed && !enc_run_.exchange(true)) {
        enc_thread_ = std::thread(&ProcessingPipeline::encoder_loop, this);
    }

    if (opts_.shader_enabled && !opts_.shader_name.empty()) {
        shader_path_cache_ = (std::filesystem::path(opts_.shader_dir) / opts_.shader_name).string();
        // If the user omitted the extension, add .glsl by default.
        if (!std::filesystem::path(shader_path_cache_).has_extension()) {
            shader_path_cache_ += ".glsl";
        }
    } else {
        shader_path_cache_.clear();
    }

    spdlog::info("Processing options: shader_enabled={}, shader_name={}, shader_dir={}, shader_for_display={}, shader_for_encode={}, encode_processed={}, encode_bitrate_kbps={}, encode_fps={}, record_colortrans={}, record_colortrans_mode={}, record_colortrans_csc={}, record_rgb2yuv_mode={}, record_rgb_format={}",
                 opts_.shader_enabled, opts_.shader_name, opts_.shader_dir, opts_.shader_for_display, opts_.shader_for_encode,
                 opts_.encode_processed, opts_.encode_bitrate_kbps, opts_.encode_fps,
                 opts_.record_colortrans, opts_.record_colortrans_mode, opts_.record_colortrans_csc, opts_.record_rgb2yuv_mode, opts_.record_rgb_format);

    if (opts_.shader_enabled && gl_ready_.load() && shader_available_.load()) {
        std::lock_guard<std::mutex> gl_lock(gl_mutex_);
        gl_.reload_shader(shader_path_cache_);
    }
}

void ProcessingPipeline::set_video_info(uint32_t width, uint32_t height, uint32_t hor_stride, uint32_t ver_stride, MppFrameFormat fmt) {
    std::lock_guard<std::mutex> lock(mutex_);
    video_width_ = width;
    video_height_ = height;
    hor_stride_ = hor_stride;
    ver_stride_ = ver_stride;
    const uint32_t base_hor_stride = hor_stride_ ? hor_stride_ : video_width_;
    const uint32_t base_ver_stride = ver_stride_ ? ver_stride_ : video_height_;
    encode_hor_stride_ = align_up(base_hor_stride, 16);
    encode_ver_stride_ = align_up(base_ver_stride, 16);
    format_ = fmt;
    spdlog::info("Processing set_video_info: shader_enabled={}, drm_fd={}, gl_ready={}, encode_stride={}x{}",
                 opts_.shader_enabled, drm_fd_, gl_ready_.load(), encode_hor_stride_, encode_ver_stride_);
    refresh_shader_if_needed();
    if (opts_.shader_enabled && opts_.shader_for_display && !gl_ready_.load() && drm_fd_ >= 0) {
        spdlog::info("Attempting GL init for shader processing");
        std::lock_guard<std::mutex> gl_lock(gl_mutex_);
        if (!gl_ready_.load()) {
            if (gl_.init(drm_fd_, video_width_, video_height_)) {
                if (shader_available_.load()) {
                    gl_.reload_shader(shader_path_cache_);
                }
                gl_ready_.store(true);
                spdlog::info("GL processing enabled for {}x{}", video_width_, video_height_);
            } else if (!gl_warned_.exchange(true)) {
                spdlog::warn("GL processing init failed; shader will be bypassed.");
            }
        }
    }
    maybe_init_record_colortrans();
}

void ProcessingPipeline::set_dvr(Dvr* dvr, int framerate) {
    std::lock_guard<std::mutex> lock(mutex_);
    dvr_ = dvr;
    if (framerate <= 0) {
        spdlog::warn("DVR framerate invalid ({}); defaulting to 60", framerate);
        dvr_fps_ = 60;
    } else {
        dvr_fps_ = framerate;
    }
}

void ProcessingPipeline::update_dvr_fps(int framerate) {
    std::lock_guard<std::mutex> lock(mutex_);
    if (framerate > 0) {
        dvr_fps_ = framerate;
    }
}

void ProcessingPipeline::shutdown() {
    if (enc_run_.exchange(false)) {
        enc_cv_.notify_all();
        if (enc_thread_.joinable()) {
            enc_thread_.join();
        }
    }
}

bool ProcessingPipeline::file_exists(const std::string& path) const {
    std::error_code ec;
    return std::filesystem::exists(path, ec);
}

void ProcessingPipeline::refresh_shader_if_needed() {
    if (shader_checked_.load() || !opts_.shader_enabled) {
        return;
    }

    shader_checked_.store(true);
    if (shader_path_cache_.empty()) {
        spdlog::warn("Shader enabled but path is empty; falling back to passthrough.");
        return;
    }

    if (file_exists(shader_path_cache_)) {
        shader_available_.store(true);
        spdlog::info("Shader selected: {}", shader_path_cache_);
    } else {
        spdlog::warn("Shader file not found at {}; running passthrough.", shader_path_cache_);
    }
}

MppFrame ProcessingPipeline::process_frame(MppFrame frame, uint32_t& out_fb_id, bool& out_is_processed) {
    refresh_shader_if_needed();
    out_is_processed = false;
    out_fb_id = 0;
    frame_total_++;
    auto maybe_log_stats = [&]() {
        uint64_t ts_now_ms = now_ms();
        if (ts_now_ms - last_log_ms_ >= 1000) {
            last_log_ms_ = ts_now_ms;
            spdlog::info("Frame stats: total={}, gl_ok={}, gl_fail={}, last_render_ms={}",
                         frame_total_, gl_ok_.load(), gl_fail_.load(), last_render_ms_.load());
        }
    };
    if (pending_rgb_.valid) {
        if (pending_rgb_.fd >= 0) {
            close(pending_rgb_.fd);
        }
        pending_rgb_ = {};
    }

    const bool want_display_shader = opts_.shader_enabled && opts_.shader_for_display;
    const bool want_encode_shader = opts_.shader_enabled && opts_.shader_for_encode && opts_.encode_processed;
    if (want_display_shader && gl_ready_.load()) {
        uint64_t start_ms = now_ms();
        std::lock_guard<std::mutex> gl_lock(gl_mutex_);
        auto pf = gl_.process(frame);
        last_render_ms_.store(now_ms() - start_ms);
        if (pf.has_value()) {
            gl_ok_.fetch_add(1);
            out_fb_id = pf->fb_id;
            out_is_processed = true;
            if (want_encode_shader && pf->bo) {
                int fd = gbm_bo_get_fd(pf->bo);
                if (fd >= 0) {
                    pending_rgb_.fd = fd;
                    pending_rgb_.width = gl_.target_width();
                    pending_rgb_.height = gl_.target_height();
                    pending_rgb_.stride = gbm_bo_get_stride(pf->bo);
                    pending_rgb_.format = RK_FORMAT_XRGB_8888;
                    pending_rgb_.valid = true;
                }
            }
            if (!processed_logged_.exchange(true)) {
                spdlog::info("First processed frame rendered via GL shader");
            }
            maybe_log_stats();
            return frame; // framebuffer id will be used instead of mpp buffer mapping
        }
        gl_fail_.fetch_add(1);
    } else if (want_display_shader && !gl_ready_.load() && !shader_warned_) {
        shader_warned_ = true;
        spdlog::warn("Shader requested but GL processing is not ready; running passthrough.");
    }
    maybe_log_stats();
    return frame;
}

bool ProcessingPipeline::ensure_gl_ready_for_encode() {
    if (!opts_.shader_enabled || !opts_.shader_for_encode) {
        return false;
    }
    refresh_shader_if_needed();
    std::lock_guard<std::mutex> gl_lock(gl_mutex_);
    if (gl_ready_.load()) {
        return true;
    }
    if (drm_fd_ < 0 || video_width_ == 0 || video_height_ == 0) {
        return false;
    }
    spdlog::info("Attempting GL init for encode-only shader processing");
    if (gl_.init(drm_fd_, video_width_, video_height_)) {
        if (shader_available_.load()) {
            gl_.reload_shader(shader_path_cache_);
        }
        gl_ready_.store(true);
        spdlog::info("GL processing enabled for encode-only {}x{}", video_width_, video_height_);
        return true;
    }
    if (!gl_warned_.exchange(true)) {
        spdlog::warn("GL processing init failed; shader will be bypassed.");
    }
    return false;
}

std::optional<ProcessedFrame> ProcessingPipeline::process_shader_frame(MppBuffer buffer,
                                                                       uint32_t width,
                                                                       uint32_t height,
                                                                       uint32_t hor_stride,
                                                                       uint32_t ver_stride,
                                                                       MppFrameFormat fmt,
                                                                       uint64_t pts) {
    if (!buffer) {
        return std::nullopt;
    }
    MppFrame gl_frame = nullptr;
    if (mpp_frame_init(&gl_frame) != MPP_OK) {
        return std::nullopt;
    }
    mpp_frame_set_width(gl_frame, width);
    mpp_frame_set_height(gl_frame, height);
    mpp_frame_set_hor_stride(gl_frame, hor_stride);
    mpp_frame_set_ver_stride(gl_frame, ver_stride);
    mpp_frame_set_fmt(gl_frame, fmt);
    mpp_frame_set_pts(gl_frame, pts);
    mpp_frame_set_buffer(gl_frame, buffer);
    std::optional<ProcessedFrame> pf;
    {
        std::lock_guard<std::mutex> gl_lock(gl_mutex_);
        pf = gl_.process(gl_frame);
    }
    mpp_frame_deinit(&gl_frame);
    return pf;
}

void ProcessingPipeline::submit_for_encoding(MppFrame frame, uint64_t pts_ms) {
    (void)frame;
    (void)pts_ms;
    auto clear_pending_rgb = [&]() {
        if (pending_rgb_.valid) {
            if (pending_rgb_.fd >= 0) {
                close(pending_rgb_.fd);
            }
            pending_rgb_ = {};
        }
    };
    if (!opts_.encode_processed) {
        clear_pending_rgb();
        return;
    }
    if (!dvr_) {
        bool expected = false;
        if (encode_warned_.compare_exchange_strong(expected, true)) {
            spdlog::warn("Processed encode requested but DVR is not configured; ignoring.");
        }
        clear_pending_rgb();
        return;
    }
    bool recording = (dvr_enabled != 0);
    if (!recording) {
        clear_pending_rgb();
    }
    MppBuffer buffer = mpp_frame_get_buffer(frame);
    if (!buffer) {
        clear_pending_rgb();
        return;
    }
    mpp_buffer_inc_ref(buffer);
    static std::atomic<bool> first_enqueue{false};
    EncJob job{};
    job.buffer = buffer;
    job.recording = recording;
    job.width = mpp_frame_get_width(frame);
    job.height = mpp_frame_get_height(frame);
    job.hor_stride = mpp_frame_get_hor_stride(frame);
    job.ver_stride = mpp_frame_get_ver_stride(frame);
    job.fmt = mpp_frame_get_fmt(frame);
    job.pts = pts_ms;
    if (pending_rgb_.valid && recording) {
        job.rgb_fd = pending_rgb_.fd;
        job.rgb_width = pending_rgb_.width;
        job.rgb_height = pending_rgb_.height;
        job.rgb_stride = pending_rgb_.stride;
        job.rgb_format = pending_rgb_.format;
        pending_rgb_ = {};
    }

      {
          std::lock_guard<std::mutex> lock(enc_mutex_);
          if (enc_queue_.size() >= 4) {
              EncJob drop = enc_queue_.front();
              enc_queue_.pop();
              if (drop.buffer) {
                  mpp_buffer_put(drop.buffer);
              }
              if (drop.rgb_fd >= 0) {
                  close(drop.rgb_fd);
              }
              enc_drop_.fetch_add(1);
          }
          enc_queue_.push(job);
      }
    enc_cv_.notify_one();
    if (!first_enqueue.exchange(true)) {
        spdlog::info("Processed encode: first frame queued");
    }
}

void ProcessingPipeline::maybe_init_record_colortrans() {
    if (!opts_.record_colortrans) {
        return;
    }
    if (!opts_.encode_processed) {
        return;
    }
    if (record_colortrans_mode_ == RecordColorTransMode::Off) {
        return;
    }
    if (record_colortrans_ready_) {
        return;
    }
    if (format_ != MPP_FMT_YUV420SP) {
        if (!record_colortrans_warned_) {
            record_colortrans_warned_ = true;
            spdlog::warn("Record colortrans requested but format is not NV12; bypassing.");
        }
        return;
    }
#if defined(HAVE_RGA) && HAVE_RGA
    RecordColorTransMode effective_mode = record_colortrans_mode_;
    if (effective_mode == RecordColorTransMode::Auto) {
        effective_mode = RecordColorTransMode::RgaCsc;
    }
    const bool use_cpu_lut = (effective_mode == RecordColorTransMode::Cpu);
    const int csc_mode = (effective_mode == RecordColorTransMode::RgaCsc) ? record_colortrans_csc_mode_ : 0;
    if (effective_mode == RecordColorTransMode::RgaCsc && csc_mode == 0) {
        if (!record_colortrans_warned_) {
            record_colortrans_warned_ = true;
            spdlog::warn("Record colortrans CSC mode is invalid; bypassing.");
        }
        return;
    }
    if (!rga_colortrans_) {
        rga_colortrans_ = std::unique_ptr<RgaColorTrans, RgaColorTransDeleter>(new RgaColorTrans());
    }
    if (rga_colortrans_->init(drm_fd_, video_width_, video_height_, hor_stride_, ver_stride_, format_, use_cpu_lut, csc_mode)) {
        record_colortrans_ready_ = true;
        if (use_cpu_lut) {
            spdlog::info("Record colortrans enabled via RGA + CPU LUT (slow)");
        } else {
            spdlog::info("Record colortrans enabled via RGA CSC mode {}", opts_.record_colortrans_csc);
        }
    } else if (!record_colortrans_warned_) {
        record_colortrans_warned_ = true;
        spdlog::warn("Record colortrans requested but RGA init failed; bypassing.");
    }
#else
    if (!record_colortrans_warned_) {
        record_colortrans_warned_ = true;
        spdlog::warn("Record colortrans requested but librga not available; bypassing.");
    }
#endif
}

void ProcessingPipeline::encoder_loop() {
    MppEncoder encoder;
    static std::atomic<bool> first_encode{false};
    static std::atomic<bool> size_warned{false};
    bool last_recording = false;
    bool reset_headers_on_ready = false;
    bool force_idr_on_ready = false;
    struct EncConfig {
        uint32_t width{0};
        uint32_t height{0};
        uint32_t hor_stride{0};
        uint32_t ver_stride{0};
        MppFrameFormat fmt{MPP_FMT_BUTT};
        int fps{0};
        int bitrate_kbps{0};
        VideoCodec codec{VideoCodec::H265};
        bool valid{false};
    };
    EncConfig enc_cfg{};
    using clock = std::chrono::steady_clock;
    auto last_log = clock::now();
    uint64_t last_drop = 0;
    uint64_t last_throttle = 0;
    uint64_t last_ok = 0;
    uint64_t last_fail = 0;
    uint64_t last_pts = 0;
    while (enc_run_) {
        EncJob job{};
        {
            std::unique_lock<std::mutex> lock(enc_mutex_);
            enc_cv_.wait(lock, [&]() { return !enc_queue_.empty() || !enc_run_; });
            if (!enc_run_) {
                break;
            }
        job = enc_queue_.front();
        enc_queue_.pop();
    }
    if (!job.buffer) {
        continue;
    }
    if (job.hor_stride == 0) {
        job.hor_stride = job.width;
    }
    if (job.ver_stride == 0) {
        job.ver_stride = job.height;
    }
    if (!job.recording) {
        last_recording = false;
        mpp_buffer_put(job.buffer);
        if (job.rgb_fd >= 0) {
            close(job.rgb_fd);
        }
        continue;
    }
    if (opts_.encode_fps > 0) {
        const uint64_t min_interval = 1000 / static_cast<uint64_t>(opts_.encode_fps);
        if (last_pts != 0 && job.pts < last_pts + min_interval) {
            enc_throttle_drop_.fetch_add(1);
            mpp_buffer_put(job.buffer);
                if (job.rgb_fd >= 0) {
                    close(job.rgb_fd);
                }
                continue;
            }
            last_pts = job.pts;
        }
        bool recording = job.recording;
        if (!recording) {
            last_recording = false;
        } else if (!last_recording) {
            reset_headers_on_ready = true;
            force_idr_on_ready = true;
            last_recording = true;
        }
        MppBuffer encode_buffer = job.buffer;
        int pool_index = -1;
        if (opts_.shader_enabled && opts_.shader_for_encode && !opts_.shader_for_display && job.rgb_fd < 0) {
            if (ensure_gl_ready_for_encode()) {
                uint64_t start_ms = now_ms();
                auto pf = process_shader_frame(job.buffer, job.width, job.height, job.hor_stride, job.ver_stride, job.fmt, job.pts);
                last_render_ms_.store(now_ms() - start_ms);
                if (pf.has_value() && pf->bo) {
                    int fd = gbm_bo_get_fd(pf->bo);
                    if (fd >= 0) {
                        job.rgb_fd = fd;
                        job.rgb_width = gl_.target_width();
                        job.rgb_height = gl_.target_height();
                        job.rgb_stride = gbm_bo_get_stride(pf->bo);
                        job.rgb_format = RK_FORMAT_XRGB_8888;
                        gl_ok_.fetch_add(1);
                        if (!processed_logged_.exchange(true)) {
                            spdlog::info("First processed frame rendered via GL shader (encode-only)");
                        }
                    } else {
                        gl_fail_.fetch_add(1);
                    }
                } else {
                    gl_fail_.fetch_add(1);
                }
            }
        }
        if (job.rgb_fd >= 0) {
#if defined(HAVE_RGA) && HAVE_RGA
            if (!rga_rgb_to_nv12_) {
                rga_rgb_to_nv12_ = std::unique_ptr<RgaRgbToNv12, RgaRgbToNv12Deleter>(new RgaRgbToNv12());
            }
            const uint32_t enc_hor_stride = encode_hor_stride_ ? encode_hor_stride_ : job.hor_stride;
            const uint32_t enc_ver_stride = encode_ver_stride_ ? encode_ver_stride_ : job.ver_stride;
            rga_rgb_to_nv12_->set_mode(record_rgb2yuv_mode_);
            rga_rgb_to_nv12_->set_src_format(record_rgb_format_);
            rga_rgb_to_nv12_->set_use_mpp_buffer(opts_.encode_use_mpp_buffer);
            if (rga_rgb_to_nv12_ &&
                rga_rgb_to_nv12_->init(drm_fd_, job.width, job.height, enc_hor_stride, enc_ver_stride) &&
                rga_rgb_to_nv12_->process(job.rgb_fd, job.rgb_width, job.rgb_height, job.rgb_stride, encode_buffer, pool_index)) {
                job.fmt = MPP_FMT_YUV420SP;
                job.hor_stride = enc_hor_stride;
                job.ver_stride = enc_ver_stride;
                enc_rgb_ok_.fetch_add(1);
            } else if (!record_colortrans_warned_) {
                record_colortrans_warned_ = true;
                spdlog::warn("Processed shader RGB->NV12 failed; falling back to raw buffer.");
                enc_rgb_fail_.fetch_add(1);
            }
#else
            if (!record_colortrans_warned_) {
                record_colortrans_warned_ = true;
                spdlog::warn("Processed shader RGB->NV12 requires librga; falling back to raw buffer.");
                enc_rgb_fail_.fetch_add(1);
            }
#endif
        } else if (opts_.record_colortrans && record_colortrans_ready_) {
#if defined(HAVE_RGA) && HAVE_RGA
            if (rga_colortrans_ && rga_colortrans_->process(job.buffer, encode_buffer, pool_index)) {
                job.fmt = MPP_FMT_YUV420SP;
            } else if (!record_colortrans_warned_) {
                record_colortrans_warned_ = true;
                spdlog::warn("Record colortrans failed; falling back to raw buffer.");
            }
#endif
        }
        if (job.fmt == MPP_FMT_YUV420SP) {
            const size_t expected_size = static_cast<size_t>(job.hor_stride) * static_cast<size_t>(job.ver_stride) * 3 / 2;
            const size_t actual_size = mpp_buffer_get_size(encode_buffer);
            if (actual_size > 0 && expected_size > 0 && actual_size < expected_size) {
                if (!size_warned.exchange(true)) {
                    spdlog::warn("Processed encode buffer too small (size {} < expected {}), dropping frame.", actual_size, expected_size);
                }
                if (encode_buffer && encode_buffer != job.buffer) {
                    mpp_buffer_put(encode_buffer);
                }
#if defined(HAVE_RGA) && HAVE_RGA
                if (pool_index >= 0) {
                    if (job.rgb_fd >= 0 && rga_rgb_to_nv12_) {
                        rga_rgb_to_nv12_->release(pool_index);
                    } else if (rga_colortrans_) {
                        rga_colortrans_->release(pool_index);
                    }
                }
#endif
                mpp_buffer_put(job.buffer);
                if (job.rgb_fd >= 0) {
                    close(job.rgb_fd);
                }
                continue;
            }
        }
        EncConfig desired{};
        desired.width = job.width;
        desired.height = job.height;
        desired.hor_stride = job.hor_stride;
        desired.ver_stride = job.ver_stride;
        desired.fmt = job.fmt;
        desired.fps = dvr_fps_;
        desired.bitrate_kbps = opts_.encode_bitrate_kbps;
        desired.codec = codec_;
        bool config_changed = !enc_cfg.valid ||
                              desired.width != enc_cfg.width ||
                              desired.height != enc_cfg.height ||
                              desired.hor_stride != enc_cfg.hor_stride ||
                              desired.ver_stride != enc_cfg.ver_stride ||
                              desired.fmt != enc_cfg.fmt ||
                              desired.fps != enc_cfg.fps ||
                              desired.bitrate_kbps != enc_cfg.bitrate_kbps ||
                              desired.codec != enc_cfg.codec;
        if (config_changed) {
            if (encoder.ready()) {
                encoder.deinit();
            }
            enc_cfg = desired;
            enc_cfg.valid = true;
            reset_headers_on_ready = true;
            force_idr_on_ready = true;
        }
        if (!encoder.ready()) {
            if (!encoder.init(desired.width, desired.height, desired.hor_stride, desired.ver_stride,
                              desired.fmt, desired.codec, desired.fps, desired.bitrate_kbps)) {
                if (!enc_warned_.exchange(true)) {
                    spdlog::warn("MPP encoder init failed; dropping processed encode.");
                }
                if (encode_buffer && encode_buffer != job.buffer) {
                    mpp_buffer_put(encode_buffer);
                }
#if defined(HAVE_RGA) && HAVE_RGA
                if (pool_index >= 0) {
                    if (job.rgb_fd >= 0 && rga_rgb_to_nv12_) {
                        rga_rgb_to_nv12_->release(pool_index);
                    } else if (rga_colortrans_) {
                        rga_colortrans_->release(pool_index);
                    }
                }
#endif
                mpp_buffer_put(job.buffer);
                if (job.rgb_fd >= 0) {
                    close(job.rgb_fd);
                }
                continue;
            }
        }
        if (reset_headers_on_ready) {
            encoder.reset_headers_sent();
            reset_headers_on_ready = false;
        }
        if (force_idr_on_ready) {
            force_idr_on_ready = false;
        }
        if (encoder.has_headers() && !encoder.headers_sent()) {
            auto headers = ensure_annexb_config(encoder.headers(), codec_);
            if (!first_encode.load()) {
                std::string hex;
                for (size_t i = 0; i < headers.size() && i < 8; i++) {
                    char buf[4];
                    snprintf(buf, sizeof(buf), "%02X", headers[i]);
                    if (!hex.empty()) hex += " ";
                    hex += buf;
                }
                spdlog::info("Processed encode headers head [{}], annexb={}", hex, has_start_code(headers));
            }
            auto parts = split_annexb(headers);
            if (parts.empty()) {
                dvr_->frame(std::make_shared<std::vector<uint8_t>>(std::move(headers)));
            } else {
                for (auto& part : parts) {
                    dvr_->frame(std::make_shared<std::vector<uint8_t>>(std::move(part)));
                }
            }
            encoder.mark_headers_sent();
        }
        MppFrame enc_frame = nullptr;
        if (mpp_frame_init(&enc_frame) == MPP_OK) {
            mpp_frame_set_width(enc_frame, job.width);
            mpp_frame_set_height(enc_frame, job.height);
            mpp_frame_set_hor_stride(enc_frame, job.hor_stride);
            mpp_frame_set_ver_stride(enc_frame, job.ver_stride);
            mpp_frame_set_fmt(enc_frame, job.fmt);
            mpp_frame_set_pts(enc_frame, job.pts);
            mpp_frame_set_buffer(enc_frame, encode_buffer);
            std::vector<uint8_t> out;
            if (encoder.encode_frame(enc_frame, out)) {
                out = ensure_annexb(std::move(out));
                auto parts = split_annexb(out);
                if (!first_encode.load()) {
                    std::string hex;
                    for (size_t i = 0; i < out.size() && i < 8; i++) {
                        char buf[4];
                        snprintf(buf, sizeof(buf), "%02X", out[i]);
                        if (!hex.empty()) hex += " ";
                        hex += buf;
                    }
                    spdlog::info("Processed encode packet head [{}], annexb={}", hex, has_start_code(out));
                }
                if (parts.empty()) {
                    dvr_->frame(std::make_shared<std::vector<uint8_t>>(std::move(out)));
                } else {
                    for (auto& part : parts) {
                        dvr_->frame(std::make_shared<std::vector<uint8_t>>(std::move(part)));
                    }
                }
                if (!first_encode.exchange(true)) {
                    spdlog::info("Processed encode: first packet written");
                }
            }
            mpp_frame_deinit(&enc_frame);
        }
        if (encode_buffer && encode_buffer != job.buffer) {
            mpp_buffer_put(encode_buffer);
        }
        mpp_buffer_put(job.buffer);
#if defined(HAVE_RGA) && HAVE_RGA
        if (pool_index >= 0) {
            if (job.rgb_fd >= 0 && rga_rgb_to_nv12_) {
                rga_rgb_to_nv12_->release(pool_index);
            } else if (rga_colortrans_) {
                rga_colortrans_->release(pool_index);
            }
        }
#endif
        if (job.rgb_fd >= 0) {
            close(job.rgb_fd);
        }
        auto now = clock::now();
        if (now - last_log >= std::chrono::seconds(1)) {
            uint64_t drops = enc_drop_.load();
            uint64_t ok = enc_rgb_ok_.load();
            uint64_t fail = enc_rgb_fail_.load();
            uint64_t throttle = enc_throttle_drop_.load();
            spdlog::info("Encode stats: drops={}, throttle/s={}, rgb_ok/s={}, rgb_fail/s={}",
                         drops, throttle - last_throttle, ok - last_ok, fail - last_fail);
            last_log = now;
            last_drop = drops;
            last_throttle = throttle;
            last_ok = ok;
            last_fail = fail;
        }
    }
    encoder.deinit();
}

std::string ProcessingPipeline::current_shader_path() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return shader_path_cache_;
}
