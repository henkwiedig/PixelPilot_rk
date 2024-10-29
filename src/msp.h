#ifndef MSP_H
#define MSP_H

#include "msp/msp.h"
#include "msp/msp_protocol.h"

extern int msp_port;
extern int msp_thread_signal;

void* __MSP_THREAD__(void* arg);

msp_error_e msp_process_data(msp_state_t *msp_state, uint8_t dat);

// size_t numOfChars(const char s[]);

// char* insertString(char s1[], const char s2[], size_t pos);

#endif