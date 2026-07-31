#pragma once
/**
 * rockchip_bo.h
 *
 * Rockchip vendor extension to struct drm_mode_create_dumb.flags (normally
 * "must be zero"), from include/uapi/drm/rockchip_drm.h -- not shipped in
 * this build's sysroot, so mirrored here. ROCKCHIP_BO_CONTIG requests
 * CMA-backed allocation, guaranteed to sit below the 4GB physical boundary
 * RGA2's MMU can address; see mem_info.h for why that matters.
 */

#include <cstdint>

static constexpr uint32_t ROCKCHIP_BO_CONTIG = 1u << 0;
