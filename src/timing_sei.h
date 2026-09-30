#ifndef PIXELPILOT_TIMING_SEI_H
#define PIXELPILOT_TIMING_SEI_H

/*
 * Per-frame timing that ar8030-transport-rx puts into the H.265 stream:
 * one prefix SEI (user_data_unregistered, our UUID) ahead of each access
 * unit's first slice. Every time in it is on this machine's
 * CLOCK_MONOTONIC in microseconds -- rx has already mapped the air unit's
 * capture time onto the ground clock -- so it compares directly against
 * get_time_us() and against DRM's page-flip timestamps.
 *
 * Canonical definition: ar8030-transport common/timing_sei.h. Keep the
 * UUID and the struct layout identical to it.
 */

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#define TIMING_SEI_VERSION 1
#define TIMING_SEI_UUID                                                                                 \
    {0x8a, 0x3f, 0x52, 0xc1, 0x6e, 0x0b, 0x4d, 0x97, 0xb2, 0x14, 0x5a, 0xe3, 0x09, 0x7c, 0xd1, 0x46}

#define TIMING_SEI_FLAG_SYNC_VALID 0x01 /* capture_ground_us is on the ground clock */
#define TIMING_SEI_FLAG_ENCODE 0x02     /* encode_us is known */

#pragma pack(push, 1)
struct timing_sei_v1 { /* little endian */
    uint8_t version;
    uint8_t flags;
    uint16_t frame_seq;           /* the air's video frame counter */
    uint64_t capture_ground_us;   /* sensor capture (encoder pts) */
    uint64_t rx_done_ground_us;   /* frame reassembled by ar8030-transport-rx */
    uint32_t sync_uncertainty_us; /* +- bound of the air -> ground clock mapping */
    uint32_t encode_us;           /* recent average capture -> encode done */
    uint32_t frames_lost;         /* lifetime frames that never reached rx whole */
};
#pragma pack(pop)

/* Looks for the timing SEI in an H.265 Annex-B access unit, scanning only
 * up to the first slice (it is a prefix SEI). Returns 1 and fills *out if
 * found, 0 otherwise. */
int timing_sei_parse(const uint8_t *au, uint32_t len, struct timing_sei_v1 *out);

#ifdef __cplusplus
}
#endif

#endif
