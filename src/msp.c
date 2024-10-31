// You will need this in /etc/wifibroadcast.cfg
// [gs]
// streams = [{'name': 'video',   'stream_rx': 0x00, 'stream_tx': None, 'service_type': 'udp_direct_rx',  'profiles': ['base', 'gs_base', 'video', 'gs_video']},
//            {'name': 'mavlink', 'stream_rx': 0x10, 'stream_tx': 0x90, 'service_type': 'mavlink',        'profiles': ['base', 'gs_base', 'mavlink', 'gs_mavlink']},
//            {'name': 'tunnel',  'stream_rx': 0x20, 'stream_tx': 0xa0, 'service_type': 'tunnel',         'profiles': ['base', 'gs_base', 'tunnel', 'gs_tunnel']},
//            {'name': 'msp',     'stream_rx': 0x30, 'stream_tx': 0xb0, 'service_type': 'udp_proxy',      'profiles': ['base', 'gs_base', 'gs_msp']}
//            ]
// [gs_msp]
// peer = 'connect://127.0.0.1:14551'  # outgoing connection
// frame_type = 'data'  # Use data or rts frames
// fec_k = 1            # FEC K (For tx side. Rx will get FEC settings from session packet)
// fec_n = 2            # FEC N (For tx side. Rx will get FEC settings from session packet)
// fec_timeout = 0      # [ms], 0 to disable. If no new packets during timeout, emit one empty packet if FEC block is open
// fec_delay = 0        # [us], 0 to disable. Issue FEC packets with delay between them.

// this on drone side
// wfb_tx -p 48 -u 14551 -K /etc/drone.key -M 3 -S 1 -L 1 -k 8 -n 12 -i 7669206 wlan0
#define _GNU_SOURCE
#include <sys/prctl.h>
#include <errno.h>
#include <fcntl.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <unistd.h>
#include <pthread.h>
#include <ctype.h>  // for isprint
#include <sys/select.h>
#include <unistd.h>

#include <arpa/inet.h>
#include <netinet/in.h>
#include <pthread.h>
#include <sys/socket.h>
#include <sys/uio.h>

#include <sys/prctl.h>

#include "msp/msp.h"
#include "msp/msp_protocol.h"
#include "msp/msp_displayport.h"

int msp_port = 14551;
int msp_thread_signal = 0;
msp_state_t *rx_msp_state;

#define ROWS 18       // Number of rows in the OSD
#define COLUMNS 50    // Number of columns in the OSD
#define MAX_MSP_STRING_LENGTH 30 // ... NULL terminated string of up to 30 characters in length ...
char osd[ROWS][COLUMNS];
time_t last_keepalive_time = 0; // Last time a keepalive was received


static void process_draw_string(uint8_t *payload) {
    uint8_t row = payload[0];
    uint8_t col = payload[1];
    uint8_t attrs = payload[2]; // INAV and Betaflight use this to specify a higher page number. 

    // Find the actual length of the payload string, stopping at null or MAX_DISPLAY_LENGTH
    uint8_t actual_length = 0;
    for (uint8_t idx = 0; idx < MAX_MSP_STRING_LENGTH && payload[3 + idx] != '\0'; idx++) {
        actual_length++;
    }

    for(uint8_t idx = 0; idx < actual_length; idx++) { //MSP Disployport message is max 30
        uint16_t character = payload[3 + idx];
        if(attrs & 0x3) {
            // shift over by the page number if they were specified
            character |= ((attrs & 0x3) * 0x100);
        }
        osd[row-1][col + idx -1 ]=character;
        //printf("%i %i\n",row,col);
    }
}

int displayport_process_message(msp_msg_t *msg) {
    if (msg->direction != MSP_INBOUND) {
        return 1;
    }
    if (msg->cmd != MSP_DISPLAYPORT) {
        return 1;
    }
    msp_displayport_cmd_e sub_cmd = msg->payload[0];
    switch(sub_cmd) {
        case MSP_DISPLAYPORT_KEEPALIVE: // 0 -> Open/Keep-Alive DisplayPort
            last_keepalive_time = time(NULL);
            break;
        case MSP_DISPLAYPORT_CLOSE: // 1 -> Close DisplayPort
            //printf("MSP_DISPLAYPORT_CLOSE\n");
            break;
        case MSP_DISPLAYPORT_CLEAR: // 2 -> Clear Screen
            // Initialize the array with empty spaces or some default character
            for (int i = 0; i < ROWS; i++) {
                for (int j = 0; j < COLUMNS; j++) {
                    osd[i][j] = ' ';  // Initialize with a blank space
                }
            }
            break;
        case MSP_DISPLAYPORT_DRAW_STRING: // 3 -> Draw String
            process_draw_string(&msg->payload[1]);
            break;
        case MSP_DISPLAYPORT_DRAW_SCREEN: // 4 -> Draw Screen
            // Clear the console and move the cursor to the top-left corner
            printf("\033[H");  // ANSI escape code to move cursor to "home" position
            printf("\033[J");  // Clear the screen from cursor down
            for (int i = 0; i < ROWS; i++) {
                for (int j = 0; j < COLUMNS; j++) {
                    char ch = osd[i][j];
                    putchar(isprint((unsigned char)ch) ? ch : 'X');
                }
                putchar('\n');
            }
            break;
        case MSP_DISPLAYPORT_SET_OPTIONS: // 5 -> Set Options (HDZero/iNav)
            printf("MSP_DISPLAYPORT_SET_OPTIONS\n");
            break;
        default:
            break;
    }
    return 0;
}


static void rx_msp_callback(msp_msg_t *msp_message)
{
    switch(msp_message->cmd) {

        case MSP_STATUS: {
            printf("Received MSP_STATUS: %x\n", msp_message->payload);
            break;
        }

         case MSP_ATTITUDE: {
            printf("Received MSP_ATTITUDE: %x\n", msp_message->payload);
         }

        case MSP_COMP_GPS: {
            printf("Received MSP_COMP_GPS: %x\n", msp_message->payload);

        }
        case MSP_RC: {
            printf("Received MSP_RC: %x\n", msp_message->payload);

         }
        case MSP_DISPLAYPORT: {
            //printf("Received MSP_DISPLAYPORT: ");
            displayport_process_message(msp_message);
            break;
        }
        case MSP_FC_VARIANT: {
            printf("Received MSP_FC_VARIANT: %x\n", msp_message->payload);
            break;
        }
        case MSP_API_VERSION: {
            printf("Received MSP_API_VERSION: %x\n", msp_message->payload);
            break;
        }
        case MSP_VTX_CONFIG: {
            printf("Received MSP_VTX_CONFIG: %x\n", msp_message->payload);
            break;
        }
        default: {
            printf("Received a uncatched MSP_COMMAND: %i\n", msp_message->cmd);
            break;
        }
    }
}


msp_error_e msp_process_data(msp_state_t *msp_state, uint8_t dat)
{
    switch (msp_state->state)
    {
        default:
        case MSP_IDLE: // look for begin
            if (dat == '$')
            {
                msp_state->state = MSP_VERSION;
            }
            else
            {
                return MSP_ERR_HDR;
            }
            break;
        case MSP_VERSION: // Look for 'M' (MSP V1, we do not support V2 at this time)
            if (dat == 'M')
            {
                msp_state->state = MSP_DIRECTION;
            }
            else
            { // Got garbage instead, try again
                msp_state->state = MSP_IDLE;
                return MSP_ERR_HDR;
            }
            break;
        case MSP_DIRECTION: // < for command, > for reply
            msp_state->state = MSP_SIZE;
            switch (dat)
            {
            case '<':
                msp_state->message.direction = MSP_OUTBOUND;
                break;
            case '>':
                msp_state->message.direction = MSP_INBOUND;
                break;
            default: // garbage, try again
                msp_state->state = MSP_IDLE;
                return MSP_ERR_HDR;
                break;
            }
            break;
        case MSP_SIZE: // next up is supposed to be size
            msp_state->message.checksum = dat;
            msp_state->message.size = dat;
            msp_state->state = MSP_CMD;
            if (msp_state->message.size > 256)
            { // bogus message, too big. this can't actually happen but good to check
                msp_state->state = MSP_IDLE;
                return MSP_ERR_LEN;
                break;
            }
            break;
        case MSP_CMD: // followed by command
            msp_state->message.cmd = dat;
            msp_state->message.checksum ^= dat;
            msp_state->buf_ptr = 0;
            if (msp_state->message.size > 0)
            {
                msp_state->state = MSP_PAYLOAD;
            }
            else
            {
                msp_state->state = MSP_CHECKSUM;
            }
            break;
        case MSP_PAYLOAD: // if we had a payload, keep going
            msp_state->message.payload[msp_state->buf_ptr] = dat;
            msp_state->message.checksum ^= dat;
            msp_state->buf_ptr++;
            if (msp_state->buf_ptr == msp_state->message.size)
            {
                msp_state->buf_ptr = 0;
                msp_state->state = MSP_CHECKSUM;
            }
            break;
        case MSP_CHECKSUM:
            if (msp_state->message.checksum == dat)
            {
                if (msp_state->cb != 0){                
                    msp_state->cb(&msp_state->message);
                }
                memset(&msp_state->message, 0, sizeof(msp_msg_t));
                msp_state->state = MSP_IDLE;
                break;            
            }
            else
            {
                msp_state->state = MSP_IDLE;
                return MSP_ERR_CKS;
            }
            break;
    }
    //printf("msp_state->state: %i\n",msp_state->state);
    return MSP_ERR_NONE;
}


void* __MSP_THREAD__(void* arg) {
  pthread_setname_np(pthread_self(), "__MSP");
  printf("Starting msp thread...\n");

  rx_msp_state = calloc(1, sizeof(msp_state_t));
  rx_msp_state->cb = &rx_msp_callback;  

    // Initialize the array with empty spaces or some default character
    for (int i = 0; i < ROWS; i++) {
        for (int j = 0; j < COLUMNS; j++) {
            osd[i][j] = ' ';  // Initialize with a blank space
        }
    }


  // Create socket
  int fd = socket(AF_INET, SOCK_DGRAM, 0);
  if (fd < 0) {
    printf("ERROR: Unable to create MSP socket: %s\n", strerror(errno));
    return 0;
  }

  // Bind port
  struct sockaddr_in addr = {};
  memset(&addr, 0, sizeof(addr));
  addr.sin_family = AF_INET;
  inet_pton(AF_INET, "0.0.0.0", &(addr.sin_addr));
  addr.sin_port = htons(msp_port);

  if (bind(fd, (struct sockaddr*)(&addr), sizeof(addr)) != 0) {
    printf("ERROR: Unable to bind MSP port: %s\n", strerror(errno));
    return 0;
  }

   char buffer[2048];
    while (!msp_thread_signal) {
        fd_set read_fds;
        struct timeval timeout;
        timeout.tv_sec = 1;
        timeout.tv_usec = 0;

        FD_ZERO(&read_fds);
        FD_SET(fd, &read_fds);

        int ready = select(fd + 1, &read_fds, NULL, NULL, &timeout);
        if (ready < 0) {
            perror("select error");
            break;
        } else if (ready == 0) {
            // No data, continue loop after timeout
            continue;
        }

        // Data available, proceed with recv
        memset(buffer, 0x00, sizeof(buffer));
        int ret = recv(fd, buffer, sizeof(buffer), 0);
        if (ret < 0) {
            perror("recv error");
            break;
        } else if (ret == 0) {
            return 0;  // Peer has done an orderly shutdown
        }

        for (int i = 0; i < ret; i++) {
            msp_process_data(rx_msp_state, buffer[i]);
        }
    }

	printf("MSP thread done.\n");
  return 0;
}


