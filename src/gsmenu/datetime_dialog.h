#ifndef GSMENU_DATETIME_DIALOG_H
#define GSMENU_DATETIME_DIALOG_H

#include <stdbool.h>
#include "../../lvgl/lvgl.h"

/* Modal joystick-driven date/time editor (local time). On Set it updates the
 * system clock and /dev/rtc0; on_done(changed) runs before it closes. */
void datetime_dialog_open(void (*on_done)(bool changed));

#endif
