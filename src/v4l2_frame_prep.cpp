#include "v4l2_frame_prep.h"

#include <errno.h>
#include <fcntl.h>
#include <string.h>
#include <sys/ioctl.h>
#include <time.h>
#include <unistd.h>

#include <drm_fourcc.h>
#include <linux/ioctl.h>
#include <linux/types.h>

#include "spdlog/spdlog.h"

// /dev/dma_heap/<name> (linux/dma-heap.h, newer than the toolchain's headers)
struct pp_dma_heap_allocation_data {
    __u64 len;
    __u32 fd;
    __u32 fd_flags;
    __u64 heap_flags;
};
#define PP_DMA_HEAP_IOCTL_ALLOC _IOWR('H', 0x0, struct pp_dma_heap_allocation_data)

// /dev/ml-dmablit (ml-kernel modules/ml_dmablit.h): copies between dma-bufs
// on the dw-axi-dmac engine.
struct pp_ml_dmablit_copy {
    __s32 src_fd;
    __u32 src_off;
    __u32 dst_off;
    __u32 len;
};
struct pp_ml_dmablit_req {
    __s32 dst_fd;
    __u32 n;
    struct pp_ml_dmablit_copy copy[8];
};
#define PP_ML_DMABLIT_SUBMIT _IOW('D', 0x01, struct pp_ml_dmablit_req)

// /dev/arscaler (ml-kernel modules/ar_scaler.h), the dma-buf ops: one plane
// cropped and resized per op.
struct pp_ar_scaler_op {
    __u32 srcphy, srcw, srch, srcstride;
    __u32 crop_x, crop_y, cropw, croph;
    __u32 dstphy, dstw, dsth, dststride;
    __u32 channels, control, interp, ctrl3c;
};
struct pp_ar_scaler_dmabuf_op {
    __s32 src_fd;
    __s32 dst_fd;
    __u32 src_off;
    __u32 dst_off;
    struct pp_ar_scaler_op op;
};
struct pp_ar_scaler_dmabuf_batch {
    __u32 count;
    __u32 reserved;
    struct pp_ar_scaler_dmabuf_op ops[8];
};
#define PP_SCALER_IOC_BATCH_DMABUF _IOWR('Z', 4, struct pp_ar_scaler_dmabuf_batch)

// The codec pool: physically contiguous, where the decoder's frames live.
static const char *kHeap = "/dev/dma_heap/mmz";

// What players assume for an HD stream that does not say (the re-encode's
// VUI does not): BT.709, limited range.
static const uint32_t kCsc = GE2D_CSC_BT709_LIMITED;

static const uint32_t k720 = 720;

static uint64_t now_us() {
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (uint64_t)ts.tv_sec * 1000000 + ts.tv_nsec / 1000;
}

static bool warn_due(uint64_t &last_ms) {
    const uint64_t now = now_us() / 1000;
    if (now - last_ms < 5000)
        return false;
    last_ms = now;
    return true;
}

static int xioctl(int fd, unsigned long req, void *arg) {
    int ret;
    do {
        ret = ioctl(fd, req, arg);
    } while (ret == -1 && errno == EINTR);
    return ret;
}

// I420 plane offsets of a frame with `stride` and `plane_height`.
static uint32_t u_offset(uint32_t stride, uint32_t plane_height) { return stride * plane_height; }
static uint32_t v_offset(uint32_t stride, uint32_t plane_height) {
    return stride * plane_height + stride / 2 * (plane_height / 2);
}

static bool is_i420(const V4l2Decoder::Frame &f) {
    return f.fourcc == DRM_FORMAT_YUV420 && f.stride % 32 == 0 && f.plane_height % 2 == 0 &&
           (size_t)f.stride * f.plane_height * 3 / 2 <= f.dmabuf_size;
}

FramePrep::~FramePrep() {
    release();
}

EncFrame FramePrep::geometry(const V4l2Decoder::Frame &f, EncResolution res) {
    EncFrame g;
    if (res == EncResolution::Res720p && f.height > k720 && is_i420(f) &&
        open_dev(scaler_fd_, scaler_missing_, "/dev/arscaler", true)) {
        g.height = k720;
        g.width = (uint32_t)(((uint64_t)f.width * k720 / f.height + 8) & ~15ull);
        g.stride = (g.width + 31) & ~31u;   // chroma rows too: 16-byte multiples
        g.plane_height = (g.height + 15) & ~15u;
        g.size = ((size_t)g.stride * g.plane_height * 3 / 2 + 4095) & ~(size_t)4095;
    } else {
        g.width = f.width;
        g.height = f.height;
        g.stride = f.stride;
        g.plane_height = f.plane_height;
        g.size = f.dmabuf_size;
    }
    return g;
}

bool FramePrep::open_dev(int &fd, bool &missing, const char *path, bool required) {
    if (fd >= 0)
        return true;
    if (missing)
        return false;
    fd = open(path, O_RDWR | O_CLOEXEC);
    if (fd < 0) {
        missing = true;
        if (required)
            spdlog::warn("DVR re-encode: no {} ({})", path, strerror(errno));
        else
            spdlog::info("DVR re-encode: no {} ({})", path, strerror(errno));
        return false;
    }
    return true;
}

void FramePrep::free_pool(Pool &pool) {
    for (int fd : pool.fds)
        if (fd >= 0)
            close(fd);
    pool.fds.clear();
    pool.size = 0;
}

bool FramePrep::ensure_pool(Pool &pool, unsigned count, size_t size) {
    if (pool.fds.size() == count && pool.size == size)
        return true;
    free_pool(pool);
    if (!open_dev(heap_fd_, heap_missing_, kHeap, true))
        return false;
    for (unsigned i = 0; i < count; i++) {
        struct pp_dma_heap_allocation_data a = {};
        a.len = size;
        a.fd_flags = O_RDWR | O_CLOEXEC;
        if (xioctl(heap_fd_, PP_DMA_HEAP_IOCTL_ALLOC, &a)) {
            spdlog::warn("DVR re-encode: {} frames of {} bytes from {}: {}", count, size, kHeap,
                         strerror(errno));
            free_pool(pool);
            return false;
        }
        pool.fds.push_back((int)a.fd);
    }
    pool.size = size;
    spdlog::info("DVR re-encode: {} frames of {} KiB", count, size / 1024);
    return true;
}

void FramePrep::release() {
    free_pool(out_);
    free_pool(staging_);
    // The engines' files keep every buffer they mapped (our frames, the
    // decoder's, the OSD's) until they are closed.
    for (int *fd : {&ge2d_fd_, &blit_fd_, &scaler_fd_, &heap_fd_}) {
        if (*fd >= 0)
            close(*fd);
        *fd = -1;
    }
    layer_ = OsdLayerSnapshot();
}

bool FramePrep::want_osd(const V4l2Decoder::Frame &f) {
    static uint64_t warn_ms;
    if (!osd_layer_snapshot(layer_) || layer_.regions.empty())
        return false;   // nothing to burn in
    if (layer_.width != f.width || layer_.height != f.height) {
        if (warn_due(warn_ms))
            spdlog::warn("DVR OSD: the OSD is {}x{}, the video {}x{}: recording without the OSD",
                         layer_.width, layer_.height, f.width, f.height);
        return false;
    }
    if (layer_.drm_format != DRM_FORMAT_ARGB4444 && layer_.drm_format != DRM_FORMAT_ARGB8888)
        return false;
    return is_i420(f) && open_dev(ge2d_fd_, ge2d_missing_, "/dev/ge2d", true);
}

// `f` copied into `dst_fd` (same layout) with the OSD blended onto it.
bool FramePrep::burn(int dst_fd, const V4l2Decoder::Frame &f, uint64_t &copy_us, uint64_t &blend_us) {
    static uint64_t warn_ms;
    ge2d_frame frame = {};
    frame.format = GE2D_FMT_I420;
    frame.width = f.width;
    frame.height = f.height;
    frame.stride[0] = f.stride;
    frame.stride[1] = frame.stride[2] = f.stride / 2;
    frame.offset[1] = u_offset(f.stride, f.plane_height);
    frame.offset[2] = v_offset(f.stride, f.plane_height);

    const uint64_t t0 = now_us();
    blits_.clear();
    bool copied = false;
    if (open_dev(blit_fd_, blit_missing_, "/dev/ml-dmablit", false)) {
        struct pp_ml_dmablit_req req = {};
        req.dst_fd = dst_fd;
        req.n = 1;
        req.copy[0].src_fd = f.dmabuf_fd;
        req.copy[0].len = (uint32_t)((size_t)f.stride * f.plane_height * 3 / 2) & ~3u;
        copied = xioctl(blit_fd_, PP_ML_DMABLIT_SUBMIT, &req) == 0;
        if (!copied && warn_due(warn_ms))
            spdlog::warn("DVR OSD: DMA copy failed ({}), the GE2D copies", strerror(errno));
    }
    if (!copied) {
        // The GE2D copies: the decoder's frame drawn opaque, first.
        ge2d_blit b = {};
        b.src = frame;
        b.src.fd = f.dmabuf_fd;
        b.width = (uint16_t)f.width;
        b.height = (uint16_t)f.height;
        b.flags = GE2D_BLIT_GLOBAL_ALPHA;
        b.alpha = 255;
        blits_.push_back(b);
    }
    const uint64_t t1 = now_us();

    ge2d_frame osd = {};
    osd.fd = layer_.prime_fd;
    osd.format = layer_.drm_format == DRM_FORMAT_ARGB4444 ? GE2D_FMT_PACKED16_1 /* ARGB4444 */
                                                          : GE2D_FMT_ARGB8888;
    osd.width = layer_.width;
    osd.height = layer_.height;
    osd.stride[0] = layer_.stride;
    for (const OsdRegion &r : layer_.regions) {
        if (blits_.size() >= GE2D_MAX_BLITS)
            break;
        ge2d_blit b = {};
        b.src = osd;
        b.src_x = b.dst_x = r.x;
        b.src_y = b.dst_y = r.y;
        b.width = r.w;
        b.height = r.h;
        blits_.push_back(b);
    }

    ge2d_job job = {};
    job.dst = frame;
    job.dst.fd = dst_fd;
    job.csc = kCsc;
    job.n_blits = (uint32_t)blits_.size();
    job.blits = (uintptr_t)blits_.data();
    if (xioctl(ge2d_fd_, GE2D_IOC_RUN, &job)) {
        if (warn_due(warn_ms))
            spdlog::warn("DVR OSD: GE2D job ({} blits) failed: {}", blits_.size(), strerror(errno));
        return false;
    }
    copy_us = t1 - t0;
    blend_us = now_us() - t1;
    return true;
}

// `src_fd` (laid out as `f`) scaled into `dst_fd` (laid out as `geom`).
bool FramePrep::scale(int src_fd, const V4l2Decoder::Frame &f, int dst_fd, const EncFrame &geom) {
    static uint64_t warn_ms;
    struct pp_ar_scaler_dmabuf_batch batch = {};
    batch.count = 3;
    for (int p = 0; p < 3; p++) {
        const uint32_t div = p ? 2 : 1;
        pp_ar_scaler_dmabuf_op &o = batch.ops[p];
        o.src_fd = src_fd;
        o.dst_fd = dst_fd;
        o.src_off = p == 0 ? 0 : p == 1 ? u_offset(f.stride, f.plane_height) : v_offset(f.stride, f.plane_height);
        o.dst_off = p == 0 ? 0 : p == 1 ? u_offset(geom.stride, geom.plane_height)
                                        : v_offset(geom.stride, geom.plane_height);
        o.op.srcw = o.op.cropw = f.width / div;
        o.op.srch = o.op.croph = f.height / div;
        o.op.srcstride = f.stride / div;
        o.op.dstw = geom.width / div;
        o.op.dsth = geom.height / div;
        o.op.dststride = geom.stride / div;
        o.op.channels = 1;
    }
    if (xioctl(scaler_fd_, PP_SCALER_IOC_BATCH_DMABUF, &batch)) {
        if (warn_due(warn_ms))
            spdlog::warn("DVR re-encode: scaling {}x{} to {}x{} failed: {}", f.width, f.height,
                         geom.width, geom.height, strerror(errno));
        return false;
    }
    return true;
}

bool FramePrep::prepare(unsigned slot, unsigned slots, const V4l2Decoder::Frame &f, const EncFrame &geom,
                        bool osd, EncFrame &out, bool &own) {
    const bool scaled = geom.width != f.width || geom.height != f.height;
    const bool osd_on = osd && want_osd(f);
    out = geom;
    out.fd = f.dmabuf_fd;
    own = false;
    if (!scaled && !osd_on)
        return true;
    if (scaled && !open_dev(scaler_fd_, scaler_missing_, "/dev/arscaler", true))
        return false;

    uint64_t copy_us = 0, blend_us = 0, scale_us = 0;
    int src_fd = f.dmabuf_fd;
    bool burnt = false;
    if (osd_on) {
        // Scaled afterwards: one frame at the decoded size to blend in.
        Pool &pool = scaled ? staging_ : out_;
        const unsigned idx = scaled ? 0 : slot;
        if (ensure_pool(pool, scaled ? 1 : slots, f.dmabuf_size) && idx < pool.fds.size() &&
            burn(pool.fds[idx], f, copy_us, blend_us)) {
            src_fd = pool.fds[idx];
            burnt = true;
        } else if (!scaled) {
            return true;   // the decoder's frame, without the OSD
        }
    }
    if (scaled) {
        if (!ensure_pool(out_, slots, geom.size) || slot >= out_.fds.size())
            return false;
        const uint64_t t = now_us();
        if (!scale(src_fd, f, out_.fds[slot], geom))
            return false;
        scale_us = now_us() - t;
    }
    out.fd = out_.fds[slot];
    out.size = out_.size;
    own = true;

    stat_frames_++;
    if (burnt) {
        stat_burnt_++;
        stat_regions_ += layer_.regions.size();
        stat_copy_us_ += copy_us;
        stat_blend_us_ += blend_us;
    }
    stat_scale_us_ += scale_us;
    const uint64_t now_ms = now_us() / 1000;
    if (!stat_start_ms_)
        stat_start_ms_ = now_ms;
    if (now_ms - stat_start_ms_ >= 5000) {
        const uint64_t b = stat_burnt_ ? stat_burnt_ : 1;
        spdlog::debug("DVR re-encode: {} frames ({}x{}), {} with the OSD ({} regions, copy {} us, "
                      "blend {} us), scale {} us (averages)",
                      stat_frames_, geom.width, geom.height, stat_burnt_, stat_regions_ / b,
                      stat_copy_us_ / b, stat_blend_us_ / b, stat_scale_us_ / stat_frames_);
        stat_start_ms_ = now_ms;
        stat_frames_ = stat_burnt_ = stat_regions_ = 0;
        stat_copy_us_ = stat_blend_us_ = stat_scale_us_ = 0;
    }
    return true;
}
