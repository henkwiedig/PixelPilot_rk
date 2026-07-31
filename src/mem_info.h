#pragma once
/**
 * mem_info.h
 *
 * Gate for the DVR-reencode CONTIG/CMA buffer workaround (see
 * frame_processor.cpp and frame_colorcorrect.cpp): RGA2's MMU can only
 * address physical memory below 4GB, and a kernel bug in the Rockchip GEM
 * allocator (the __GFP_DMA32 safety flag is only ever applied under
 * CONFIG_ARM_LPAE, a 32-bit-only Kconfig symbol never set on this arm64
 * kernel) means a buffer can silently land above that boundary on board
 * variants with >=4GB RAM. On the 1GB variant no buffer can ever
 * physically be placed above 4GB, so forcing CMA allocation there would
 * only add memory pressure for zero benefit -- gate on actual installed
 * RAM rather than a compile-time board flag so one binary is correct on
 * both variants.
 */

#include <cstdio>

inline bool platform_has_large_ram() {
    FILE *f = fopen("/proc/meminfo", "r");
    if (!f) return false;
    char line[256];
    unsigned long kb = 0;
    bool found = false;
    while (fgets(line, sizeof(line), f)) {
        if (sscanf(line, "MemTotal: %lu kB", &kb) == 1) { found = true; break; }
    }
    fclose(f);
    // Wide margin between the ~1GB and ~4GB board variants -- this only
    // needs to land on the right side, not find a precise boundary.
    return found && kb > 2ul * 1024 * 1024;
}
