#pragma once
#include <stdint.h>

typedef struct sbuf_s {
    uint8_t *ptr;          // data pointer must be first (sbuf_t* is equivalent to uint8_t **)
    uint8_t *end;
} sbuf_t;

#define MSP_V2_FRAME_ID         255

typedef enum {
    MSP_V1          = 0,
    MSP_V2_OVER_V1  = 1,
    MSP_V2_NATIVE   = 2,
    MSP_VERSION_COUNT
} mspVersion_e;

#define MSP_VERSION_MAGIC_INITIALIZER { 'M', 'M', 'X' }

// return positive for ACK, negative on error, zero for no reply
typedef enum {
    MSP_RESULT_ACK = 1,
    MSP_RESULT_ERROR = -1,
    MSP_RESULT_NO_REPLY = 0,
    MSP_RESULT_CMD_UNKNOWN = -2,   // don't know how to process command, try next handler
} mspResult_e;

typedef enum {
    MSP_DIRECTION_REPLY = 0,
    MSP_DIRECTION_REQUEST = 1
} mspDirection_e;

typedef struct mspPacket_s {
    sbuf_t buf;         // payload only w/o header or crc
    int16_t cmd;
    int16_t result;
    uint8_t flags;      // MSPv2 flags byte. It looks like unused (yet?).
    uint8_t direction;  // It also looks like unused and might be deleted.
} mspPacket_t;

typedef enum {
    MSP_ERR_NONE,
    MSP_ERR_HDR,
    MSP_ERR_LEN,
    MSP_ERR_CKS
} msp_error_e;

typedef enum {
    MSP_IDLE,
    MSP_VERSION,
    MSP_DIRECTION,
    MSP_SIZE,
    MSP_CMD,
    MSP_PAYLOAD,
    MSP_CHECKSUM,
} msp_state_machine_e;

typedef enum {
    MSP_INBOUND,
    MSP_OUTBOUND
} msp_direction_e;

typedef struct msp_msg_s {
    uint8_t checksum;
    uint8_t cmd;
    uint8_t size;
    msp_direction_e direction;
    uint8_t payload[256];
} msp_msg_t;

typedef void (*msp_msg_callback)(msp_msg_t *);

typedef struct msp_state_s {
    msp_msg_callback cb;
    msp_state_machine_e state;
    uint8_t buf_ptr;
    msp_msg_t message;
} msp_state_t;