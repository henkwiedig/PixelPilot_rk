#include "timing_sei.h"

#include <string.h>

#define SEI_PREFIX_NAL_TYPE 39
#define SEI_USER_DATA_UNREGISTERED 5
#define TIMING_SEI_PAYLOAD_SIZE (16 + (int)sizeof(struct timing_sei_v1))

static const uint8_t k_uuid[16] = TIMING_SEI_UUID;

/* Offset of the next start code at or after from (len if none), and its
 * length in *sc_len. */
static uint32_t find_start_code(const uint8_t *buf, uint32_t len, uint32_t from, uint32_t *sc_len)
{
    for (uint32_t i = from; i + 2 < len; i++) {
        if (buf[i] == 0 && buf[i + 1] == 0) {
            if (buf[i + 2] == 1) {
                *sc_len = 3;
                return i;
            }
            if (i + 3 < len && buf[i + 2] == 0 && buf[i + 3] == 1) {
                *sc_len = 4;
                return i;
            }
        }
    }
    *sc_len = 0;
    return len;
}

int timing_sei_parse(const uint8_t *au, uint32_t len, struct timing_sei_v1 *out)
{
    uint32_t sc_len;
    uint32_t pos = find_start_code(au, len, 0, &sc_len);
    while (pos < len) {
        uint32_t nal = pos + sc_len;
        if (nal + 2 >= len)
            return 0;
        uint8_t type = (au[nal] >> 1) & 0x3f;
        if (type <= 31)
            return 0; /* first slice: a prefix SEI can only come before it */
        uint32_t next_sc;
        uint32_t next = find_start_code(au, len, nal, &next_sc);
        if (type == SEI_PREFIX_NAL_TYPE) {
            /* Undo emulation prevention, only as far as our message goes. */
            uint8_t rbsp[2 + TIMING_SEI_PAYLOAD_SIZE];
            uint32_t r = 0;
            int zeros = 0;
            for (uint32_t i = nal + 2; i < next && r < sizeof(rbsp); i++) {
                if (zeros >= 2 && au[i] == 3) {
                    zeros = 0;
                    continue;
                }
                rbsp[r++] = au[i];
                zeros = au[i] == 0 ? zeros + 1 : 0;
            }
            if (r == sizeof(rbsp) && rbsp[0] == SEI_USER_DATA_UNREGISTERED &&
                rbsp[1] >= TIMING_SEI_PAYLOAD_SIZE && memcmp(rbsp + 2, k_uuid, sizeof(k_uuid)) == 0 &&
                rbsp[2 + 16] == TIMING_SEI_VERSION) {
                memcpy(out, rbsp + 2 + 16, sizeof(*out));
                return 1;
            }
        }
        pos = next;
        sc_len = next_sc;
    }
    return 0;
}
