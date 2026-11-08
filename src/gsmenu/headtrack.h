#pragma once

#include <stdbool.h>

#ifdef __cplusplus
extern "C" {
#endif

/* Head tracking on boards that ship /usr/bin/gs-headtrack (the VRX Pro's
 * IMU -> gimbal on the air unit, sbc-groundstations). gsmenu.sh turns it on
 * and off (get/set gs system headtrack); these two talk to the running
 * tracker directly. */

/* gs-headtrack is installed. */
bool headtrack_available(void);

/* Take the current head attitude as the gimbal's centre: SIGUSR1 to the
 * running gs-headtrack, found by its pid file. False when it isn't running. */
bool headtrack_recentre(void);

#ifdef __cplusplus
}
#endif
