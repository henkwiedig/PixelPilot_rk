#include "uvc_sink.h"

#include <errno.h>
#include <fcntl.h>
#include <poll.h>
#include <string.h>
#include <unistd.h>

#include <sys/ioctl.h>
#include <sys/mman.h>
#include <sys/stat.h>

#include <linux/videodev2.h>
#include <linux/usb/ch9.h>
#include <linux/usb/video.h>
#include <linux/usb/g_uvc.h>

#include "spdlog/spdlog.h"

namespace {
constexpr unsigned NBUF = 4;
constexpr int UVC_SC_LEN = (int)sizeof(struct uvc_streaming_control);

int xioctl(int fd, unsigned long req, void *arg) {
    int r;
    do { r = ioctl(fd, req, arg); } while (r == -1 && errno == EINTR);
    return r;
}
} // namespace

UvcSink::UvcSink(std::vector<FrameSize> frames, std::string device)
    : frames_(std::move(frames)), device_path_(std::move(device)) {
    if (frames_.empty())
        frames_.push_back({1280, 720, 30});
    probe_.resize(UVC_SC_LEN);
    commit_.resize(UVC_SC_LEN);
    // Seed probe/commit with the default (preferred) frame.
    fill_streaming_control(probe_.data(), 1, 0);
    fill_streaming_control(commit_.data(), 1, 0);
}

UvcSink::~UvcSink() {
    stop();
}

std::string UvcSink::discover_device() {
    for (int i = 0; i < 64; ++i) {
        char path[32];
        snprintf(path, sizeof(path), "/dev/video%d", i);
        int fd = open(path, O_RDWR | O_NONBLOCK);
        if (fd < 0) continue;
        struct v4l2_capability cap;
        memset(&cap, 0, sizeof(cap));
        bool match = false;
        if (xioctl(fd, VIDIOC_QUERYCAP, &cap) == 0) {
            __u32 caps = (cap.capabilities & V4L2_CAP_DEVICE_CAPS)
                             ? cap.device_caps : cap.capabilities;
            // The f_uvc gadget node is a VIDEO_OUTPUT device on the "gadget" bus.
            if ((caps & V4L2_CAP_VIDEO_OUTPUT) &&
                strstr((const char *)cap.bus_info, "gadget"))
                match = true;
        }
        close(fd);
        if (match) return path;
    }
    return "";
}

bool UvcSink::start() {
    if (running_.load()) return true;

    if (device_path_.empty()) {
        device_path_ = discover_device();
        if (device_path_.empty()) {
            spdlog::error("UvcSink: no UVC gadget V4L2 node found (is webcam-gadget up?)");
            return false;
        }
    }

    fd_ = open(device_path_.c_str(), O_RDWR | O_NONBLOCK);
    if (fd_ < 0) {
        spdlog::error("UvcSink: cannot open {}: {}", device_path_, strerror(errno));
        return false;
    }

    struct v4l2_event_subscription sub;
    const __u32 events[] = {UVC_EVENT_SETUP, UVC_EVENT_DATA,
                            UVC_EVENT_STREAMON, UVC_EVENT_STREAMOFF,
                            UVC_EVENT_DISCONNECT};
    for (__u32 ev : events) {
        memset(&sub, 0, sizeof(sub));
        sub.type = ev;
        if (xioctl(fd_, VIDIOC_SUBSCRIBE_EVENT, &sub) < 0)
            spdlog::warn("UvcSink: subscribe event {:#x} failed: {}", ev, strerror(errno));
    }

    running_.store(true);
    worker_ = std::thread(&UvcSink::__THREAD__, this);
    spdlog::info("UvcSink: started on {}", device_path_);
    return true;
}

void UvcSink::stop() {
    if (!running_.exchange(false)) {
        if (fd_ >= 0) { close(fd_); fd_ = -1; }
        return;
    }
    if (worker_.joinable()) worker_.join();
    stop_streaming();
    if (fd_ >= 0) { close(fd_); fd_ = -1; }
    spdlog::info("UvcSink: stopped");
}

void UvcSink::submit_frame(std::shared_ptr<std::vector<uint8_t>> jpeg) {
    std::lock_guard<std::mutex> lk(frame_mtx_);
    latest_frame_ = std::move(jpeg);
}

void *UvcSink::__THREAD__(void *self) {
    pthread_setname_np(pthread_self(), "__UVCSINK");
    static_cast<UvcSink *>(self)->loop();
    return nullptr;
}

void UvcSink::loop() {
    while (running_.load()) {
        struct pollfd pfd;
        pfd.fd = fd_;
        pfd.events = POLLPRI;
        if (streaming_.load()) pfd.events |= POLLOUT;
        pfd.revents = 0;

        int r = poll(&pfd, 1, 200);
        if (r < 0) {
            if (errno == EINTR) continue;
            spdlog::warn("UvcSink: poll error: {}", strerror(errno));
            break;
        }
        if (r == 0) continue;
        if (pfd.revents & (POLLERR | POLLHUP | POLLNVAL)) {
            // Host detached / device gone — drop streaming, keep waiting.
            stop_streaming();
            continue;
        }
        if (pfd.revents & POLLPRI) process_events();
        if (pfd.revents & POLLOUT) process_video();
    }
}

// ---------------------------------------------------------------------------
// UVC event handling
// ---------------------------------------------------------------------------
void UvcSink::process_events() {
    struct v4l2_event v4l2_ev;
    memset(&v4l2_ev, 0, sizeof(v4l2_ev));
    if (xioctl(fd_, VIDIOC_DQEVENT, &v4l2_ev) < 0)
        return;

    struct uvc_event *uvc_ev = reinterpret_cast<struct uvc_event *>(&v4l2_ev.u.data);
    struct uvc_request_data resp;
    memset(&resp, 0, sizeof(resp));
    resp.length = -EL2HLT;  // stall unless a handler fills it

    switch (v4l2_ev.type) {
    case UVC_EVENT_DISCONNECT:
        stop_streaming();
        return;
    case UVC_EVENT_STREAMON:
        start_streaming();
        return;
    case UVC_EVENT_STREAMOFF:
        stop_streaming();
        return;
    case UVC_EVENT_SETUP:
        handle_setup(&uvc_ev->req, &resp);
        break;
    case UVC_EVENT_DATA:
        handle_data(&uvc_ev->data);
        return;
    default:
        return;
    }

    if (xioctl(fd_, UVCIOC_SEND_RESPONSE, &resp) < 0)
        spdlog::debug("UvcSink: UVCIOC_SEND_RESPONSE failed: {}", strerror(errno));
}

void UvcSink::handle_setup(const void *ctrl_ptr, void *resp_ptr) {
    const struct usb_ctrlrequest *ctrl =
        static_cast<const struct usb_ctrlrequest *>(ctrl_ptr);
    struct uvc_request_data *resp = static_cast<struct uvc_request_data *>(resp_ptr);

    // Only class-specific interface requests are relevant here.
    if ((ctrl->bRequestType & USB_TYPE_MASK) != USB_TYPE_CLASS)
        return;  // standard/vendor — leave stalled; driver handles SET_INTERFACE

    uint8_t cs = ctrl->wValue >> 8;  // control selector
    switch (cs) {
    case UVC_VS_PROBE_CONTROL:
        handle_streaming_control(ctrl->bRequest, probe_.data(), resp);
        // Only a SET_CUR is followed by a DATA phase to apply.
        if (ctrl->bRequest == UVC_SET_CUR) control_pending_ = 1;
        break;
    case UVC_VS_COMMIT_CONTROL:
        handle_streaming_control(ctrl->bRequest, commit_.data(), resp);
        if (ctrl->bRequest == UVC_SET_CUR) control_pending_ = 2;
        break;
    default:
        // VideoControl unit/terminal requests — we expose no controls. GET_INFO
        // answers "no capabilities" so hosts don't retry endlessly; others stall.
        if (ctrl->bRequest == UVC_GET_INFO) {
            resp->data[0] = 0x00;
            resp->length = 1;
        }
        break;
    }
}

void UvcSink::handle_streaming_control(uint8_t req, void *target, void *resp_ptr) {
    struct uvc_request_data *resp = static_cast<struct uvc_request_data *>(resp_ptr);

    switch (req) {
    case UVC_SET_CUR:
        // Data phase follows in UVC_EVENT_DATA; accept the full control length.
        resp->length = UVC_SC_LEN;
        break;
    case UVC_GET_CUR:
        memcpy(resp->data, target, UVC_SC_LEN);
        resp->length = UVC_SC_LEN;
        break;
    case UVC_GET_MIN:
    case UVC_GET_DEF: {
        struct uvc_streaming_control sc;
        fill_streaming_control(&sc, 1, 0);  // preferred frame
        memcpy(resp->data, &sc, UVC_SC_LEN);
        resp->length = UVC_SC_LEN;
        break;
    }
    case UVC_GET_MAX: {
        struct uvc_streaming_control sc;
        fill_streaming_control(&sc, (int)frames_.size(), 0);  // largest frame
        memcpy(resp->data, &sc, UVC_SC_LEN);
        resp->length = UVC_SC_LEN;
        break;
    }
    case UVC_GET_RES:
        memset(resp->data, 0, UVC_SC_LEN);
        resp->length = UVC_SC_LEN;
        break;
    case UVC_GET_LEN:
        resp->data[0] = UVC_SC_LEN;
        resp->data[1] = 0x00;
        resp->length = 2;
        break;
    case UVC_GET_INFO:
        resp->data[0] = UVC_CONTROL_CAP_GET | UVC_CONTROL_CAP_SET;
        resp->length = 1;
        break;
    default:
        break;
    }
}

void UvcSink::handle_data(const void *data_ptr) {
    const struct uvc_request_data *data =
        static_cast<const struct uvc_request_data *>(data_ptr);
    if (control_pending_ == 0) return;
    if (data->length < UVC_SC_LEN) return;

    const struct uvc_streaming_control *in =
        reinterpret_cast<const struct uvc_streaming_control *>(data->data);

    int iframe = in->bFrameIndex;
    if (iframe < 1) iframe = 1;
    if (iframe > (int)frames_.size()) iframe = (int)frames_.size();

    void *target = (control_pending_ == 2) ? commit_.data() : probe_.data();
    fill_streaming_control(target, iframe, in->dwFrameInterval);

    if (control_pending_ == 2) {
        // COMMIT latched — this is the format the host will stream.
        current_frame_index_ = iframe;
        spdlog::info("UvcSink: host committed {}x{}",
                     frames_[iframe - 1].width, frames_[iframe - 1].height);
    }
    control_pending_ = 0;
}

void UvcSink::fill_streaming_control(void *sc_ptr, int iframe, uint32_t interval) {
    struct uvc_streaming_control *sc =
        static_cast<struct uvc_streaming_control *>(sc_ptr);
    if (iframe < 1) iframe = 1;
    if (iframe > (int)frames_.size()) iframe = (int)frames_.size();
    const FrameSize &f = frames_[iframe - 1];

    memset(sc, 0, UVC_SC_LEN);
    sc->bmHint = 1;                        // dwFrameInterval fixed
    sc->bFormatIndex = 1;                  // single MJPEG format
    sc->bFrameIndex = (uint8_t)iframe;
    sc->dwFrameInterval = interval ? interval : (10000000u / f.fps);
    sc->dwMaxVideoFrameSize = f.width * f.height * 2;  // generous MJPEG cap
    sc->dwMaxPayloadTransferSize = max_payload_;
    sc->bmFramingInfo = 3;
    sc->bPreferedVersion = 1;
    sc->bMinVersion = 1;
    sc->bMaxVersion = 1;
}

// ---------------------------------------------------------------------------
// Streaming lifecycle
// ---------------------------------------------------------------------------
void UvcSink::start_streaming() {
    if (streaming_.load()) return;

    int idx = current_frame_index_;
    if (idx < 1 || idx > (int)frames_.size()) idx = 1;
    const FrameSize &f = frames_[idx - 1];

    struct v4l2_format fmt;
    memset(&fmt, 0, sizeof(fmt));
    fmt.type = V4L2_BUF_TYPE_VIDEO_OUTPUT;
    fmt.fmt.pix.width = f.width;
    fmt.fmt.pix.height = f.height;
    fmt.fmt.pix.pixelformat = V4L2_PIX_FMT_MJPEG;
    fmt.fmt.pix.field = V4L2_FIELD_NONE;
    fmt.fmt.pix.sizeimage = f.width * f.height * 2;
    if (xioctl(fd_, VIDIOC_S_FMT, &fmt) < 0) {
        spdlog::error("UvcSink: VIDIOC_S_FMT failed: {}", strerror(errno));
        return;
    }

    struct v4l2_requestbuffers rb;
    memset(&rb, 0, sizeof(rb));
    rb.count = NBUF;
    rb.type = V4L2_BUF_TYPE_VIDEO_OUTPUT;
    rb.memory = V4L2_MEMORY_MMAP;
    if (xioctl(fd_, VIDIOC_REQBUFS, &rb) < 0 || rb.count == 0) {
        spdlog::error("UvcSink: VIDIOC_REQBUFS failed: {}", strerror(errno));
        return;
    }

    buffers_.assign(rb.count, MappedBuf{});
    for (unsigned i = 0; i < rb.count; ++i) {
        struct v4l2_buffer buf;
        memset(&buf, 0, sizeof(buf));
        buf.type = V4L2_BUF_TYPE_VIDEO_OUTPUT;
        buf.memory = V4L2_MEMORY_MMAP;
        buf.index = i;
        if (xioctl(fd_, VIDIOC_QUERYBUF, &buf) < 0) {
            spdlog::error("UvcSink: VIDIOC_QUERYBUF failed: {}", strerror(errno));
            stop_streaming();
            return;
        }
        void *p = mmap(nullptr, buf.length, PROT_READ | PROT_WRITE,
                       MAP_SHARED, fd_, buf.m.offset);
        if (p == MAP_FAILED) {
            spdlog::error("UvcSink: mmap failed: {}", strerror(errno));
            stop_streaming();
            return;
        }
        buffers_[i].start = p;
        buffers_[i].length = buf.length;

        fill_buffer(i, &buf);
        if (xioctl(fd_, VIDIOC_QBUF, &buf) < 0) {
            spdlog::error("UvcSink: initial VIDIOC_QBUF failed: {}", strerror(errno));
            stop_streaming();
            return;
        }
    }

    int type = V4L2_BUF_TYPE_VIDEO_OUTPUT;
    if (xioctl(fd_, VIDIOC_STREAMON, &type) < 0) {
        spdlog::error("UvcSink: VIDIOC_STREAMON failed: {}", strerror(errno));
        stop_streaming();
        return;
    }

    streaming_.store(true);
    spdlog::info("UvcSink: streaming {}x{} MJPEG", f.width, f.height);
}

void UvcSink::stop_streaming() {
    if (fd_ >= 0 && (streaming_.load() || !buffers_.empty())) {
        int type = V4L2_BUF_TYPE_VIDEO_OUTPUT;
        xioctl(fd_, VIDIOC_STREAMOFF, &type);
    }
    for (auto &b : buffers_) {
        if (b.start && b.start != MAP_FAILED) munmap(b.start, b.length);
    }
    buffers_.clear();

    if (fd_ >= 0) {
        struct v4l2_requestbuffers rb;
        memset(&rb, 0, sizeof(rb));
        rb.count = 0;
        rb.type = V4L2_BUF_TYPE_VIDEO_OUTPUT;
        rb.memory = V4L2_MEMORY_MMAP;
        xioctl(fd_, VIDIOC_REQBUFS, &rb);
    }
    streaming_.store(false);
}

void UvcSink::process_video() {
    struct v4l2_buffer buf;
    memset(&buf, 0, sizeof(buf));
    buf.type = V4L2_BUF_TYPE_VIDEO_OUTPUT;
    buf.memory = V4L2_MEMORY_MMAP;
    if (xioctl(fd_, VIDIOC_DQBUF, &buf) < 0) {
        if (errno != EAGAIN)
            spdlog::debug("UvcSink: VIDIOC_DQBUF failed: {}", strerror(errno));
        return;
    }
    fill_buffer(buf.index, &buf);
    static uint64_t n = 0;  // __UVCSINK thread only
    if (++n % 300 == 0)     // ~every 10s at 30fps
        spdlog::debug("UvcSink: delivered {} buffers, last={} bytes", n, buf.bytesused);
    if (xioctl(fd_, VIDIOC_QBUF, &buf) < 0)
        spdlog::debug("UvcSink: VIDIOC_QBUF failed: {}", strerror(errno));
}

void UvcSink::fill_buffer(unsigned index, void *v4l2_buf_ptr) {
    struct v4l2_buffer *buf = static_cast<struct v4l2_buffer *>(v4l2_buf_ptr);
    std::shared_ptr<std::vector<uint8_t>> frame;
    {
        std::lock_guard<std::mutex> lk(frame_mtx_);
        frame = latest_frame_;
    }
    size_t n = 0;
    if (index < buffers_.size() && frame && !frame->empty()) {
        n = frame->size();
        if (n > buffers_[index].length) n = buffers_[index].length;
        memcpy(buffers_[index].start, frame->data(), n);
    }
    buf->bytesused = (unsigned)n;
    buf->field = V4L2_FIELD_NONE;
}
