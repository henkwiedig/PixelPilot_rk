#include "v4l2_encoder.h"

#include <errno.h>
#include <fcntl.h>
#include <poll.h>
#include <pthread.h>
#include <string.h>
#include <sys/eventfd.h>
#include <sys/ioctl.h>
#include <sys/mman.h>
#include <time.h>
#include <unistd.h>

#include <linux/videodev2.h>

// Newer than the toolchain's kernel headers (Linux 6.0).
#ifndef V4L2_CID_MPEG_VIDEO_AU_DELIMITER
#define V4L2_CID_MPEG_VIDEO_AU_DELIMITER (V4L2_CID_MPEG_BASE + 231)
#endif

#include "spdlog/spdlog.h"

#include "dvr.h"

// Frames on their way through the encoder, and coded-picture buffers. A
// coded buffer holds an uncompressed frame's worth (the driver's ceiling):
// a keyframe at a high bitrate outgrows 1 MiB, and the encoder then drops
// the access unit, so a recording that starts with it never starts.
static const unsigned kSourceBuffers = 3;
static const unsigned kCodedBuffers = 4;

static int xioctl(int fd, unsigned long req, void *arg) {
    int ret;
    do {
        ret = ioctl(fd, req, arg);
    } while (ret == -1 && errno == EINTR);
    return ret;
}

static uint32_t v4l2_codec_fourcc(VideoCodec codec) {
    return codec == VideoCodec::H265 ? V4L2_PIX_FMT_HEVC : V4L2_PIX_FMT_H264;
}

static const char *codec_name(VideoCodec codec) {
    return codec == VideoCodec::H265 ? "H.265" : "H.264";
}

V4l2Encoder::V4l2Encoder(EncoderParams p, FrameCallback cb)
    : params(p), output_cb(cb) {
    wake_fd_ = eventfd(0, EFD_CLOEXEC | EFD_NONBLOCK);
}

V4l2Encoder::~V4l2Encoder() {
    close_device();
    if (wake_fd_ >= 0)
        close(wake_fd_);
}

void V4l2Encoder::enqueue(Rpc rpc) {
    {
        std::lock_guard<std::mutex> lock(mtx);
        queue.push(std::move(rpc));
    }
    uint64_t one = 1;
    if (write(wake_fd_, &one, sizeof(one)) < 0) {}
}

void V4l2Encoder::push_frame(V4l2Decoder *dec, const V4l2Decoder::Frame &frame) {
    dec->ref(frame.index);
    Rpc rpc;
    rpc.command = Rpc::FRAME;
    rpc.dec = dec;
    rpc.frame = frame;
    enqueue(std::move(rpc));
}

void V4l2Encoder::request_idr() {
    Rpc rpc;
    rpc.command = Rpc::IDR;
    enqueue(std::move(rpc));
}

void V4l2Encoder::shutdown() {
    Rpc rpc;
    rpc.command = Rpc::SHUTDOWN;
    enqueue(std::move(rpc));
}

void V4l2Encoder::set_bitrate(int kbps) {
    Rpc rpc;
    rpc.command = Rpc::BITRATE;
    rpc.value = kbps;
    enqueue(std::move(rpc));
}

void V4l2Encoder::set_codec(VideoCodec c) {
    Rpc rpc;
    rpc.command = Rpc::CODEC;
    rpc.codec = c;
    enqueue(std::move(rpc));
}

void V4l2Encoder::set_fps(int fps) {
    Rpc rpc;
    rpc.command = Rpc::FPS;
    rpc.value = fps;
    enqueue(std::move(rpc));
}

void V4l2Encoder::drop_frames() {
    uint64_t ticket;
    {
        std::lock_guard<std::mutex> lock(mtx);
        ticket = ++drops_requested_;
    }
    Rpc rpc;
    rpc.command = Rpc::DROP;
    enqueue(std::move(rpc));
    std::unique_lock<std::mutex> lock(mtx);
    if (!drop_cv_.wait_for(lock, std::chrono::seconds(3), [&] { return drops_done_ >= ticket; }))
        spdlog::warn("V4L2 encoder: frames not given back in time");
}

void *V4l2Encoder::__THREAD__(void *param) {
    pthread_setname_np(pthread_self(), "__ENCODER");
    ((V4l2Encoder *)param)->loop();
    return nullptr;
}

void V4l2Encoder::loop() {
    for (;;) {
        std::queue<Rpc> q;
        {
            std::lock_guard<std::mutex> lock(mtx);
            std::swap(q, queue);
        }
        while (!q.empty()) {
            Rpc &rpc = q.front();
            switch (rpc.command) {
            case Rpc::FRAME:
                encode(rpc);
                break;
            case Rpc::IDR:
                idr_pending_ = true;
                break;
            case Rpc::BITRATE:
                params.bitrate_kbps = rpc.value;
                if (fd_ >= 0)
                    set_ctrl(V4L2_CID_MPEG_VIDEO_BITRATE, params.bitrate_kbps * 1000, "bitrate");
                break;
            case Rpc::CODEC:
                if (rpc.codec != params.codec) {
                    params.codec = rpc.codec;
                    reopen_ = true;
                }
                break;
            case Rpc::FPS:
                params.fps = rpc.value;
                if (fd_ >= 0)
                    apply_fps();
                break;
            case Rpc::DROP: {
                // Frames still waiting in this batch belong to the decoder too.
                std::queue<Rpc> rest;
                q.pop();
                while (!q.empty()) {
                    if (q.front().command == Rpc::FRAME)
                        q.front().dec->unref(q.front().frame.index);
                    else
                        rest.push(std::move(q.front()));
                    q.pop();
                }
                close_device();
                {
                    std::lock_guard<std::mutex> lock(mtx);
                    drops_done_++;
                }
                drop_cv_.notify_all();
                std::swap(q, rest);
                continue;
            }
            case Rpc::SHUTDOWN:
                q.pop();
                while (!q.empty()) {
                    if (q.front().command == Rpc::FRAME)
                        q.front().dec->unref(q.front().frame.index);
                    q.pop();
                }
                close_device();
                spdlog::info("V4L2 encoder: done");
                return;
            }
            q.pop();
        }

        bool inflight = false;
        for (const Source &s : sources_)
            inflight |= s.busy;
        struct pollfd fds[2] = {{wake_fd_, POLLIN, 0}, {fd_, POLLIN | POLLOUT, 0}};
        const int n = (fd_ >= 0 && inflight) ? 2 : 1;
        if (poll(fds, n, 500) <= 0)
            continue;
        if (fds[0].revents & POLLIN) {
            uint64_t v;
            if (read(wake_fd_, &v, sizeof(v)) < 0) {}
        }
        if (n == 2 && fds[1].revents)
            collect_output();
    }
}

static int open_encoder_for(VideoCodec codec) {
    const uint32_t fourcc = v4l2_codec_fourcc(codec);
    for (int i = 0; i < 16; i++) {
        char path[32];
        snprintf(path, sizeof(path), "/dev/video%d", i);
        int fd = open(path, O_RDWR | O_NONBLOCK | O_CLOEXEC);
        if (fd < 0)
            continue;
        struct v4l2_capability cap = {};
        if (xioctl(fd, VIDIOC_QUERYCAP, &cap) == 0) {
            const uint32_t caps = (cap.capabilities & V4L2_CAP_DEVICE_CAPS) ? cap.device_caps : cap.capabilities;
            if (caps & V4L2_CAP_VIDEO_M2M_MPLANE) {
                for (uint32_t j = 0;; j++) {
                    struct v4l2_fmtdesc desc = {};
                    desc.index = j;
                    desc.type = V4L2_BUF_TYPE_VIDEO_CAPTURE_MPLANE;
                    if (xioctl(fd, VIDIOC_ENUM_FMT, &desc))
                        break;
                    if (desc.pixelformat == fourcc) {
                        spdlog::info("V4L2 encoder: {} ({})", path, (const char *)cap.card);
                        return fd;
                    }
                }
            }
        }
        close(fd);
    }
    return -1;
}

bool V4l2Encoder::set_ctrl(uint32_t id, int32_t value, const char *name) {
    struct v4l2_control ctrl = {};
    ctrl.id = id;
    ctrl.value = value;
    if (xioctl(fd_, VIDIOC_S_CTRL, &ctrl)) {
        spdlog::debug("V4L2 encoder: {} = {}: {}", name, value, strerror(errno));
        return false;
    }
    return true;
}

void V4l2Encoder::apply_fps() {
    struct v4l2_streamparm parm = {};
    parm.type = V4L2_BUF_TYPE_VIDEO_OUTPUT_MPLANE;
    parm.parm.output.timeperframe.numerator = 1;
    parm.parm.output.timeperframe.denominator = params.fps > 0 ? params.fps : 30;
    if (xioctl(fd_, VIDIOC_S_PARM, &parm))
        spdlog::warn("V4L2 encoder: S_PARM {} fps: {}", params.fps, strerror(errno));
}

bool V4l2Encoder::open_device(const V4l2Decoder::Frame &f) {
    fd_ = open_encoder_for(params.codec);
    if (fd_ < 0) {
        spdlog::error("V4L2 encoder: no device encodes {}", codec_name(params.codec));
        return false;
    }

    struct v4l2_format fmt = {};
    fmt.type = V4L2_BUF_TYPE_VIDEO_CAPTURE_MPLANE;
    fmt.fmt.pix_mp.pixelformat = v4l2_codec_fourcc(params.codec);
    fmt.fmt.pix_mp.width = f.width;
    fmt.fmt.pix_mp.height = f.height;
    fmt.fmt.pix_mp.num_planes = 1;
    fmt.fmt.pix_mp.plane_fmt[0].sizeimage = f.width * f.height * 3 / 2;
    if (xioctl(fd_, VIDIOC_S_FMT, &fmt)) {
        spdlog::error("V4L2 encoder: S_FMT (coded): {}", strerror(errno));
        close_device();
        return false;
    }

    // The decoder's buffer as it is: I420, its stride, its padded rows (the
    // chroma planes follow those); the picture is cropped to the visible rows.
    fmt = {};
    fmt.type = V4L2_BUF_TYPE_VIDEO_OUTPUT_MPLANE;
    fmt.fmt.pix_mp.pixelformat = V4L2_PIX_FMT_YUV420;
    fmt.fmt.pix_mp.width = f.width;
    fmt.fmt.pix_mp.height = f.plane_height;
    fmt.fmt.pix_mp.num_planes = 1;
    fmt.fmt.pix_mp.plane_fmt[0].bytesperline = f.stride;
    if (xioctl(fd_, VIDIOC_S_FMT, &fmt)) {
        spdlog::error("V4L2 encoder: S_FMT (frames): {}", strerror(errno));
        close_device();
        return false;
    }
    const uint32_t offered_stride = fmt.fmt.pix_mp.plane_fmt[0].bytesperline;
    const uint32_t offered_size = fmt.fmt.pix_mp.plane_fmt[0].sizeimage;
    if (fmt.fmt.pix_mp.pixelformat != V4L2_PIX_FMT_YUV420 || fmt.fmt.pix_mp.num_planes != 1 ||
        offered_stride != f.stride || offered_size > f.dmabuf_size) {
        spdlog::error("V4L2 encoder: cannot take the decoder's {}x{} frames (stride {}, {} bytes): "
                      "offers stride {}, {} bytes", f.width, f.plane_height, f.stride, f.dmabuf_size,
                      offered_stride, offered_size);
        close_device();
        return false;
    }

    struct v4l2_selection sel = {};
    sel.type = V4L2_BUF_TYPE_VIDEO_OUTPUT;
    sel.target = V4L2_SEL_TGT_CROP;
    sel.r.width = f.width;
    sel.r.height = f.height;
    if (xioctl(fd_, VIDIOC_S_SELECTION, &sel))
        spdlog::warn("V4L2 encoder: crop to {}x{}: {}", f.width, f.height, strerror(errno));

    apply_fps();
    const int gop = params.fps > 0 ? params.fps : 30;   // a keyframe a second
    set_ctrl(V4L2_CID_MPEG_VIDEO_FRAME_RC_ENABLE, 1, "rate control");
    set_ctrl(V4L2_CID_MPEG_VIDEO_BITRATE_MODE, V4L2_MPEG_VIDEO_BITRATE_MODE_CBR, "bitrate mode");
    set_ctrl(V4L2_CID_MPEG_VIDEO_BITRATE, params.bitrate_kbps * 1000, "bitrate");
    set_ctrl(V4L2_CID_MPEG_VIDEO_GOP_SIZE, gop, "gop size");
    set_ctrl(V4L2_CID_MPEG_VIDEO_AU_DELIMITER, 0, "access unit delimiters");
    if (params.codec == VideoCodec::H265) {
        set_ctrl(V4L2_CID_MPEG_VIDEO_HEVC_REFRESH_TYPE, V4L2_MPEG_VIDEO_HEVC_REFRESH_IDR, "refresh type");
        set_ctrl(V4L2_CID_MPEG_VIDEO_HEVC_REFRESH_PERIOD, gop, "refresh period");
    } else {
        set_ctrl(V4L2_CID_MPEG_VIDEO_H264_I_PERIOD, gop, "intra period");
        set_ctrl(V4L2_CID_MPEG_VIDEO_PREPEND_SPSPPS_TO_IDR, 1, "headers on IDR");
    }

    struct v4l2_requestbuffers req = {};
    req.count = kSourceBuffers;
    req.type = V4L2_BUF_TYPE_VIDEO_OUTPUT_MPLANE;
    req.memory = V4L2_MEMORY_DMABUF;
    if (xioctl(fd_, VIDIOC_REQBUFS, &req) || req.count == 0) {
        spdlog::error("V4L2 encoder: REQBUFS (frames): {}", strerror(errno));
        close_device();
        return false;
    }
    sources_.assign(req.count, Source());

    req = {};
    req.count = kCodedBuffers;
    req.type = V4L2_BUF_TYPE_VIDEO_CAPTURE_MPLANE;
    req.memory = V4L2_MEMORY_MMAP;
    if (xioctl(fd_, VIDIOC_REQBUFS, &req) || req.count == 0) {
        spdlog::error("V4L2 encoder: REQBUFS (coded): {}", strerror(errno));
        close_device();
        return false;
    }
    coded_.assign(req.count, {nullptr, 0});
    for (uint32_t i = 0; i < req.count; i++) {
        struct v4l2_plane planes[VIDEO_MAX_PLANES] = {};
        struct v4l2_buffer buf = {};
        buf.index = i;
        buf.type = V4L2_BUF_TYPE_VIDEO_CAPTURE_MPLANE;
        buf.memory = V4L2_MEMORY_MMAP;
        buf.m.planes = planes;
        buf.length = VIDEO_MAX_PLANES;
        if (xioctl(fd_, VIDIOC_QUERYBUF, &buf)) {
            spdlog::error("V4L2 encoder: QUERYBUF (coded): {}", strerror(errno));
            close_device();
            return false;
        }
        void *map = mmap(NULL, planes[0].length, PROT_READ, MAP_SHARED, fd_, planes[0].m.mem_offset);
        if (map == MAP_FAILED) {
            spdlog::error("V4L2 encoder: mmap (coded): {}", strerror(errno));
            close_device();
            return false;
        }
        coded_[i] = {map, planes[0].length};
        buf.length = 1;
        if (xioctl(fd_, VIDIOC_QBUF, &buf)) {
            spdlog::error("V4L2 encoder: QBUF (coded): {}", strerror(errno));
            close_device();
            return false;
        }
    }

    int type = V4L2_BUF_TYPE_VIDEO_OUTPUT_MPLANE;
    if (xioctl(fd_, VIDIOC_STREAMON, &type)) {
        spdlog::error("V4L2 encoder: STREAMON (frames): {}", strerror(errno));
        close_device();
        return false;
    }
    type = V4L2_BUF_TYPE_VIDEO_CAPTURE_MPLANE;
    if (xioctl(fd_, VIDIOC_STREAMON, &type)) {
        spdlog::error("V4L2 encoder: STREAMON (coded): {}", strerror(errno));
        close_device();
        return false;
    }

    width_ = f.width;
    height_ = f.height;
    stride_ = f.stride;
    plane_height_ = f.plane_height;
    reopen_ = false;
    idr_pending_ = false;   // the first picture is one anyway
    spdlog::info("V4L2 encoder: {} {}x{} at {} fps, {} kbit/s", codec_name(params.codec), f.width, f.height,
                 params.fps, params.bitrate_kbps);
    return true;
}

void V4l2Encoder::return_sources() {
    for (Source &s : sources_) {
        if (s.busy && s.dec)
            s.dec->unref(s.index);
        s = Source();
    }
}

void V4l2Encoder::close_device() {
    if (fd_ < 0)
        return;
    int type = V4L2_BUF_TYPE_VIDEO_OUTPUT_MPLANE;
    xioctl(fd_, VIDIOC_STREAMOFF, &type);
    type = V4L2_BUF_TYPE_VIDEO_CAPTURE_MPLANE;
    xioctl(fd_, VIDIOC_STREAMOFF, &type);
    // The frames are the driver's no longer; freeing the queue lets go of
    // the dma-bufs, after which the decoder may free them.
    struct v4l2_requestbuffers req = {};
    req.type = V4L2_BUF_TYPE_VIDEO_OUTPUT_MPLANE;
    req.memory = V4L2_MEMORY_DMABUF;
    xioctl(fd_, VIDIOC_REQBUFS, &req);
    return_sources();
    sources_.clear();
    for (auto &c : coded_)
        if (c.first)
            munmap(c.first, c.second);
    coded_.clear();
    req = {};
    req.type = V4L2_BUF_TYPE_VIDEO_CAPTURE_MPLANE;
    req.memory = V4L2_MEMORY_MMAP;
    xioctl(fd_, VIDIOC_REQBUFS, &req);
    close(fd_);
    fd_ = -1;
}

void V4l2Encoder::encode(const Rpc &rpc) {
    const V4l2Decoder::Frame &f = rpc.frame;
    if (fd_ >= 0 && (reopen_ || f.width != width_ || f.height != height_ || f.stride != stride_ ||
                     f.plane_height != plane_height_))
        close_device();
    if (fd_ < 0 && !open_device(f)) {
        rpc.dec->unref(f.index);
        return;
    }
    collect_output();

    int slot = -1;
    for (size_t i = 0; i < sources_.size(); i++) {
        if (!sources_[i].busy) {
            slot = (int)i;
            break;
        }
    }
    if (slot < 0) {
        // The encoder is behind: this frame is skipped.
        static uint64_t last_warn_ms = 0;
        struct timespec ts;
        clock_gettime(CLOCK_MONOTONIC, &ts);
        const uint64_t now = (uint64_t)ts.tv_sec * 1000 + ts.tv_nsec / 1000000;
        if (now - last_warn_ms > 1000) {
            last_warn_ms = now;
            spdlog::warn("V4L2 encoder: behind, frame skipped");
        }
        rpc.dec->unref(f.index);
        return;
    }

    if (idr_pending_) {
        set_ctrl(V4L2_CID_MPEG_VIDEO_FORCE_KEY_FRAME, 1, "keyframe");
        idr_pending_ = false;
    }

    struct v4l2_plane planes[VIDEO_MAX_PLANES] = {};
    struct v4l2_buffer buf = {};
    buf.index = slot;
    buf.type = V4L2_BUF_TYPE_VIDEO_OUTPUT_MPLANE;
    buf.memory = V4L2_MEMORY_DMABUF;
    buf.m.planes = planes;
    buf.length = 1;
    planes[0].m.fd = f.dmabuf_fd;
    planes[0].length = f.dmabuf_size;
    planes[0].bytesused = f.dmabuf_size;
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    buf.timestamp.tv_sec = ts.tv_sec;
    buf.timestamp.tv_usec = ts.tv_nsec / 1000;
    if (xioctl(fd_, VIDIOC_QBUF, &buf)) {
        spdlog::warn("V4L2 encoder: QBUF (frame): {}", strerror(errno));
        rpc.dec->unref(f.index);
        return;
    }
    sources_[slot] = {true, rpc.dec, f.index};
}

void V4l2Encoder::collect_output() {
    if (fd_ < 0)
        return;
    for (;;) {
        struct v4l2_plane planes[VIDEO_MAX_PLANES] = {};
        struct v4l2_buffer buf = {};
        buf.type = V4L2_BUF_TYPE_VIDEO_OUTPUT_MPLANE;
        buf.memory = V4L2_MEMORY_DMABUF;
        buf.m.planes = planes;
        buf.length = VIDEO_MAX_PLANES;
        if (xioctl(fd_, VIDIOC_DQBUF, &buf))
            break;
        Source &s = sources_[buf.index];
        if (s.busy && s.dec)
            s.dec->unref(s.index);
        s = Source();
    }
    for (;;) {
        struct v4l2_plane planes[VIDEO_MAX_PLANES] = {};
        struct v4l2_buffer buf = {};
        buf.type = V4L2_BUF_TYPE_VIDEO_CAPTURE_MPLANE;
        buf.memory = V4L2_MEMORY_MMAP;
        buf.m.planes = planes;
        buf.length = VIDEO_MAX_PLANES;
        if (xioctl(fd_, VIDIOC_DQBUF, &buf))
            break;
        const uint32_t used = planes[0].bytesused;
        if (buf.flags & V4L2_BUF_FLAG_ERROR) {
            static uint64_t last_warn_ms = 0;
            struct timespec ts;
            clock_gettime(CLOCK_MONOTONIC, &ts);
            const uint64_t now = (uint64_t)ts.tv_sec * 1000 + ts.tv_nsec / 1000000;
            if (now - last_warn_ms > 1000) {
                last_warn_ms = now;
                spdlog::warn("V4L2 encoder: a picture failed to encode");
            }
        }
        {
            // What the encoder hands out, every 5 s at debug level.
            static uint64_t stat_start_ms, stat_pictures, stat_empty, stat_bytes, stat_keyframes;
            struct timespec ts;
            clock_gettime(CLOCK_MONOTONIC, &ts);
            const uint64_t now = (uint64_t)ts.tv_sec * 1000 + ts.tv_nsec / 1000000;
            if (!stat_start_ms)
                stat_start_ms = now;
            stat_pictures++;
            stat_bytes += used;
            if (!used) stat_empty++;
            if (buf.flags & V4L2_BUF_FLAG_KEYFRAME) stat_keyframes++;
            if (now - stat_start_ms >= 5000) {
                spdlog::debug("V4L2 encoder: {} pictures ({} keyframes, {} empty), {} kbit/s",
                              stat_pictures, stat_keyframes, stat_empty,
                              stat_bytes * 8 / (now - stat_start_ms));
                stat_start_ms = now;
                stat_pictures = stat_empty = stat_bytes = stat_keyframes = 0;
            }
        }
        if (used && !(buf.flags & V4L2_BUF_FLAG_ERROR) && buf.index < coded_.size() &&
            used <= coded_[buf.index].second && output_cb) {
            const uint8_t *data = (const uint8_t *)coded_[buf.index].first + planes[0].data_offset;
            output_cb(std::make_shared<std::vector<uint8_t>>(data, data + used));
        }
        buf.length = 1;
        planes[0].bytesused = 0;
        if (xioctl(fd_, VIDIOC_QBUF, &buf))
            spdlog::warn("V4L2 encoder: QBUF (coded): {}", strerror(errno));
    }
}

FrameProcessor::FrameProcessor(V4l2Encoder *enc, int fps, EncResolution res, int drm_fd,
                               std::function<void()> on_fatal_error)
    : encoder(enc), interval_ns(1000000000L / (fps > 0 ? fps : 30)) {
    (void)drm_fd;
    (void)on_fatal_error;
    set_resolution(res);
}

FrameProcessor::~FrameProcessor() {
    std::lock_guard<std::mutex> lk(mtx);
    drop_latest();
}

void FrameProcessor::drop_latest() {
    if (dec_ && latest_.index >= 0)
        dec_->unref(latest_.index);
    latest_ = V4l2Decoder::Frame();
}

void FrameProcessor::push_latest(V4l2Decoder *dec, const V4l2Decoder::Frame &frame) {
    std::lock_guard<std::mutex> lk(mtx);
    drop_latest();
    // Not recording: hold nothing, encode nothing.
    if (!running || !dvr_enabled)
        return;
    dec->ref(frame.index);
    dec_ = dec;
    latest_ = frame;
}

void FrameProcessor::shutdown() {
    {
        std::lock_guard<std::mutex> lk(mtx);
        running = false;
        drop_latest();
    }
    cv.notify_all();
}

void FrameProcessor::set_fps(int fps) {
    std::lock_guard<std::mutex> lk(mtx);
    interval_ns = 1000000000L / (fps > 0 ? fps : 30);
}

void FrameProcessor::set_resolution(EncResolution r) {
    if (r != EncResolution::Res1080p)
        spdlog::info("DVR re-encode: no scaler here, the recording keeps the video's resolution");
}

void FrameProcessor::set_color_correction(float gain, float offset) {
    (void)gain;
    (void)offset;
    spdlog::info("DVR re-encode: no GPU here, the recording is not colour corrected");
}

void FrameProcessor::set_osd_blend(int prime_fd, uint32_t w, uint32_t h, uint32_t stride_px) {
    (void)prime_fd; (void)w; (void)h; (void)stride_px;
}

void FrameProcessor::drain_decoder_refs() {
    {
        std::lock_guard<std::mutex> lk(mtx);
        drop_latest();
    }
    encoder->drop_frames();
}

void *FrameProcessor::__THREAD__(void *p) {
    pthread_setname_np(pthread_self(), "__FRAME_PACER");
    ((FrameProcessor *)p)->loop();
    return nullptr;
}

void FrameProcessor::loop() {
    struct timespec next;
    clock_gettime(CLOCK_MONOTONIC, &next);
    for (;;) {
        long interval;
        {
            std::lock_guard<std::mutex> lk(mtx);
            if (!running)
                break;
            interval = interval_ns;
        }
        next.tv_nsec += interval;
        while (next.tv_nsec >= 1000000000L) {
            next.tv_nsec -= 1000000000L;
            next.tv_sec++;
        }
        struct timespec now;
        clock_gettime(CLOCK_MONOTONIC, &now);
        // Fell behind by more than a frame: start counting from now.
        if (now.tv_sec > next.tv_sec + 1 ||
            (now.tv_sec - next.tv_sec) * 1000000000L + (now.tv_nsec - next.tv_nsec) > interval)
            next = now;
        while (clock_nanosleep(CLOCK_MONOTONIC, TIMER_ABSTIME, &next, NULL) == EINTR) {}

        std::lock_guard<std::mutex> lk(mtx);
        if (!running)
            break;
        if (!dvr_enabled)
            drop_latest();
        else if (dec_ && latest_.index >= 0)
            encoder->push_frame(dec_, latest_);
    }
    spdlog::info("Frame pacer done.");
}
