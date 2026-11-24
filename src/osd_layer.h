#ifndef OSD_LAYER_H
#define OSD_LAYER_H

// The OSD as the DVR re-encoder burns it in: the OSD plane's front buffer and
// the parts of it that hold anything visible. Kept by the OSD thread at every
// flush (osd_drm.cpp), read by the encoder at every recorded frame.

#include <stdint.h>
#include <vector>

struct OsdRegion {
    uint16_t x, y, w, h;
};

struct OsdLayerSnapshot {
    int prime_fd = -1;          // dma-buf of the buffer on screen; -1 = no OSD
    uint32_t drm_format = 0;    // DRM_FORMAT_ARGB4444 or DRM_FORMAT_ARGB8888
    uint32_t width = 0, height = 0;
    uint32_t stride = 0;        // bytes per row
    std::vector<OsdRegion> regions;   // what is not fully transparent
    uint64_t serial = 0;        // counts the flushes: a new value = the OSD changed
    // The OSD as ARGB8888 (for a scaler, which takes bytes, not 4-bit
    // channels): the front buffer itself on an ARGB8888 plane, else the copy
    // osd_layer_set_argb8888_copy() registered; -1 = none.
    int argb8888_fd = -1;
    uint32_t argb8888_stride = 0;
};

// A copy of the current state; false while the OSD has none yet.
bool osd_layer_snapshot(OsdLayerSnapshot &out);

// For a consumer that scales the OSD (the re-encoder recording at more than
// the screen's size): an ARGB8888 dma-buf for the OSD's top-left w x h (all
// of it but for a test crop), mapped at `map` (rows `stride` bytes apart),
// which the OSD thread keeps current from now on -- filled at once from what
// is on screen, then the changed areas at every flush. Only with an ARGB4444
// plane (an ARGB8888 one is its own copy). fd -1 unregisters it; after that
// the OSD thread no longer touches it.
void osd_layer_set_argb8888_copy(int fd, uint8_t *map, uint32_t stride, uint32_t w, uint32_t h);

#endif // OSD_LAYER_H
