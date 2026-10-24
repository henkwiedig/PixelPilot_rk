#include "v4l2_decoder.h"

#include <errno.h>
#include <fcntl.h>
#include <poll.h>
#include <string.h>
#include <sys/ioctl.h>
#include <sys/mman.h>
#include <time.h>
#include <unistd.h>

#include <linux/videodev2.h>
#include <drm_fourcc.h>
#include <xf86drm.h>
#include <xf86drmMode.h>

#include "spdlog/spdlog.h"

// Bitstream buffers: an access unit bigger than one is split over several
// (the decoder copies them into its own ring buffer, so a split is free).
static const unsigned kOutputBuffers = 4;
static const uint32_t kOutputBufferSize = 1024 * 1024;
// CAPTURE buffers on top of what the decoder needs for itself: two the
// display may read (see v4l2_decoder.h), two waiting for their flip
// (--vsync-lock queues frames) and one the DVR re-encoder holds.
static const unsigned kExtraCaptureBuffers = 5;

static int xioctl(int fd, unsigned long req, void *arg) {
    int ret;
    do {
        ret = ioctl(fd, req, arg);
    } while (ret == -1 && errno == EINTR);
    return ret;
}

static uint64_t now_ms() {
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (uint64_t)ts.tv_sec * 1000 + ts.tv_nsec / 1000000;
}

static uint32_t v4l2_codec_fourcc(VideoCodec codec) {
    return codec == VideoCodec::H264 ? V4L2_PIX_FMT_H264 : V4L2_PIX_FMT_HEVC;
}

V4l2Decoder::V4l2Decoder(int drm_fd) : drm_fd_(drm_fd) {}

V4l2Decoder::~V4l2Decoder() {
    close();
}

// A decoder takes the compressed format on its OUTPUT queue (an encoder has
// it on CAPTURE).
static bool is_decoder_for(int fd, uint32_t fourcc) {
    struct v4l2_capability cap = {};
    if (xioctl(fd, VIDIOC_QUERYCAP, &cap))
        return false;
    const uint32_t caps = (cap.capabilities & V4L2_CAP_DEVICE_CAPS) ? cap.device_caps : cap.capabilities;
    if (!(caps & V4L2_CAP_VIDEO_M2M_MPLANE) || !(caps & V4L2_CAP_STREAMING))
        return false;
    for (uint32_t i = 0;; i++) {
        struct v4l2_fmtdesc fd_desc = {};
        fd_desc.index = i;
        fd_desc.type = V4L2_BUF_TYPE_VIDEO_OUTPUT_MPLANE;
        if (xioctl(fd, VIDIOC_ENUM_FMT, &fd_desc))
            return false;
        if (fd_desc.pixelformat == fourcc)
            return true;
    }
}

bool V4l2Decoder::find_device(VideoCodec codec, const char *device) {
    const uint32_t fourcc = v4l2_codec_fourcc(codec);
    if (device) {
        fd_ = ::open(device, O_RDWR | O_NONBLOCK | O_CLOEXEC);
        if (fd_ < 0) {
            spdlog::error("V4L2 decoder: cannot open {}: {}", device, strerror(errno));
            return false;
        }
        if (!is_decoder_for(fd_, fourcc)) {
            spdlog::error("V4L2 decoder: {} does not decode {}", device,
                          codec == VideoCodec::H264 ? "H.264" : "H.265");
            ::close(fd_);
            fd_ = -1;
            return false;
        }
        spdlog::info("V4L2 decoder: {}", device);
        return true;
    }
    for (int i = 0; i < 16; i++) {
        char path[32];
        snprintf(path, sizeof(path), "/dev/video%d", i);
        int fd = ::open(path, O_RDWR | O_NONBLOCK | O_CLOEXEC);
        if (fd < 0)
            continue;
        if (is_decoder_for(fd, fourcc)) {
            struct v4l2_capability cap = {};
            xioctl(fd, VIDIOC_QUERYCAP, &cap);
            spdlog::info("V4L2 decoder: {} ({})", path, (const char *)cap.card);
            fd_ = fd;
            return true;
        }
        ::close(fd);
    }
    spdlog::error("V4L2 decoder: no device decodes {}", codec == VideoCodec::H264 ? "H.264" : "H.265");
    return false;
}

bool V4l2Decoder::open(VideoCodec codec, const char *device) {
    close();
    if (codec != VideoCodec::H264 && codec != VideoCodec::H265)
        codec = VideoCodec::H265;
    if (!find_device(codec, device))
        return false;
    out_type_ = V4L2_BUF_TYPE_VIDEO_OUTPUT_MPLANE;
    cap_type_ = V4L2_BUF_TYPE_VIDEO_CAPTURE_MPLANE;

    struct v4l2_event_subscription sub = {};
    sub.type = V4L2_EVENT_SOURCE_CHANGE;
    if (xioctl(fd_, VIDIOC_SUBSCRIBE_EVENT, &sub))
        spdlog::warn("V4L2 decoder: no source change events: {}", strerror(errno));
    sub = {};
    sub.type = V4L2_EVENT_EOS;
    xioctl(fd_, VIDIOC_SUBSCRIBE_EVENT, &sub);

    if (!setup_output(codec)) {
        close();
        return false;
    }
    codec_ = codec;
    draining_ = false;
    reconfig_pending_ = false;
    return true;
}

bool V4l2Decoder::setup_output(VideoCodec codec) {
    struct v4l2_format fmt = {};
    fmt.type = out_type_;
    fmt.fmt.pix_mp.pixelformat = v4l2_codec_fourcc(codec);
    fmt.fmt.pix_mp.width = 1920;   // a hint only: the stream's own size comes with the source change
    fmt.fmt.pix_mp.height = 1080;
    fmt.fmt.pix_mp.num_planes = 1;
    fmt.fmt.pix_mp.plane_fmt[0].sizeimage = kOutputBufferSize;
    if (xioctl(fd_, VIDIOC_S_FMT, &fmt)) {
        spdlog::error("V4L2 decoder: S_FMT (bitstream): {}", strerror(errno));
        return false;
    }

    struct v4l2_requestbuffers req = {};
    req.count = kOutputBuffers;
    req.type = out_type_;
    req.memory = V4L2_MEMORY_MMAP;
    if (xioctl(fd_, VIDIOC_REQBUFS, &req) || req.count == 0) {
        spdlog::error("V4L2 decoder: REQBUFS (bitstream): {}", strerror(errno));
        return false;
    }
    std::lock_guard<std::mutex> lk(out_mtx_);
    outputs_.assign(req.count, Output());
    for (uint32_t i = 0; i < req.count; i++) {
        struct v4l2_plane planes[VIDEO_MAX_PLANES] = {};
        struct v4l2_buffer buf = {};
        buf.index = i;
        buf.type = out_type_;
        buf.memory = V4L2_MEMORY_MMAP;
        buf.m.planes = planes;
        buf.length = VIDEO_MAX_PLANES;
        if (xioctl(fd_, VIDIOC_QUERYBUF, &buf)) {
            spdlog::error("V4L2 decoder: QUERYBUF (bitstream): {}", strerror(errno));
            return false;
        }
        void *map = mmap(NULL, planes[0].length, PROT_READ | PROT_WRITE, MAP_SHARED, fd_,
                         planes[0].m.mem_offset);
        if (map == MAP_FAILED) {
            spdlog::error("V4L2 decoder: mmap (bitstream): {}", strerror(errno));
            return false;
        }
        outputs_[i].map = map;
        outputs_[i].size = planes[0].length;
    }

    int type = out_type_;
    if (xioctl(fd_, VIDIOC_STREAMON, &type)) {
        spdlog::error("V4L2 decoder: STREAMON (bitstream): {}", strerror(errno));
        return false;
    }
    spdlog::info("V4L2 decoder: {} bitstream buffers of {} KiB", outputs_.size(), outputs_[0].size / 1024);
    return true;
}

void V4l2Decoder::close() {
    if (fd_ < 0)
        return;
    int type = cap_type_;
    if (cap_streaming_)
        xioctl(fd_, VIDIOC_STREAMOFF, &type);
    type = out_type_;
    xioctl(fd_, VIDIOC_STREAMOFF, &type);
    {
        std::lock_guard<std::mutex> lk(mtx_);
        cap_streaming_ = false;
    }
    free_capture();
    {
        std::lock_guard<std::mutex> lk(out_mtx_);
        for (Output &o : outputs_)
            if (o.map)
                munmap(o.map, o.size);
        outputs_.clear();
    }
    struct v4l2_requestbuffers req = {};
    req.type = out_type_;
    req.memory = V4L2_MEMORY_MMAP;
    xioctl(fd_, VIDIOC_REQBUFS, &req);
    ::close(fd_);
    fd_ = -1;
    codec_ = VideoCodec::UNKNOWN;
    format_ = Frame();
}

// A bitstream buffer the decoder is done with: one never queued, or one it
// handed back.
bool V4l2Decoder::get_output_buffer(int *index, int timeout_ms) {
    {
        std::lock_guard<std::mutex> lk(out_mtx_);
        for (size_t i = 0; i < outputs_.size(); i++) {
            if (!outputs_[i].queued) {
                *index = (int)i;
                return true;
            }
        }
    }
    const uint64_t deadline = now_ms() + timeout_ms;
    for (;;) {
        struct v4l2_plane planes[VIDEO_MAX_PLANES] = {};
        struct v4l2_buffer buf = {};
        buf.type = out_type_;
        buf.memory = V4L2_MEMORY_MMAP;
        buf.m.planes = planes;
        buf.length = VIDEO_MAX_PLANES;
        if (xioctl(fd_, VIDIOC_DQBUF, &buf) == 0) {
            std::lock_guard<std::mutex> lk(out_mtx_);
            outputs_[buf.index].queued = false;
            *index = (int)buf.index;
            return true;
        }
        if (errno != EAGAIN) {
            spdlog::warn("V4L2 decoder: DQBUF (bitstream): {}", strerror(errno));
            return false;
        }
        const uint64_t now = now_ms();
        if (now >= deadline)
            return false;
        struct pollfd pfd = {fd_, POLLOUT, 0};
        if (poll(&pfd, 1, (int)(deadline - now)) > 0 && (pfd.revents & POLLERR) && !(pfd.revents & POLLOUT))
            usleep(1000);
    }
}

bool V4l2Decoder::feed(const uint8_t *data, size_t size, uint64_t pts_us, int timeout_ms) {
    if (fd_ < 0)
        return false;
    while (size) {
        int index;
        if (!get_output_buffer(&index, timeout_ms))
            return false;
        Output &o = outputs_[index];
        const size_t n = size < o.size ? size : o.size;
        memcpy(o.map, data, n);

        struct v4l2_plane planes[VIDEO_MAX_PLANES] = {};
        struct v4l2_buffer buf = {};
        buf.index = index;
        buf.type = out_type_;
        buf.memory = V4L2_MEMORY_MMAP;
        buf.m.planes = planes;
        buf.length = 1;
        planes[0].bytesused = n;
        buf.timestamp.tv_sec = pts_us / 1000000;
        buf.timestamp.tv_usec = pts_us % 1000000;
        {
            std::lock_guard<std::mutex> lk(out_mtx_);
            if (xioctl(fd_, VIDIOC_QBUF, &buf)) {
                spdlog::warn("V4L2 decoder: QBUF (bitstream): {}", strerror(errno));
                return false;
            }
            o.queued = true;
        }
        data += n;
        size -= n;
    }
    return true;
}

void V4l2Decoder::send_eos() {
    if (fd_ < 0)
        return;
    struct v4l2_decoder_cmd cmd = {};
    cmd.cmd = V4L2_DEC_CMD_STOP;
    if (xioctl(fd_, VIDIOC_DECODER_CMD, &cmd)) {
        spdlog::warn("V4L2 decoder: DEC_CMD_STOP: {}", strerror(errno));
        return;
    }
    draining_ = true;
}

void V4l2Decoder::free_capture() {
    std::lock_guard<std::mutex> lk(mtx_);
    for (Capture &c : captures_) {
        if (c.refs)
            spdlog::warn("V4L2 decoder: freeing a frame that still has {} reader(s)", c.refs);
        if (c.fb_id)
            drmModeRmFB(drm_fd_, c.fb_id);
        if (c.gem_handle) {
            struct drm_gem_close gc = {};
            gc.handle = c.gem_handle;
            drmIoctl(drm_fd_, DRM_IOCTL_GEM_CLOSE, &gc);
        }
        if (c.dmabuf_fd >= 0)
            ::close(c.dmabuf_fd);
    }
    captures_.clear();
    shown_last_ = shown_prev_ = -1;
    if (fd_ >= 0) {
        struct v4l2_requestbuffers req = {};
        req.type = cap_type_;
        req.memory = V4L2_MEMORY_MMAP;
        if (xioctl(fd_, VIDIOC_REQBUFS, &req))
            spdlog::warn("V4L2 decoder: freeing the frame buffers: {}", strerror(errno));
    }
}

bool V4l2Decoder::reconfigure() {
    if (fd_ < 0)
        return false;
    if (cap_streaming_) {
        int type = cap_type_;
        xioctl(fd_, VIDIOC_STREAMOFF, &type);
        std::lock_guard<std::mutex> lk(mtx_);
        cap_streaming_ = false;
    }
    free_capture();
    reconfig_pending_ = false;
    return setup_capture();
}

bool V4l2Decoder::setup_capture() {
    struct v4l2_format fmt = {};
    fmt.type = cap_type_;
    if (xioctl(fd_, VIDIOC_G_FMT, &fmt)) {
        spdlog::error("V4L2 decoder: G_FMT (frames): {}", strerror(errno));
        return false;
    }
    // I420 in one buffer: the format both the decoder and the display take
    // with the fewest assumptions (planes at fixed offsets).
    if (fmt.fmt.pix_mp.pixelformat != V4L2_PIX_FMT_YUV420) {
        fmt.fmt.pix_mp.pixelformat = V4L2_PIX_FMT_YUV420;
        fmt.fmt.pix_mp.num_planes = 1;
        if (xioctl(fd_, VIDIOC_S_FMT, &fmt))
            spdlog::warn("V4L2 decoder: S_FMT I420: {}", strerror(errno));
        xioctl(fd_, VIDIOC_G_FMT, &fmt);
    }
    const struct v4l2_pix_format_mplane &mp = fmt.fmt.pix_mp;
    uint32_t drm_fourcc;
    switch (mp.pixelformat) {
    case V4L2_PIX_FMT_YUV420: drm_fourcc = DRM_FORMAT_YUV420; break;
    case V4L2_PIX_FMT_NV12:   drm_fourcc = DRM_FORMAT_NV12; break;
    default:
        spdlog::error("V4L2 decoder: unsupported frame format {:.4}", (const char *)&mp.pixelformat);
        return false;
    }
    if (mp.num_planes != 1) {
        spdlog::error("V4L2 decoder: {} memory planes, only one is supported", mp.num_planes);
        return false;
    }
    const uint32_t stride = mp.plane_fmt[0].bytesperline;
    const uint32_t plane_height = mp.height;

    // The visible picture: the decoded one is padded to the codec's block size.
    uint32_t width = mp.width, height = mp.height;
    struct v4l2_selection sel = {};
    sel.type = V4L2_BUF_TYPE_VIDEO_CAPTURE;
    sel.target = V4L2_SEL_TGT_COMPOSE;
    if (xioctl(fd_, VIDIOC_G_SELECTION, &sel) == 0 && sel.r.width && sel.r.height) {
        width = sel.r.width;
        height = sel.r.height;
    }

    struct v4l2_control ctrl = {};
    ctrl.id = V4L2_CID_MIN_BUFFERS_FOR_CAPTURE;
    unsigned min_buffers = 4;
    if (xioctl(fd_, VIDIOC_G_CTRL, &ctrl) == 0 && ctrl.value > 0)
        min_buffers = ctrl.value;

    struct v4l2_requestbuffers req = {};
    req.count = min_buffers + kExtraCaptureBuffers;
    req.type = cap_type_;
    req.memory = V4L2_MEMORY_MMAP;
    if (xioctl(fd_, VIDIOC_REQBUFS, &req) || req.count == 0) {
        spdlog::error("V4L2 decoder: REQBUFS (frames, {}): {}", min_buffers + kExtraCaptureBuffers, strerror(errno));
        return false;
    }

    std::lock_guard<std::mutex> lk(mtx_);
    captures_.assign(req.count, Capture());
    for (uint32_t i = 0; i < req.count; i++) {
        Capture &c = captures_[i];
        struct v4l2_plane planes[VIDEO_MAX_PLANES] = {};
        struct v4l2_buffer buf = {};
        buf.index = i;
        buf.type = cap_type_;
        buf.memory = V4L2_MEMORY_MMAP;
        buf.m.planes = planes;
        buf.length = VIDEO_MAX_PLANES;
        if (xioctl(fd_, VIDIOC_QUERYBUF, &buf)) {
            spdlog::error("V4L2 decoder: QUERYBUF (frames): {}", strerror(errno));
            return false;
        }
        c.size = planes[0].length;

        struct v4l2_exportbuffer exp = {};
        exp.type = cap_type_;
        exp.index = i;
        exp.plane = 0;
        exp.flags = O_RDWR | O_CLOEXEC;
        if (xioctl(fd_, VIDIOC_EXPBUF, &exp)) {
            spdlog::error("V4L2 decoder: EXPBUF: {}", strerror(errno));
            return false;
        }
        c.dmabuf_fd = exp.fd;
        if (drmPrimeFDToHandle(drm_fd_, c.dmabuf_fd, &c.gem_handle)) {
            spdlog::error("V4L2 decoder: the display cannot import a frame buffer: {}", strerror(errno));
            return false;
        }
        uint32_t handles[4] = {c.gem_handle, c.gem_handle, 0, 0};
        uint32_t pitches[4] = {stride, stride, 0, 0};
        uint32_t offsets[4] = {0, stride * plane_height, 0, 0};
        if (drm_fourcc == DRM_FORMAT_YUV420) {
            handles[2] = c.gem_handle;
            pitches[1] = pitches[2] = stride / 2;
            offsets[2] = offsets[1] + (stride / 2) * (plane_height / 2);
        }
        if (drmModeAddFB2(drm_fd_, width, height, drm_fourcc, handles, pitches, offsets, &c.fb_id, 0)) {
            spdlog::error("V4L2 decoder: AddFB2 {}x{} stride {}: {}", width, height, stride, strerror(errno));
            return false;
        }
        if (!queue_capture((int)i))
            return false;
    }

    int type = cap_type_;
    if (xioctl(fd_, VIDIOC_STREAMON, &type)) {
        spdlog::error("V4L2 decoder: STREAMON (frames): {}", strerror(errno));
        return false;
    }
    cap_streaming_ = true;

    format_ = Frame();
    format_.width = width;
    format_.height = height;
    format_.stride = stride;
    format_.plane_height = plane_height;
    format_.fourcc = drm_fourcc;
    format_.dmabuf_size = captures_.empty() ? 0 : captures_[0].size;
    format_.fb_id = captures_.empty() ? 0 : captures_[0].fb_id;
    spdlog::info("V4L2 decoder: {}x{} ({}x{} buffers, stride {}), {} frame buffers ({} for the decoder)",
                 width, height, mp.width, plane_height, stride, captures_.size(), min_buffers);
    return true;
}

bool V4l2Decoder::queue_capture(int index) {
    Capture &c = captures_[index];
    struct v4l2_plane planes[VIDEO_MAX_PLANES] = {};
    struct v4l2_buffer buf = {};
    buf.index = index;
    buf.type = cap_type_;
    buf.memory = V4L2_MEMORY_MMAP;
    buf.m.planes = planes;
    buf.length = 1;
    if (xioctl(fd_, VIDIOC_QBUF, &buf)) {
        spdlog::warn("V4L2 decoder: QBUF (frame {}): {}", index, strerror(errno));
        return false;
    }
    c.queued = true;
    c.display = false;
    return true;
}

void V4l2Decoder::maybe_requeue(int index) {
    Capture &c = captures_[index];
    if (!c.queued && !c.display && c.refs == 0 && cap_streaming_)
        queue_capture(index);
}

bool V4l2Decoder::dequeue_event(bool *source_change, bool *eos) {
    struct v4l2_event ev = {};
    if (xioctl(fd_, VIDIOC_DQEVENT, &ev))
        return false;
    if (ev.type == V4L2_EVENT_SOURCE_CHANGE &&
        (ev.u.src_change.changes & V4L2_EVENT_SRC_CH_RESOLUTION))
        *source_change = true;
    else if (ev.type == V4L2_EVENT_EOS)
        *eos = true;
    return true;
}

V4l2Decoder::Result V4l2Decoder::next(Frame *out, int timeout_ms) {
    if (fd_ < 0)
        return Result::Error;
    const uint64_t deadline = now_ms() + timeout_ms;
    for (;;) {
        bool source_change = false, eos = false;
        while (dequeue_event(&source_change, &eos)) {}
        if (source_change) {
            // The new frame buffers are allocated by reconfigure(), once the
            // caller has let go of the old ones.
            reconfig_pending_ = true;
            return Result::FormatChanged;
        }
        if (eos && draining_) {
            draining_ = false;
            return Result::Eos;
        }

        if (cap_streaming_) {
            struct v4l2_plane planes[VIDEO_MAX_PLANES] = {};
            struct v4l2_buffer buf = {};
            buf.type = cap_type_;
            buf.memory = V4L2_MEMORY_MMAP;
            buf.m.planes = planes;
            buf.length = VIDEO_MAX_PLANES;
            if (xioctl(fd_, VIDIOC_DQBUF, &buf) == 0) {
                std::lock_guard<std::mutex> lk(mtx_);
                Capture &c = captures_[buf.index];
                c.queued = false;
                if (buf.flags & V4L2_BUF_FLAG_LAST && planes[0].bytesused == 0) {
                    // Empty: the end of a drain, or of the old format before a
                    // resolution change (its event follows).
                    c.display = false;
                    if (draining_) {
                        draining_ = false;
                        return Result::Eos;
                    }
                    continue;
                }
                c.display = true;
                c.seq = next_seq_++;
                *out = format_;
                out->index = (int)buf.index;
                out->fb_id = c.fb_id;
                out->dmabuf_fd = c.dmabuf_fd;
                out->dmabuf_size = c.size;
                out->pts_us = (uint64_t)buf.timestamp.tv_sec * 1000000 + buf.timestamp.tv_usec;
                out->error = (buf.flags & V4L2_BUF_FLAG_ERROR) != 0;
                return Result::Frame;
            }
            if (errno == EPIPE) {
                // The last buffer was dequeued already: wait for the event.
            } else if (errno != EAGAIN) {
                spdlog::error("V4L2 decoder: DQBUF (frames): {}", strerror(errno));
                return Result::Error;
            }
        }

        const uint64_t now = now_ms();
        if (now >= deadline)
            return Result::Timeout;
        struct pollfd pfd = {fd_, POLLIN | POLLPRI, 0};
        const int ret = poll(&pfd, 1, (int)(deadline - now));
        if (ret < 0 && errno != EINTR)
            return Result::Error;
        // Before the frame buffers exist, or with the bitstream queue empty,
        // poll() reports an error at once: wait a little instead of spinning.
        if (ret > 0 && (pfd.revents & POLLERR) && !(pfd.revents & (POLLIN | POLLPRI)))
            usleep(2000);
    }
}

void V4l2Decoder::shown(uint32_t fb_id) {
    if (!fb_id)
        return;
    std::lock_guard<std::mutex> lk(mtx_);
    int idx = -1;
    for (size_t i = 0; i < captures_.size(); i++) {
        if (captures_[i].fb_id == fb_id) {
            idx = (int)i;
            break;
        }
    }
    if (idx < 0 || idx == shown_last_)
        return;
    shown_prev_ = shown_last_;
    shown_last_ = idx;
    const uint64_t seq = captures_[idx].seq;
    for (size_t i = 0; i < captures_.size(); i++) {
        Capture &c = captures_[i];
        if ((int)i == shown_last_ || (int)i == shown_prev_ || c.queued || !c.display || c.seq >= seq)
            continue;
        c.display = false;
        maybe_requeue((int)i);
    }
}

void V4l2Decoder::release(int index) {
    std::lock_guard<std::mutex> lk(mtx_);
    if (index < 0 || index >= (int)captures_.size())
        return;
    captures_[index].display = false;
    maybe_requeue(index);
}

void V4l2Decoder::ref(int index) {
    std::lock_guard<std::mutex> lk(mtx_);
    if (index >= 0 && index < (int)captures_.size())
        captures_[index].refs++;
}

void V4l2Decoder::unref(int index) {
    std::lock_guard<std::mutex> lk(mtx_);
    if (index < 0 || index >= (int)captures_.size() || captures_[index].refs == 0)
        return;
    captures_[index].refs--;
    maybe_requeue(index);
}
