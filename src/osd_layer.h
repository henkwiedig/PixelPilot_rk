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
};

// A copy of the current state; false while the OSD has none yet.
bool osd_layer_snapshot(OsdLayerSnapshot &out);

#endif // OSD_LAYER_H
