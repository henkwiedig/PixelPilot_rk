#ifndef V4L2_FRAME_PREP_H
#define V4L2_FRAME_PREP_H

// What the V4L2 re-encoder records, made from a decoded frame on the Artosyn
// AR9311's engines (no GPU there):
//  - the OSD: the frame is copied (dw-axi-dmac via /dev/ml-dmablit, else the
//    GE2D itself) and the GE2D (/dev/ge2d) blends the OSD plane's front buffer
//    onto the copy, only where the OSD shows something (osd_layer.h);
//  - 720p: the frame (or that copy) is scaled down on the scaler
//    (/dev/arscaler);
//  - the OSD at the recording's size but not the video's (a 720p screen, a
//    720p recording of a 1080p camera): the frame is scaled first and the OSD
//    blended onto that, 1:1;
//  - an OSD that matches neither (a recording bigger than the screen, as on
//    a SinYine F1 tester's: a 720p HDMI screen, a 1080p recording): OsdUpscaler scales
//    the OSD to the recording's size, and that is blended 1:1.
// The recording's size is the DVR setting's, never the screen's.
// The decoder's frame is never written: it may be on screen.

#include <stddef.h>
#include <stdint.h>

#include <condition_variable>
#include <mutex>
#include <thread>
#include <vector>

#include "artosyn_ge2d.h"
#include "encoder_params.h"
#include "osd_layer.h"
#include "v4l2_decoder.h"

// An I420 frame as the encoder gets it: one dma-buf, chroma planes after
// plane_height luma rows.
struct EncFrame {
    int fd = -1;
    size_t size = 0;
    uint32_t width = 0, height = 0;   // visible
    uint32_t stride = 0;              // luma bytes per line
    uint32_t plane_height = 0;        // luma rows in the buffer
};

// The OSD scaled to another size on the scaler (/dev/arscaler, 4 bytes a
// pixel: it takes ARGB8888, interpolating each byte on its own) whenever it
// changed, at most every kIntervalMs, on a thread of its own: a 720p -> 1080p
// OSD takes ~24 ms of the scaler. Its buffers come from the codec pool (mmz,
// a carve-out outside the system's RAM, ~15 MB free on a VR04 while it
// re-encodes 1080p60): an ARGB8888 copy of the OSD (an ARGB4444 plane has
// no other) and the scaled OSD, 3.7 + 8.3 MB for a 720p screen and a 1080p
// recording. One scaled buffer, not two: the "reserved" heap is CMA, its
// pages come out of the 128 MB of RAM, and a second buffer there had the
// kernel kill PixelPilot. So a recorded frame may get a scale in progress --
// the old OSD and the new one in parts, for one frame, when the OSD changes.
class OsdUpscaler {
public:
    ~OsdUpscaler() { stop(); }
    // Scales the top-left src_w x src_h of the OSD (all of it, but for the
    // PIXELPILOT_TEST_OSD_SIZE crop) to dst_w x dst_h; layer_w x layer_h is
    // the OSD buffer, copy: register an ARGB8888 copy (an ARGB4444 plane).
    // Restarts when any of it changed. False: no scaler or no memory.
    bool run(uint32_t src_w, uint32_t src_h, uint32_t dst_w, uint32_t dst_h, uint32_t layer_w, uint32_t layer_h,
             bool copy);
    void stop();
    bool running() const { return thread_.joinable(); }
    // The newest scaled OSD (ARGB8888, dst_w x dst_h); false: none yet.
    bool latest(int &fd, uint32_t &stride, std::vector<OsdRegion> &regions);

private:
    static const uint64_t kIntervalMs = 100;
    struct Buf {
        int fd = -1;
        uint8_t *map = nullptr;
        size_t len = 0;
    };
    bool alloc(Buf &b, size_t len);
    void free_buf(Buf &b);
    void loop();
    bool scale_once(const OsdLayerSnapshot &s);

    uint32_t src_w_ = 0, src_h_ = 0, dst_w_ = 0, dst_h_ = 0, layer_w_ = 0, layer_h_ = 0;
    bool copy_wanted_ = false;
    Buf copy_, out_;
    uint32_t copy_stride_ = 0, out_stride_ = 0;
    int scaler_fd_ = -1;
    std::thread thread_;
    std::mutex mtx_;              // have_, regions_, stop_
    std::condition_variable cv_;
    bool stop_ = false;
    bool have_ = false;           // out_ holds a scaled OSD
    std::vector<OsdRegion> regions_;
};

class FramePrep {
public:
    FramePrep();
    ~FramePrep();

    // The frames the encoder gets for decoded frames like `f` at the
    // resolution setting: f's own geometry, or scaled to 720 lines (when
    // there is a scaler).
    EncFrame geometry(const V4l2Decoder::Frame &f, EncResolution res);

    // The frame to encode in encoder slot `slot` (of `slots`), with
    // `geom`'s geometry: the decoder's own (own = false), or one of ours
    // with the OSD on it and/or scaled (own = true). False: none, skip it
    // (the scaler failed).
    bool prepare(unsigned slot, unsigned slots, const V4l2Decoder::Frame &f, const EncFrame &geom,
                 bool osd, EncFrame &out, bool &own);

    // Frees our frames and lets go of every buffer the engines hold.
    void release();

private:
    struct Pool {
        std::vector<int> fds;
        size_t size = 0;
    };
    // Where the OSD goes on: not at all, onto a copy at the decoded size
    // (before any scaling), or onto our scaled frame.
    enum class OsdAt { None, Decoded, Output };
    OsdAt want_osd(const V4l2Decoder::Frame &f, const EncFrame &geom, unsigned slots);
    // The OSD as it is now into layer_, its size (the screen's) into
    // osd_w_/osd_h_; false: no OSD.
    bool snapshot_osd();
    bool open_dev(int &fd, bool &missing, const char *path, bool required);
    bool ensure_pool(Pool &pool, unsigned count, size_t size);
    void free_pool(Pool &pool);
    // The OSD the blits read: the screen's (layer_) or the upscaled one.
    struct OsdSrc {
        int fd = -1;
        uint32_t format = GE2D_FMT_ARGB8888;
        uint32_t width = 0, height = 0, stride = 0;
        uint32_t clip_w = 0, clip_h = 0;   // what of it counts
        const std::vector<OsdRegion> *regions = nullptr;
    };
    void add_osd_blits(const OsdSrc &src);
    bool run_ge2d(const ge2d_frame &dst);
    bool burn(int dst_fd, const V4l2Decoder::Frame &f, uint64_t &copy_us, uint64_t &blend_us);
    bool blend(int dst_fd, const EncFrame &geom, uint64_t &blend_us);
    bool scale(int src_fd, const V4l2Decoder::Frame &f, int dst_fd, const EncFrame &geom);

    int ge2d_fd_ = -1, blit_fd_ = -1, scaler_fd_ = -1, heap_fd_ = -1;
    bool ge2d_missing_ = false, blit_missing_ = false, scaler_missing_ = false, heap_missing_ = false;
    Pool out_;       // what the encoder reads, one per encoder slot
    Pool staging_;   // the OSD copy at the decoded size, before the scaler
    std::vector<ge2d_blit> blits_;
    OsdLayerSnapshot layer_;
    uint32_t osd_w_ = 0, osd_h_ = 0;            // snapshot_osd()
    OsdSrc osd_src_;                            // want_osd()'s
    OsdUpscaler upscaler_;
    int up_fd_ = -1;
    uint32_t up_stride_ = 0;
    std::vector<OsdRegion> up_regions_;
    uint32_t geom_w_ = 0, geom_h_ = 0;          // the last geometry(), for its log line
    // Bench test only, PIXELPILOT_TEST_OSD_SIZE=<w>x<h>: the OSD counts as
    // that size, its top-left part (a screen smaller than the video on a
    // goggle whose panel has one mode).
    uint32_t test_osd_w_ = 0, test_osd_h_ = 0;

    // every 5 s at debug level
    uint64_t stat_start_ms_ = 0, stat_frames_ = 0, stat_burnt_ = 0, stat_regions_ = 0;
    uint64_t stat_copy_us_ = 0, stat_blend_us_ = 0, stat_scale_us_ = 0;
};

#endif // V4L2_FRAME_PREP_H
