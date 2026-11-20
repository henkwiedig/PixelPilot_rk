#ifndef V4L2_FRAME_PREP_H
#define V4L2_FRAME_PREP_H

// What the V4L2 re-encoder records, made from a decoded frame on the Artosyn
// AR9311's engines (no GPU there):
//  - the OSD: the frame is copied (dw-axi-dmac via /dev/ml-dmablit, else the
//    GE2D itself) and the GE2D (/dev/ge2d) blends the OSD plane's front buffer
//    onto the copy, only where the OSD shows something (osd_layer.h);
//  - 720p: the frame (or that copy) is scaled down on the scaler
//    (/dev/arscaler).
// The decoder's frame is never written: it may be on screen.

#include <stddef.h>
#include <stdint.h>

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

class FramePrep {
public:
    FramePrep() = default;
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
    bool want_osd(const V4l2Decoder::Frame &f);
    bool open_dev(int &fd, bool &missing, const char *path, bool required);
    bool ensure_pool(Pool &pool, unsigned count, size_t size);
    void free_pool(Pool &pool);
    bool burn(int dst_fd, const V4l2Decoder::Frame &f, uint64_t &copy_us, uint64_t &blend_us);
    bool scale(int src_fd, const V4l2Decoder::Frame &f, int dst_fd, const EncFrame &geom);

    int ge2d_fd_ = -1, blit_fd_ = -1, scaler_fd_ = -1, heap_fd_ = -1;
    bool ge2d_missing_ = false, blit_missing_ = false, scaler_missing_ = false, heap_missing_ = false;
    Pool out_;       // what the encoder reads, one per encoder slot
    Pool staging_;   // the OSD copy at the decoded size, before the scaler
    std::vector<ge2d_blit> blits_;
    OsdLayerSnapshot layer_;

    // every 5 s at debug level
    uint64_t stat_start_ms_ = 0, stat_frames_ = 0, stat_burnt_ = 0, stat_regions_ = 0;
    uint64_t stat_copy_us_ = 0, stat_blend_us_ = 0, stat_scale_us_ = 0;
};

#endif // V4L2_FRAME_PREP_H
