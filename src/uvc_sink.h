#ifndef UVC_SINK_H
#define UVC_SINK_H

#include <stdint.h>
#include <atomic>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

// ---------------------------------------------------------------------------
// UvcSink: exposes a UVC (USB Video Class) webcam to a USB host.
//
// Opens the f_uvc gadget V4L2 output device (created by the webcam-gadget
// script), performs the UVC PROBE/COMMIT streaming negotiation, and streams
// MJPEG frames pushed via submit_frame() to the host. The advertised MJPEG
// format/frame set MUST match the descriptors the webcam-gadget script writes
// into configfs.
//
// Threading: start() spawns one worker thread that owns the V4L2 fd, runs the
// event/streaming poll loop, and never blocks on the producer. submit_frame()
// is called from the encoder callback (any thread) and only updates a
// mutex-guarded latest-frame slot (latest-wins, matching the low-latency
// intent of FrameProcessor).
// ---------------------------------------------------------------------------
class UvcSink {
public:
    struct FrameSize { uint32_t width; uint32_t height; uint32_t fps; };

    // frames  : supported MJPEG resolutions; frames[0] is the default/preferred.
    // device  : explicit /dev/videoN, or empty to auto-discover the gadget node.
    explicit UvcSink(std::vector<FrameSize> frames, std::string device = "");
    ~UvcSink();

    bool start();   // open device, subscribe to UVC events, spawn worker thread
    void stop();    // stop streaming, join worker, close device

    // Store the latest JPEG for delivery to the host (thread-safe, latest-wins).
    void submit_frame(std::shared_ptr<std::vector<uint8_t>> jpeg);

    bool is_streaming() const { return streaming_.load(std::memory_order_relaxed); }

    // dwMaxPayloadTransferSize advertised during negotiation. Must match the
    // gadget's streaming_maxpacket. Optional tuning knob; sane default set.
    void set_max_payload(uint32_t bytes) { max_payload_ = bytes; }

private:
    struct MappedBuf { void *start = nullptr; size_t length = 0; };

    static void *__THREAD__(void *self);
    void loop();

    // UVC event handling (worker thread only).
    void process_events();
    void handle_setup(const void *ctrl_ptr, void *resp_ptr);
    void handle_streaming_control(uint8_t req, void *target, void *resp_ptr);
    void handle_data(const void *data_ptr);
    void fill_streaming_control(void *sc_ptr, int iframe, uint32_t interval);

    // Streaming lifecycle (worker thread only).
    void start_streaming();
    void stop_streaming();
    void process_video();          // one DQBUF → refill → QBUF cycle
    void fill_buffer(unsigned index, void *v4l2_buf_ptr);

    static std::string discover_device();

    std::vector<FrameSize> frames_;
    std::string device_path_;
    int fd_ = -1;

    std::thread worker_;
    std::atomic<bool> running_{false};
    std::atomic<bool> streaming_{false};

    uint32_t max_payload_ = 3072;  // must match gadget streaming_maxpacket

    // Latest JPEG from the encoder (producer) → worker (consumer).
    std::mutex frame_mtx_;
    std::shared_ptr<std::vector<uint8_t>> latest_frame_;

    // Negotiation state (worker thread only). Stored as byte blobs to keep the
    // kernel UAPI struct out of this header.
    std::vector<uint8_t> probe_;   // sizeof(struct uvc_streaming_control)
    std::vector<uint8_t> commit_;
    int  control_pending_ = 0;      // 0=none, 1=probe, 2=commit (for DATA phase)
    int  current_frame_index_ = 1;  // 1-based index into frames_ (from COMMIT)

    std::vector<MappedBuf> buffers_;
};

#endif // UVC_SINK_H
