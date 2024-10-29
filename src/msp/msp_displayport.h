#pragma once
#include <stdint.h>
#include <stdbool.h>
#include "msp.h"

typedef enum {
    MSP_DISPLAYPORT_KEEPALIVE,
    MSP_DISPLAYPORT_CLOSE,
    MSP_DISPLAYPORT_CLEAR,
    MSP_DISPLAYPORT_DRAW_STRING,
    MSP_DISPLAYPORT_DRAW_SCREEN,
    MSP_DISPLAYPORT_SET_OPTIONS,
    MSP_DISPLAYPORT_DRAW_SYSTEM
} msp_displayport_cmd_e;

typedef enum {
    MSP_SD_OPTION_30_16,
    MSP_HD_OPTION_50_18,
    MSP_HD_OPTION_30_16,
    MSP_HD_OPTION_60_22
} msp_hd_options_e;
