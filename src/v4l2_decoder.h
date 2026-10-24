#ifndef V4L2_DECODER_H
#define V4L2_DECODER_H

// Stateful V4L2 memory-to-memory video decoder (wave5 on the Artosyn AR9311)
// whose CAPTURE buffers go straight to the display: each one is exported as a
// dma-buf and wrapped in a DRM framebuffer once, when it is allocated, so a
// decoded frame is flipped to without a copy.
//
// The decoder never gets a buffer back while the display may still read it.
// A decoded frame stays held until a newer frame has been flipped to and the
// one after that too (shown()): the flip event can arrive a refresh before
// the display controller latches the new address, so the frame before the
// newest one may still be on the glass. Frames that were never shown are
// returned as soon as a newer one has been. Other readers (the DVR
// re-encoder) take a reference of their own with ref()/unref().

#include <stdint.h>
#include <stddef.h>

#include <mutex>
#include <vector>

#include "gstrtpreceiver.h"

class V4l2Decoder {
public:
    struct Frame {
        int      index = -1;      // CAPTURE buffer index, for ref()/unref()/shown()
        uint32_t fb_id = 0;       // DRM framebuffer
        int      dmabuf_fd = -1;  // the buffer as a dma-buf (owned by the decoder)
        size_t   dmabuf_size = 0;
        uint64_t pts_us = 0;      // the timestamp fed with the access unit
        bool     error = false;   // the decoder flagged the picture as corrupt
        uint32_t width = 0;       // visible picture
        uint32_t height = 0;
        uint32_t stride = 0;      // luma bytes per line
        uint32_t plane_height = 0;// luma rows in the buffer (chroma follows them)
        uint32_t fourcc = 0;      // DRM_FORMAT_*
    };

    enum class Result {
        Frame,          // *out is a decoded frame, held for the caller
        FormatChanged,  // new CAPTURE buffers: dimensions in format()
        Eos,            // the drain after send_eos() is complete
        Timeout,
        Error,
    };

    explicit V4l2Decoder(int drm_fd);
    ~V4l2Decoder();

    // Finds a decoder device for the codec (or uses `device`) and starts its
    // OUTPUT (bitstream) side. CAPTURE is set up at the first source change.
    bool open(VideoCodec codec, const char *device = nullptr);
    void close();
    bool is_open() const { return fd_ >= 0; }
    VideoCodec codec() const { return codec_; }

    // One access unit (split over several bitstream buffers if it is bigger
    // than one). Waits up to timeout_ms for a free buffer; false if none
    // came free (the decoder is stalled) or the device failed.
    bool feed(const uint8_t *data, size_t size, uint64_t pts_us, int timeout_ms);
    // Starts the drain: frames still inside come out, then Result::Eos.
    void send_eos();

    // Waits up to timeout_ms for the next decoded frame or event.
    Result next(Frame *out, int timeout_ms);
    // Geometry of the current CAPTURE buffers (after FormatChanged).
    const Frame &format() const { return format_; }

    // After Result::FormatChanged, once nothing outside holds a frame any
    // more (the display may still show one): frees the old CAPTURE buffers
    // and allocates new ones for the new format.
    bool reconfigure();

    // Display bookkeeping, see the top of this file. Thread-safe.
    void shown(uint32_t fb_id);
    // A frame from next() that will not be displayed.
    void release(int index);
    void ref(int index);
    void unref(int index);

private:
    struct Capture {
        uint32_t fb_id = 0;
        uint32_t gem_handle = 0;
        int      dmabuf_fd = -1;
        size_t   size = 0;
        bool     queued = false;   // owned by the driver
        bool     display = false;  // the display (or a pending flip) may read it
        int      refs = 0;         // other readers
        uint64_t seq = 0;          // dequeue order
    };

    bool find_device(VideoCodec codec, const char *device);
    bool setup_output(VideoCodec codec);
    bool setup_capture();
    void free_capture();
    bool queue_capture(int index);           // with mtx_ held
    void maybe_requeue(int index);           // with mtx_ held
    bool dequeue_event(bool *source_change, bool *eos);
    bool get_output_buffer(int *index, int timeout_ms);

    const int drm_fd_;
    int fd_ = -1;
    VideoCodec codec_ = VideoCodec::UNKNOWN;
    uint32_t out_type_ = 0, cap_type_ = 0;

    struct Output {
        void  *map = nullptr;
        size_t size = 0;
        bool   queued = false;
    };
    std::vector<Output> outputs_;
    std::mutex out_mtx_;

    std::vector<Capture> captures_;
    std::mutex mtx_;              // captures_ and the display bookkeeping
    bool cap_streaming_ = false;
    bool draining_ = false;
    bool reconfig_pending_ = false;
    uint64_t next_seq_ = 1;
    int shown_last_ = -1;         // newest shown capture index
    int shown_prev_ = -1;         // the one before it
    Frame format_;
};

#endif // V4L2_DECODER_H
