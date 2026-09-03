// input.h
#ifndef INPUT_H
#define INPUT_H

#include <stdint.h>
#include "lvgl/lvgl.h"

typedef enum {
    GSMENU_CONTROL_MODE_NAV = 0,
    GSMENU_CONTROL_MODE_EDIT,
    GSMENU_CONTROL_MODE_SLIDER,
    GSMENU_CONTROL_MODE_KEYBOARD
} gsmenu_control_mode_t;


#ifdef __cplusplus
extern "C" {
#endif

void simulate_key_press(uint32_t key_code);

// Shared control-mode -> LV key dispatch, used by every physical input
// backend (GPIO buttons, VRX Pro ADC buttons, ...) so they all drive the
// same navigation semantics instead of each reimplementing the mapping.
// Sets the pending key state; the caller is responsible for its own
// backend-specific logging.
void dispatch_named_key_event(const char *name);

// Shared long-press behavior for the (currently two) buttons that have
// distinct long-press semantics: "right" acts as confirm, "left" toggles
// recording.
void dispatch_named_long_press(const char *name);

// True if the most recent dispatch_named_key_event() call (or any earlier
// one still pending release) mapped to an actual LVGL key. Backends use
// this only to decide whether to log a press -- next_key isn't reset until
// the following release, so this reflects "there's a pending key", not
// strictly "the last call set one".
bool dispatch_has_pending_key(void);

// Marks the pending key as released, for backends (like the ADC poller)
// that detect "no button is currently active" directly rather than through
// a hardware release edge.
void dispatch_release_key(void);

// Function to make stdin non-blocking
void set_stdin_nonblock(void);
// Function to restore terminal settings
void restore_stdin(void);

// Handle WASD input and convert to LVGL key codes
void handle_keyboard_input(void);

void toggle_rec_enabled(void);

// Custom function to simulate keyboard input
static void virtual_keyboard_read(lv_indev_t * indev, lv_indev_data_t * data);

// Function to create the virtual keyboard
lv_indev_t * create_virtual_keyboard();

void cleanup_gpio(void);

#ifdef __cplusplus
}
#endif
#endif // INPUT_H