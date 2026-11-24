#include "v4l2_frame_prep.h"

#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <algorithm>
#include <chrono>
#include <pthread.h>
#include <sys/ioctl.h>
#include <sys/mman.h>
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
#define PP_SCALER_IOC_SINGLE_DMABUF _IOWR('Z', 3, struct pp_ar_scaler_dmabuf_op)
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


// --- OsdUpscaler ------------------------------------------------------------

bool OsdUpscaler::alloc(Buf &b, size_t len) {
    for (const char *heap : {kHeap}) {
        const int h = open(heap, O_RDWR | O_CLOEXEC);
        if (h < 0)
            continue;
        struct pp_dma_heap_allocation_data a = {};
        a.len = len;
        a.fd_flags = O_RDWR | O_CLOEXEC;
        const int r = xioctl(h, PP_DMA_HEAP_IOCTL_ALLOC, &a);
        close(h);
        if (r)
            continue;
        void *m = mmap(nullptr, len, PROT_READ | PROT_WRITE, MAP_SHARED, (int)a.fd, 0);
        if (m == MAP_FAILED) {
            close((int)a.fd);
            continue;
        }
        b.fd = (int)a.fd;
        b.map = (uint8_t *)m;
        b.len = len;
        return true;
    }
    return false;
}

void OsdUpscaler::free_buf(Buf &b) {
    if (b.map)
        munmap(b.map, b.len);
    if (b.fd >= 0)
        close(b.fd);
    b = Buf();
}

bool OsdUpscaler::run(uint32_t src_w, uint32_t src_h, uint32_t dst_w, uint32_t dst_h, uint32_t layer_w,
                      uint32_t layer_h, bool copy) {
    if (running() && src_w == src_w_ && src_h == src_h_ && dst_w == dst_w_ && dst_h == dst_h_ &&
        layer_w == layer_w_ && layer_h == layer_h_ && copy == copy_wanted_)
        return true;
    stop();
    // A failed start is not retried every frame.
    static uint64_t failed_ms;
    const uint64_t now_ms = now_us() / 1000;
    if (failed_ms && now_ms - failed_ms < 5000)
        return false;
    if (!src_w || !src_h || !dst_w || !dst_h || src_w > layer_w || src_h > layer_h)
        return false;

    scaler_fd_ = open("/dev/arscaler", O_RDWR | O_CLOEXEC);
    out_stride_ = (dst_w * 4 + 15) & ~15u;
    const size_t out_len = ((size_t)out_stride_ * dst_h + 4095) & ~(size_t)4095;
    copy_stride_ = (src_w * 4 + 15) & ~15u;
    const size_t copy_len = ((size_t)copy_stride_ * src_h + 4095) & ~(size_t)4095;
    if (scaler_fd_ < 0 || !alloc(out_, out_len) || (copy && !alloc(copy_, copy_len))) {
        spdlog::warn("DVR OSD: cannot scale the OSD to {}x{}: {}", dst_w, dst_h,
                     scaler_fd_ < 0 ? "no /dev/arscaler" : "no memory for its buffers");
        failed_ms = now_ms;
        stop();
        return false;
    }
    failed_ms = 0;
    src_w_ = src_w;
    src_h_ = src_h;
    dst_w_ = dst_w;
    dst_h_ = dst_h;
    layer_w_ = layer_w;
    layer_h_ = layer_h;
    copy_wanted_ = copy;
    {
        std::lock_guard<std::mutex> lk(mtx_);
        stop_ = false;
        have_ = false;
        regions_.clear();
    }
    thread_ = std::thread(&OsdUpscaler::loop, this);
    spdlog::info("DVR OSD: the OSD scaled from {}x{} to {}x{} for the recording ({:.1f} MB of buffers)", src_w,
                 src_h, dst_w, dst_h, ((double)out_len + (copy ? copy_len : 0)) / (1 << 20));
    return true;
}

void OsdUpscaler::stop() {
    {
        std::lock_guard<std::mutex> lk(mtx_);
        stop_ = true;
    }
    cv_.notify_all();
    if (thread_.joinable())
        thread_.join();
    if (copy_.fd >= 0)
        osd_layer_set_argb8888_copy(-1, nullptr, 0, 0, 0);
    free_buf(copy_);
    free_buf(out_);
    if (scaler_fd_ >= 0)
        close(scaler_fd_);
    scaler_fd_ = -1;
    have_ = false;
    regions_.clear();
    src_w_ = src_h_ = dst_w_ = dst_h_ = layer_w_ = layer_h_ = 0;
}

bool OsdUpscaler::latest(int &fd, uint32_t &stride, std::vector<OsdRegion> &regions) {
    std::lock_guard<std::mutex> lk(mtx_);
    if (!have_)
        return false;
    fd = out_.fd;
    stride = out_stride_;
    regions = regions_;
    return true;
}

bool OsdUpscaler::scale_once(const OsdLayerSnapshot &s) {
    static uint64_t warn_ms;
    struct pp_ar_scaler_dmabuf_op o = {};
    o.src_fd = s.argb8888_fd;
    o.dst_fd = out_.fd;
    // Our copy holds just the part that counts; an ARGB8888 front buffer is
    // the whole OSD, cropped here.
    o.op.srcw = copy_wanted_ ? src_w_ : s.width;
    o.op.srch = copy_wanted_ ? src_h_ : s.height;
    o.op.srcstride = s.argb8888_stride;
    o.op.cropw = src_w_;
    o.op.croph = src_h_;
    o.op.dstw = dst_w_;
    o.op.dsth = dst_h_;
    o.op.dststride = out_stride_;
    o.op.channels = 4;   // ARGB8888, each byte on its own
    if (xioctl(scaler_fd_, PP_SCALER_IOC_SINGLE_DMABUF, &o)) {
        if (warn_due(warn_ms))
            spdlog::warn("DVR OSD: scaling the OSD {}x{} to {}x{} failed: {}", src_w_, src_h_, dst_w_, dst_h_,
                         strerror(errno));
        return false;
    }
    return true;
}

void OsdUpscaler::loop() {
    pthread_setname_np(pthread_self(), "osd-upscale");
    // Here, not on the encoder's thread: registering fills the copy (3.7 MB
    // for a 720p OSD, into uncached memory).
    if (copy_wanted_)
        osd_layer_set_argb8888_copy(copy_.fd, copy_.map, copy_stride_, src_w_, src_h_);
    uint64_t done_serial = 0, last_ms = 0, first_us = 0;
    uint64_t stat_ms = now_us() / 1000, stat_n = 0, stat_us = 0;
    bool have = false;
    for (;;) {
        // For the log (support packages): how often and how long, every 30 s.
        if (now_us() / 1000 - stat_ms >= 30000) {
            if (stat_n)
                spdlog::info("DVR OSD: scaled {} times in the last 30 s, {:.1f} ms each", stat_n,
                             stat_us / 1000.0 / stat_n);
            stat_ms = now_us() / 1000;
            stat_n = stat_us = 0;
        }
        {
            std::unique_lock<std::mutex> lk(mtx_);
            cv_.wait_for(lk, std::chrono::milliseconds(20), [this] { return stop_; });
            if (stop_)
                return;
        }
        OsdLayerSnapshot s;
        const uint64_t now_ms = now_us() / 1000;
        if (!osd_layer_snapshot(s) || s.argb8888_fd < 0 || s.width != layer_w_ || s.height != layer_h_ ||
            (have && s.serial == done_serial) || now_ms - last_ms < kIntervalMs)
            continue;
        last_ms = now_ms;
        done_serial = s.serial;
        const uint64_t t0 = now_us();
        if (!scale_once(s))
            continue;
        // The regions at the new size. Both edges are rounded the same way,
        // so regions that touch still touch and none overlap (an overlap
        // would blend its semi-transparent pixels twice).
        std::vector<OsdRegion> regions;
        regions.reserve(s.regions.size());
        auto map = [](uint32_t v, uint32_t from, uint32_t to) {
            return (uint32_t)(((uint64_t)v * to + from / 2) / from);
        };
        for (const OsdRegion &r : s.regions) {
            if (r.x >= src_w_ || r.y >= src_h_)
                continue;
            const uint32_t x0 = map(r.x, src_w_, dst_w_), y0 = map(r.y, src_h_, dst_h_);
            const uint32_t x1 = map(std::min<uint32_t>(r.x + r.w, src_w_), src_w_, dst_w_);
            const uint32_t y1 = map(std::min<uint32_t>(r.y + r.h, src_h_), src_h_, dst_h_);
            if (x1 > x0 && y1 > y0)
                regions.push_back({(uint16_t)x0, (uint16_t)y0, (uint16_t)(x1 - x0), (uint16_t)(y1 - y0)});
        }
        {
            std::lock_guard<std::mutex> lk(mtx_);
            have_ = true;
            regions_.swap(regions);
        }
        stat_n++;
        stat_us += now_us() - t0;
        if (!have) {
            first_us = now_us() - t0;
            spdlog::info("DVR OSD: first OSD scaled in {:.1f} ms", first_us / 1000.0);
        }
        have = true;
    }
}

FramePrep::FramePrep() {
    const char *s = getenv("PIXELPILOT_TEST_OSD_SIZE");
    unsigned w = 0, h = 0;
    if (s && sscanf(s, "%ux%u", &w, &h) == 2 && w && h) {
        test_osd_w_ = w;
        test_osd_h_ = h;
        spdlog::warn("DVR OSD: TEST: the OSD counts as {}x{} (its top-left part)", w, h);
    }
}

FramePrep::~FramePrep() {
    release();
}

bool FramePrep::snapshot_osd() {
    if (!osd_layer_snapshot(layer_))
        return false;
    osd_w_ = test_osd_w_ ? std::min(test_osd_w_, layer_.width) : layer_.width;
    osd_h_ = test_osd_h_ ? std::min(test_osd_h_, layer_.height) : layer_.height;
    return true;
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
    if (g.width != geom_w_ || g.height != geom_h_) {
        geom_w_ = g.width;
        geom_h_ = g.height;
        spdlog::info("DVR re-encode: {}x{} (video {}x{})", g.width, g.height, f.width, f.height);
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
    upscaler_.stop();
    up_fd_ = -1;
    up_regions_.clear();
    osd_src_ = OsdSrc();
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

FramePrep::OsdAt FramePrep::want_osd(const V4l2Decoder::Frame &f, const EncFrame &geom, unsigned slots) {
    static uint64_t warn_ms;
    if (!snapshot_osd() || layer_.regions.empty())
        return OsdAt::None;   // nothing to burn in
    if (layer_.drm_format != DRM_FORMAT_ARGB4444 && layer_.drm_format != DRM_FORMAT_ARGB8888)
        return OsdAt::None;
    if (!is_i420(f) || !open_dev(ge2d_fd_, ge2d_missing_, "/dev/ge2d", true))
        return OsdAt::None;
    const bool matches_video = osd_w_ == f.width && osd_h_ == f.height;
    const bool matches_rec = osd_w_ == geom.width && osd_h_ == geom.height;
    if (matches_video || matches_rec) {
        // The screen's OSD 1:1: at the decoded size (then scaled with the
        // frame, if at all), or onto the scaled frame.
        upscaler_.stop();
        osd_src_ = OsdSrc();
        osd_src_.fd = layer_.prime_fd;
        osd_src_.format = layer_.drm_format == DRM_FORMAT_ARGB4444 ? GE2D_FMT_PACKED16_1 /* ARGB4444 */
                                                                   : GE2D_FMT_ARGB8888;
        osd_src_.width = layer_.width;
        osd_src_.height = layer_.height;
        osd_src_.stride = layer_.stride;
        osd_src_.clip_w = osd_w_;
        osd_src_.clip_h = osd_h_;
        osd_src_.regions = &layer_.regions;
        return matches_video ? OsdAt::Decoded : OsdAt::Output;
    }
    // Neither: the OSD scaled to the recording's size, blended 1:1 there.
    // The frames the encoder reads come first: the codec pool is short while
    // recording, and without them there is no recording at all, while
    // without the scaler's buffers there is only no OSD.
    const bool at_decoded = geom.width == f.width && geom.height == f.height;
    if (!upscaler_.running() &&
        !ensure_pool(out_, slots, at_decoded ? f.dmabuf_size : geom.size))
        return OsdAt::None;
    if (!upscaler_.run(osd_w_, osd_h_, geom.width, geom.height, layer_.width, layer_.height,
                       layer_.drm_format == DRM_FORMAT_ARGB4444)) {
        if (warn_due(warn_ms))
            spdlog::warn("DVR OSD: the OSD (the screen) is {}x{}, the recording {}x{}, and it cannot be "
                         "scaled: recording without the OSD",
                         osd_w_, osd_h_, geom.width, geom.height);
        return OsdAt::None;
    }
    if (!upscaler_.latest(up_fd_, up_stride_, up_regions_))
        return OsdAt::None;   // its first one is still coming
    osd_src_ = OsdSrc();
    osd_src_.fd = up_fd_;
    osd_src_.format = GE2D_FMT_ARGB8888;
    osd_src_.width = osd_src_.clip_w = geom.width;
    osd_src_.height = osd_src_.clip_h = geom.height;
    osd_src_.stride = up_stride_;
    osd_src_.regions = &up_regions_;
    return at_decoded ? OsdAt::Decoded : OsdAt::Output;
}

// An I420 frame of ours or the decoder's as the GE2D reads or writes it.
static ge2d_frame i420_frame(int fd, uint32_t width, uint32_t height, uint32_t stride, uint32_t plane_height) {
    ge2d_frame frame = {};
    frame.fd = fd;
    frame.format = GE2D_FMT_I420;
    frame.width = width;
    frame.height = height;
    frame.stride[0] = stride;
    frame.stride[1] = frame.stride[2] = stride / 2;
    frame.offset[1] = u_offset(stride, plane_height);
    frame.offset[2] = v_offset(stride, plane_height);
    return frame;
}

// The OSD's visible parts (within its clip size), each blended 1:1 onto the
// same place.
void FramePrep::add_osd_blits(const OsdSrc &src) {
    if (src.fd < 0 || !src.regions)
        return;
    ge2d_frame osd = {};
    osd.fd = src.fd;
    osd.format = src.format;
    osd.width = src.width;
    osd.height = src.height;
    osd.stride[0] = src.stride;
    for (const OsdRegion &r : *src.regions) {
        if (blits_.size() >= GE2D_MAX_BLITS)
            break;
        if (r.x >= src.clip_w || r.y >= src.clip_h)
            continue;
        ge2d_blit b = {};
        b.src = osd;
        b.src_x = b.dst_x = r.x;
        b.src_y = b.dst_y = r.y;
        b.width = (uint16_t)std::min<uint32_t>(r.w, src.clip_w - r.x);
        b.height = (uint16_t)std::min<uint32_t>(r.h, src.clip_h - r.y);
        blits_.push_back(b);
    }
}

// blits_ onto `dst`.
bool FramePrep::run_ge2d(const ge2d_frame &dst) {
    static uint64_t warn_ms;
    if (blits_.empty())
        return true;
    ge2d_job job = {};
    job.dst = dst;
    job.csc = kCsc;
    job.n_blits = (uint32_t)blits_.size();
    job.blits = (uintptr_t)blits_.data();
    if (xioctl(ge2d_fd_, GE2D_IOC_RUN, &job)) {
        if (warn_due(warn_ms))
            spdlog::warn("DVR OSD: GE2D job ({} blits) failed: {}", blits_.size(), strerror(errno));
        return false;
    }
    return true;
}

// `f` copied into `dst_fd` (same layout) with the OSD blended onto it.
bool FramePrep::burn(int dst_fd, const V4l2Decoder::Frame &f, uint64_t &copy_us, uint64_t &blend_us) {
    static uint64_t warn_ms;
    const ge2d_frame frame = i420_frame(dst_fd, f.width, f.height, f.stride, f.plane_height);

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

    add_osd_blits(osd_src_);
    if (!run_ge2d(frame))
        return false;
    copy_us = t1 - t0;
    blend_us = now_us() - t1;
    return true;
}

// The OSD blended onto our own frame `dst_fd` (laid out as `geom`), in place.
bool FramePrep::blend(int dst_fd, const EncFrame &geom, uint64_t &blend_us) {
    const uint64_t t0 = now_us();
    blits_.clear();
    add_osd_blits(osd_src_);
    if (!run_ge2d(i420_frame(dst_fd, geom.width, geom.height, geom.stride, geom.plane_height)))
        return false;
    blend_us = now_us() - t0;
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
    const OsdAt osd_at = osd ? want_osd(f, geom, slots) : OsdAt::None;
    out = geom;
    out.fd = f.dmabuf_fd;
    own = false;
    if (!scaled && osd_at == OsdAt::None)
        return true;
    if (scaled && !open_dev(scaler_fd_, scaler_missing_, "/dev/arscaler", true))
        return false;

    uint64_t copy_us = 0, blend_us = 0, scale_us = 0;
    int src_fd = f.dmabuf_fd;
    bool burnt = false;
    if (osd_at == OsdAt::Decoded) {
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
        // The OSD at the recording's size: onto the scaled frame, 1:1. A
        // failed blend leaves the frame without it.
        if (osd_at == OsdAt::Output)
            burnt = blend(out_.fds[slot], geom, blend_us);
    }
    out.fd = out_.fds[slot];
    out.size = out_.size;
    own = true;

    stat_frames_++;
    if (burnt) {
        stat_burnt_++;
        stat_regions_ += osd_src_.regions ? osd_src_.regions->size() : 0;
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
