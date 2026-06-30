#include <fcntl.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <termios.h>
#include <unistd.h>
#include <gpiod.h>
#ifndef USE_SIMULATOR
#include <linux/input.h>
#include <sys/inotify.h>
#include <sys/ioctl.h>
#include <errno.h>
#endif
#include <dirent.h>
#include <string.h>
#include <time.h>
#include <yaml-cpp/yaml.h>
#include <glob.h>
#include "main.h"
#include "lvgl/lvgl.h"
#include "input.h"
#include "gsmenu/gs_system.h"

extern YAML::Node config;
extern lv_group_t *main_group;
extern lv_indev_t * indev_drv;


extern int dvr_enabled;

#ifdef USE_SIMULATOR
bool menu_active;
lv_timer_t * timer = NULL;
#endif
#ifndef USE_SIMULATOR
extern bool menu_active;
#define MAX_GPIO_BUTTONS 6  // Adjust based on your hardware
#define DEBOUNCE_DELAY_MS 50 // Debounce delay in milliseconds
#define INITIAL_REPEAT_DELAY_MS 500  // Time before repeat starts
#define REPEAT_RATE_MS 100           // Time between repeated events

// gpio_button_t structure
typedef struct {
    const char *name;        // Button name ("up", "down", etc.)
    int pin_number;          // Physical pin number from YAML
    const char *chip_name;   // GPIO chip path (e.g., "/dev/gpiochip0")
    const char *chip_label;  // GPIO chip label (e.g., "gpiochip3")
    int line_num;            // GPIO line number
    struct gpiod_chip *chip;
    struct gpiod_line *line;
    int last_state;
    long last_time;
    long repeat_time;
    bool is_holding;
    bool long_press_sent;
} gpio_button_t;

// Global array of GPIO buttons
gpio_button_t gpio_buttons[MAX_GPIO_BUTTONS] = {0};
#endif

extern lv_obj_t * pp_menu_screen;

// Global or static variable to store the next key state
static lv_key_t next_key = LV_KEY_END;  // Default to no key
static bool next_key_pressed = false;    // Indicates if the next key should be pressed or released
gsmenu_control_mode_t control_mode = GSMENU_CONTROL_MODE_NAV;

extern uint64_t gtotal_tunnel_data;
void simulate_traffic(lv_timer_t *t)
{
    gtotal_tunnel_data++;
}

#ifndef USE_SIMULATOR
// Function to find GPIO chip and line for a given pin number
bool find_gpio_mapping(int pin, const char** chip_name, int* line_num) {
    glob_t globbuf;
    struct gpiod_chip *chip = NULL;
    bool found = false;

    if (glob("/dev/gpiochip*", 0, NULL, &globbuf) != 0) {
        perror("Failed to find GPIO chips");
        return false;
    }

    for (size_t i = 0; i < globbuf.gl_pathc && !found; i++) {
        chip = gpiod_chip_open(globbuf.gl_pathv[i]);
        if (!chip) continue;

        // Check chip label first
        const char *label = gpiod_chip_label(chip);
        if (label) {
            // If we were looking for a specific chip label, we'd check here
        }

        // For libgpiod v1.x
        int num_lines = gpiod_chip_num_lines(chip);
        for (int offset = 0; offset < num_lines && !found; offset++) {
            struct gpiod_line *line = gpiod_chip_get_line(chip, offset);
            if (!line) continue;

            const char *name = gpiod_line_name(line);
            if (name) {
                int extracted_pin = 0;
                if (sscanf(name, "PIN_%d", &extracted_pin) == 1 || 
                    sscanf(name, "GPIO%d", &extracted_pin) == 1 ||
                    sscanf(name, "%d", &extracted_pin) == 1) {
                    if (extracted_pin == pin) {
                        *chip_name = strdup(globbuf.gl_pathv[i]);
                        *line_num = offset;
                        found = true;
                    }
                }
            }
            gpiod_line_release(line);
        }
        gpiod_chip_close(chip);
    }
    globfree(&globbuf);
    return found;
}

void init_button_from_config(YAML::Node& gpio_config, const char* button_name, int& button_index) {
    if (!gpio_config[button_name] || gpio_config[button_name].IsNull()) {
        printf("Omitting GPIO mapping for button %s\n", button_name);
        return;
    }

    gpio_buttons[button_index].name = button_name;

    // Check if the button config is a simple pin number or a map
    if (gpio_config[button_name].IsScalar()) {
        // Simple format: just a pin number
        gpio_buttons[button_index].pin_number = gpio_config[button_name].as<int>();
        if (!find_gpio_mapping(gpio_buttons[button_index].pin_number, 
                             &gpio_buttons[button_index].chip_name,
                             &gpio_buttons[button_index].line_num)) {
            fprintf(stderr, "Failed to find GPIO mapping for pin %d (%s)\n", 
                    gpio_buttons[button_index].pin_number, button_name);
            return;
        }
    } else {
        // Complex format: chip and pin specified
        YAML::Node button_config = gpio_config[button_name];
        if (!button_config["chip"] || !button_config["pin"]) {
            fprintf(stderr, "Invalid GPIO config for button %s - missing chip or pin\n", button_name);
            return;
        }

        gpio_buttons[button_index].chip_label = strdup(button_config["chip"].as<std::string>().c_str());
        gpio_buttons[button_index].pin_number = button_config["pin"].as<int>();
        
        // Construct chip path from label
        std::string chip_path = "/dev/" + std::string(gpio_buttons[button_index].chip_label);
        gpio_buttons[button_index].chip_name = strdup(chip_path.c_str());
        
        // For direct chip/pin mapping, we can use the pin number directly as line_num
        gpio_buttons[button_index].line_num = gpio_buttons[button_index].pin_number;
    }

    button_index++;
}

void init_gpio_buttons_from_config(YAML::Node& config) {
    YAML::Node gpio_config = config["gsmenu"]["gpio"];
    int button_index = 0;
    
    init_button_from_config(gpio_config, "up", button_index);
    init_button_from_config(gpio_config, "down", button_index);
    init_button_from_config(gpio_config, "left", button_index);
    init_button_from_config(gpio_config, "right", button_index);
    init_button_from_config(gpio_config, "center", button_index);
    init_button_from_config(gpio_config, "rec", button_index);
}

// Function to initialize GPIO buttons
void setup_gpio(YAML::Node& config) {
    // Initialize buttons from config first
    init_gpio_buttons_from_config(config);
    
    // Then setup the GPIO lines
    for (size_t i = 0; i < sizeof(gpio_buttons) / sizeof(gpio_buttons[0]); i++) {
        if (gpio_buttons[i].chip_name == NULL) continue;
        
        gpio_buttons[i].chip = gpiod_chip_open(gpio_buttons[i].chip_name);
        if (!gpio_buttons[i].chip) {
            perror("Failed to open GPIO chip");
            continue;
        }

        gpio_buttons[i].line = gpiod_chip_get_line(gpio_buttons[i].chip, gpio_buttons[i].line_num);
        if (!gpio_buttons[i].line) {
            perror("Failed to get GPIO line");
            gpiod_chip_close(gpio_buttons[i].chip);
            continue;
        }

        // Create the consumer name with "pixelpilot_" prefix
        char consumer_name[32];
        snprintf(consumer_name, sizeof(consumer_name), "pixelpilot_%s", gpio_buttons[i].name);

        if (gpiod_line_request_input(gpio_buttons[i].line, consumer_name) < 0) {
            perror("Failed to request GPIO input");
            gpiod_chip_close(gpio_buttons[i].chip);
            gpio_buttons[i].chip = NULL;
            gpio_buttons[i].line = NULL;
        }
    }
}

void send_long_press_event(size_t button_index) {
    if (strcmp(gpio_buttons[button_index].name, "right") == 0) {
        next_key = LV_KEY_ENTER;
        next_key_pressed = true;

        printf("GPIO Long Press: %s (acting as center) (Pin: %d, Chip: %s)\n",
               gpio_buttons[button_index].name,
               gpio_buttons[button_index].pin_number,
               gpio_buttons[button_index].chip_name);
    }
    else if (strcmp(gpio_buttons[button_index].name, "left") == 0 && !menu_active) {
        toggle_rec_enabled();

        printf("GPIO Long Press: %s (toggling recording) (Pin: %d, Chip: %s)\n",
               gpio_buttons[button_index].name,
               gpio_buttons[button_index].pin_number,
               gpio_buttons[button_index].chip_name);
    }
}

void send_button_event(size_t button_index) {
    if (gpio_buttons[button_index].name == NULL) return;

    // Adjust for control_mode
    switch (control_mode) {
        case GSMENU_CONTROL_MODE_NAV:
            if (strcmp(gpio_buttons[button_index].name, "up") == 0) {
                next_key = LV_KEY_PREV;
            } 
            else if (strcmp(gpio_buttons[button_index].name, "down") == 0) {
                next_key = LV_KEY_NEXT;
            }
            else if (strcmp(gpio_buttons[button_index].name, "left") == 0) {
                next_key = LV_KEY_HOME;
            }
            else if (strcmp(gpio_buttons[button_index].name, "right") == 0) {
                next_key = LV_KEY_ENTER;
            }
            else if (strcmp(gpio_buttons[button_index].name, "center") == 0) {
                next_key = LV_KEY_ENTER;
            }
            else if (strcmp(gpio_buttons[button_index].name, "rec") == 0) {
                #ifdef USE_SIMULATOR
                                dvr_enabled ^= 1;
                #endif
                            toggle_rec_enabled();
            }
            break;
            
        case GSMENU_CONTROL_MODE_EDIT:
            if (strcmp(gpio_buttons[button_index].name, "up") == 0) {
                next_key = LV_KEY_UP;
            } 
            else if (strcmp(gpio_buttons[button_index].name, "down") == 0) {
                next_key = LV_KEY_DOWN;
            }
            else if (strcmp(gpio_buttons[button_index].name, "left") == 0) {
                next_key = LV_KEY_ESC;
            }
            else if (strcmp(gpio_buttons[button_index].name, "right") == 0 ||
                     strcmp(gpio_buttons[button_index].name, "center") == 0) {
                next_key = LV_KEY_ENTER;
            }
            break;
            
        case GSMENU_CONTROL_MODE_SLIDER:
            if (strcmp(gpio_buttons[button_index].name, "up") == 0) {
                next_key = LV_KEY_RIGHT;
            } 
            else if (strcmp(gpio_buttons[button_index].name, "down") == 0) {
                next_key = LV_KEY_LEFT;
            }
            else if (strcmp(gpio_buttons[button_index].name, "left") == 0) {
                next_key = LV_KEY_ESC;
            }
            else if (strcmp(gpio_buttons[button_index].name, "right") == 0 ||
                     strcmp(gpio_buttons[button_index].name, "center") == 0) {
                next_key = LV_KEY_ENTER;
            }
            break;
            
        case GSMENU_CONTROL_MODE_KEYBOARD:
            if (strcmp(gpio_buttons[button_index].name, "up") == 0) {
                next_key = LV_KEY_UP;
            } 
            else if (strcmp(gpio_buttons[button_index].name, "down") == 0) {
                next_key = LV_KEY_DOWN;
            }
            else if (strcmp(gpio_buttons[button_index].name, "left") == 0) {
                next_key = LV_KEY_LEFT;
            }
            else if (strcmp(gpio_buttons[button_index].name, "right") == 0) {
                next_key = LV_KEY_RIGHT;
            }
            else if (strcmp(gpio_buttons[button_index].name, "center") == 0) {
                next_key = LV_KEY_ENTER;
            }
            break;
            
        default:
            break;
    }
    
    if (next_key != LV_KEY_END) {
        next_key_pressed = true;
        printf("GPIO %s: %s (Pin: %d, Chip: %s)\n", 
               gpio_buttons[button_index].is_holding ? "Holding" : "Pressed", 
               gpio_buttons[button_index].name,
               gpio_buttons[button_index].pin_number,
               gpio_buttons[button_index].chip_name);
    }
}

void handle_gpio_input(void) {
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    long current_time = ts.tv_sec * 1000 + ts.tv_nsec / 1000000;
    
    for (size_t i = 0; i < sizeof(gpio_buttons) / sizeof(gpio_buttons[0]); i++) {
        if (gpio_buttons[i].chip && gpio_buttons[i].line) {
            int current_state = gpiod_line_get_value(gpio_buttons[i].line);
            
            // Check for state change (with debounce)
            if (current_state != gpio_buttons[i].last_state &&
                (current_time - gpio_buttons[i].last_time) > DEBOUNCE_DELAY_MS) {
                
                gpio_buttons[i].last_state = current_state;
                gpio_buttons[i].last_time = current_time;
                
                if (current_state == 1) { // Button pressed
                    gpio_buttons[i].is_holding = true;
                    gpio_buttons[i].long_press_sent = false;
                    gpio_buttons[i].repeat_time = current_time + INITIAL_REPEAT_DELAY_MS;
                    
                    // Fire event immediately for all buttons EXCEPT 'right' and 'left'.
                    // For those, we wait to see if it's a short or long press.
                    if (strcmp(gpio_buttons[i].name, "right") != 0 &&
                        strcmp(gpio_buttons[i].name, "left") != 0) {
                        send_button_event(i);
                    }
                } else { // Button released
                    gpio_buttons[i].is_holding = false;
                    
                    // If 'right' or 'left' was released without a long press, send the normal event now.
                    if ((strcmp(gpio_buttons[i].name, "right") == 0 ||
                         strcmp(gpio_buttons[i].name, "left") == 0) &&
                        !gpio_buttons[i].long_press_sent) {
                        send_button_event(i);
                    } else {
                        // For all other buttons, or for a long-pressed 'right' button,
                        // just send a generic release to LVGL.
                        next_key_pressed = false;
                    }
                }
            }
            
            // LOGIC FOR HELD BUTTONS (LONG PRESS / REPEAT) ---
            if (gpio_buttons[i].is_holding && current_state == 1 && 
                current_time >= gpio_buttons[i].repeat_time) {
                
                // Special long-press handling for 'right' and 'left'
                if (strcmp(gpio_buttons[i].name, "right") == 0 ||
                    strcmp(gpio_buttons[i].name, "left") == 0) {
                    if (!gpio_buttons[i].long_press_sent) {
                        send_long_press_event(i);
                        gpio_buttons[i].long_press_sent = true;
                    }
                }
                else {
                    // Standard repeat for all other buttons
                    send_button_event(i);
                    gpio_buttons[i].repeat_time = current_time + REPEAT_RATE_MS;
                }
            }
        }
    }
}


// Cleanup function for GPIO
void cleanup_gpio(void) {
    for (int i = 0; i < MAX_GPIO_BUTTONS; i++) {
        if (gpio_buttons[i].chip) {
            gpiod_chip_close(gpio_buttons[i].chip);
            gpio_buttons[i].chip = NULL;
            gpio_buttons[i].line = NULL;
        }
        if (gpio_buttons[i].chip_name) {
            free((void*)gpio_buttons[i].chip_name);
            gpio_buttons[i].chip_name = NULL;
        }
        if (gpio_buttons[i].chip_label) {
            free((void*)gpio_buttons[i].chip_label);
            gpio_buttons[i].chip_label = NULL;
        }
    }
}

// ---------------------------------------------------------------------------
// evdev backend: Linux input event devices (gpio-keys overlay, USB and
// Bluetooth keyboards, IR remotes...). Selected when no gpio: block is present
// in the config, or when gsmenu.input.backend == "evdev". Hotplug-aware so a
// Bluetooth keyboard that connects after startup is picked up automatically.
// ---------------------------------------------------------------------------

// Fixed button names, used regardless of whether a gpio: block exists.
static const char *evdev_button_names[MAX_GPIO_BUTTONS] = {
    "up", "down", "left", "right", "center", "rec"
};

// Map a Linux key code to a button index (0..5), or -1 if it is not a nav key.
// Covers WASD(E) and the native arrow/enter cluster (real keyboards), plus the
// F13..F18 codes emitted by the gpio-keys overlay. The gpio buttons use the
// F13..F18 range on purpose: those codes are not in the console keymap, so
// button presses never leak to the foreground tty -- independent of whether
// PixelPilot is running -- while still being delivered to us over evdev.
static int keycode_to_button(int code) {
    switch (code) {
        case KEY_F13: case KEY_UP:    case KEY_W:                   return 0; // up
        case KEY_F14: case KEY_DOWN:  case KEY_S:                   return 1; // down
        case KEY_F15: case KEY_LEFT:  case KEY_A:                   return 2; // left
        case KEY_F16: case KEY_RIGHT: case KEY_D:                   return 3; // right
        case KEY_F17: case KEY_ENTER: case KEY_KPENTER: case KEY_E: return 4; // center
        case KEY_F18: case KEY_RECORD: case KEY_R:                  return 5; // rec
        default:                                                    return -1;
    }
}

#define MAX_EVDEV 32
static int  evdev_fds[MAX_EVDEV];
static char evdev_paths[MAX_EVDEV][64];
static int  evdev_count = 0;
static int  inotify_fd  = -1;

// Open one event device if it can emit key events and we don't already have it.
static void evdev_try_open(const char *path) {
    for (int i = 0; i < evdev_count; i++)
        if (strcmp(evdev_paths[i], path) == 0) return;   // already open
    if (evdev_count >= MAX_EVDEV) return;

    int fd = open(path, O_RDONLY | O_NONBLOCK);
    if (fd < 0) return;

    // Keep only devices that can produce EV_KEY (skip pure pointer/accel nodes).
    unsigned long evbit = 0;
    if (ioctl(fd, EVIOCGBIT(0, sizeof(evbit)), &evbit) < 0 ||
        !(evbit & (1UL << EV_KEY))) {
        close(fd);
        return;
    }

    evdev_fds[evdev_count] = fd;
    strncpy(evdev_paths[evdev_count], path, sizeof(evdev_paths[0]) - 1);
    evdev_paths[evdev_count][sizeof(evdev_paths[0]) - 1] = '\0';
    evdev_count++;
    printf("evdev: opened %s\n", path);
}

static void evdev_close_idx(int i) {
    printf("evdev: closed %s\n", evdev_paths[i]);
    close(evdev_fds[i]);
    evdev_fds[i] = evdev_fds[evdev_count - 1];
    memcpy(evdev_paths[i], evdev_paths[evdev_count - 1], sizeof(evdev_paths[0]));
    evdev_count--;
}

void setup_evdev(void) {
    // Button names are fixed for the evdev backend; unlike the gpio backend
    // they do not depend on a gpio: block being present in the YAML config.
    for (int i = 0; i < MAX_GPIO_BUTTONS; i++) {
        gpio_buttons[i].name = evdev_button_names[i];
        gpio_buttons[i].last_state = 0;
    }

    glob_t globbuf;
    if (glob("/dev/input/event*", 0, NULL, &globbuf) == 0) {
        for (size_t i = 0; i < globbuf.gl_pathc; i++)
            evdev_try_open(globbuf.gl_pathv[i]);
        globfree(&globbuf);
    }

    // Watch for hotplugged devices (e.g. a Bluetooth keyboard connecting later).
    inotify_fd = inotify_init1(IN_NONBLOCK);
    if (inotify_fd >= 0)
        inotify_add_watch(inotify_fd, "/dev/input", IN_CREATE);
    else
        perror("evdev: inotify_init1");
}

void handle_evdev_input(void) {
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    long current_time = ts.tv_sec * 1000 + ts.tv_nsec / 1000000;

    // (a) Service hotplug: open any newly-appeared event devices.
    if (inotify_fd >= 0) {
        char buf[1024];
        ssize_t n;
        while ((n = read(inotify_fd, buf, sizeof(buf))) > 0) {
            for (char *p = buf; p < buf + n; ) {
                struct inotify_event *e = (struct inotify_event *)p;
                if (e->len && strncmp(e->name, "event", 5) == 0) {
                    char path[80];
                    snprintf(path, sizeof(path), "/dev/input/%s", e->name);
                    evdev_try_open(path);
                }
                p += sizeof(struct inotify_event) + e->len;
            }
        }
    }

    // (b) Drain key events from every open device.
    struct input_event ev;
    for (int f = 0; f < evdev_count; ) {
        ssize_t r;
        bool removed = false;
        while ((r = read(evdev_fds[f], &ev, sizeof(ev))) == (ssize_t)sizeof(ev)) {
            if (ev.type != EV_KEY) continue;
            if (ev.value == 2) continue;             // ignore kernel autorepeat

            int i = keycode_to_button(ev.code);
            if (i < 0 || gpio_buttons[i].name == NULL) continue;

            gpio_buttons[i].last_state = ev.value;   // 1 = press, 0 = release
            gpio_buttons[i].last_time  = current_time;

            if (ev.value == 1) {                     // pressed
                gpio_buttons[i].is_holding      = true;
                gpio_buttons[i].long_press_sent = false;
                gpio_buttons[i].repeat_time     = current_time + INITIAL_REPEAT_DELAY_MS;

                // Defer 'right'/'left' to distinguish short vs long press.
                if (strcmp(gpio_buttons[i].name, "right") != 0 &&
                    strcmp(gpio_buttons[i].name, "left")  != 0) {
                    send_button_event(i);
                }
            } else {                                 // released
                gpio_buttons[i].is_holding = false;
                if ((strcmp(gpio_buttons[i].name, "right") == 0 ||
                     strcmp(gpio_buttons[i].name, "left")  == 0) &&
                    !gpio_buttons[i].long_press_sent) {
                    send_button_event(i);
                } else {
                    next_key_pressed = false;
                }
            }
        }
        // Device went away (e.g. Bluetooth disconnect): drop it.
        if (r < 0 && errno != EAGAIN && errno != EWOULDBLOCK) {
            evdev_close_idx(f);
            removed = true;
        }
        if (!removed) f++;
    }

    // (c) Long-press / repeat for still-held buttons (same policy as gpio).
    for (size_t i = 0; i < MAX_GPIO_BUTTONS; i++) {
        if (gpio_buttons[i].name == NULL) continue;
        if (gpio_buttons[i].is_holding && gpio_buttons[i].last_state == 1 &&
            current_time >= gpio_buttons[i].repeat_time) {
            if (strcmp(gpio_buttons[i].name, "right") == 0 ||
                strcmp(gpio_buttons[i].name, "left")  == 0) {
                if (!gpio_buttons[i].long_press_sent) {
                    send_long_press_event(i);
                    gpio_buttons[i].long_press_sent = true;
                }
            } else {
                send_button_event(i);
                gpio_buttons[i].repeat_time = current_time + REPEAT_RATE_MS;
            }
        }
    }
}

void cleanup_evdev(void) {
    for (int i = 0; i < evdev_count; i++)
        close(evdev_fds[i]);
    evdev_count = 0;
    if (inotify_fd >= 0) {
        close(inotify_fd);
        inotify_fd = -1;
    }
}

// ---------------------------------------------------------------------------
// Backend selection: gpio (libgpiod polling) vs evdev. Defaults to "auto",
// which uses gpiod iff a non-empty gpio: block exists, else evdev. Override
// with gsmenu.input.backend: gpio | evdev | auto.
// ---------------------------------------------------------------------------
static input_backend_t input_backend = INPUT_BACKEND_GPIO;

static input_backend_t choose_input_backend(YAML::Node& config) {
    std::string sel = "auto";
    YAML::Node in = config["gsmenu"]["input"];
    if (in && in["backend"])
        sel = in["backend"].as<std::string>();

    if (sel == "gpio")  return INPUT_BACKEND_GPIO;
    if (sel == "evdev") return INPUT_BACKEND_EVDEV;

    YAML::Node gpio = config["gsmenu"]["gpio"];
    bool has_gpio = gpio && gpio.IsMap() && gpio.size() > 0;
    return has_gpio ? INPUT_BACKEND_GPIO : INPUT_BACKEND_EVDEV;
}

void setup_input(YAML::Node& config) {
    input_backend = choose_input_backend(config);
    if (input_backend == INPUT_BACKEND_GPIO) {
        printf("input: backend=gpio (libgpiod polling)\n");
        setup_gpio(config);
    } else {
        printf("input: backend=evdev (hotplug-aware)\n");
        setup_evdev();
    }
}

void handle_input(void) {
    if (input_backend == INPUT_BACKEND_GPIO)
        handle_gpio_input();
    else
        handle_evdev_input();
}

void cleanup_input(void) {
    if (input_backend == INPUT_BACKEND_GPIO)
        cleanup_gpio();
    else
        cleanup_evdev();
}
#endif

// Function to make stdin non-blocking
void set_stdin_nonblock(void) {
    int flags = fcntl(STDIN_FILENO, F_GETFL, 0);
    fcntl(STDIN_FILENO, F_SETFL, flags | O_NONBLOCK);
    
    // Disable canonical mode and echo
    struct termios term;
    tcgetattr(STDIN_FILENO, &term);
    term.c_lflag &= ~(ICANON | ECHO);
    term.c_cc[VMIN] = 0;
    term.c_cc[VTIME] = 0;
    tcsetattr(STDIN_FILENO, TCSANOW, &term);
}

// Function to restore terminal settings
void restore_stdin(void) {
    struct termios term;
    tcgetattr(STDIN_FILENO, &term);
    term.c_lflag |= (ICANON | ECHO);
    tcsetattr(STDIN_FILENO, TCSANOW, &term);
}

void toggle_screen(void) {
    if( ! menu_active ) {
        lv_scr_load(pp_menu_screen);
        lv_indev_set_group(indev_drv,main_group);
        lv_obj_invalidate(pp_menu_screen);
        menu_active = true;
    }
}

// Direction helpers shared by WASD, the E/Enter key and terminal arrow-key
// escape sequences. Each sets next_key according to the active control_mode.
static void kb_dir_up(void) {
    switch (control_mode) {
        case GSMENU_CONTROL_MODE_NAV:      next_key = LV_KEY_PREV;  break;
        case GSMENU_CONTROL_MODE_SLIDER:   next_key = LV_KEY_RIGHT; break;
        case GSMENU_CONTROL_MODE_EDIT:     next_key = LV_KEY_UP;    break;
        case GSMENU_CONTROL_MODE_KEYBOARD: next_key = LV_KEY_UP;    break;
        default: break;
    }
    next_key_pressed = true;
    printf("Up\n");
}

static void kb_dir_down(void) {
    switch (control_mode) {
        case GSMENU_CONTROL_MODE_SLIDER:   next_key = LV_KEY_LEFT; break;
        case GSMENU_CONTROL_MODE_NAV:      next_key = LV_KEY_NEXT; break;
        case GSMENU_CONTROL_MODE_EDIT:     next_key = LV_KEY_DOWN; break;
        case GSMENU_CONTROL_MODE_KEYBOARD: next_key = LV_KEY_DOWN; break;
        default: break;
    }
    next_key_pressed = true;
    printf("Down\n");
}

static void kb_dir_left(void) {
    switch (control_mode) {
        case GSMENU_CONTROL_MODE_SLIDER:
        case GSMENU_CONTROL_MODE_EDIT:     next_key = LV_KEY_ESC;  break;
        case GSMENU_CONTROL_MODE_NAV:      next_key = LV_KEY_HOME; break;
        case GSMENU_CONTROL_MODE_KEYBOARD: next_key = LV_KEY_LEFT; break;
        default: break;
    }
    next_key_pressed = true;
    printf("Left\n");
}

static void kb_dir_right(void) {
    switch (control_mode) {
        case GSMENU_CONTROL_MODE_NAV:
            next_key = menu_active ? LV_KEY_ENTER : LV_KEY_RIGHT;
            break;
        case GSMENU_CONTROL_MODE_SLIDER:   next_key = LV_KEY_ENTER; break;
        case GSMENU_CONTROL_MODE_EDIT:     next_key = LV_KEY_ENTER; break;
        case GSMENU_CONTROL_MODE_KEYBOARD: next_key = LV_KEY_RIGHT; break;
        default: break;
    }
    next_key_pressed = true;
    printf("Right\n");
}

static void kb_enter(void) {
    next_key = LV_KEY_ENTER;
    next_key_pressed = true;
    printf("Enter\n");
}

// Handle stdin input (WASD/E, Enter and terminal arrow-key escape sequences)
// and convert to LVGL key codes. Always active in addition to the selected
// button backend, so a serial console or SSH session can drive the menu.
void handle_keyboard_input(void) {
    char buf[8];
    int n = read(STDIN_FILENO, buf, sizeof(buf));
    for (int k = 0; k < n; k++) {
        char c = buf[k];

        // Terminal arrow keys arrive as ESC '[' 'A'..'D'.
        if (c == 0x1b && k + 2 < n && buf[k + 1] == '[') {
            switch (buf[k + 2]) {
                case 'A': kb_dir_up();    break;
                case 'B': kb_dir_down();  break;
                case 'C': kb_dir_right(); break;
                case 'D': kb_dir_left();  break;
                default: break;
            }
            k += 2;
            continue;
        }

        switch (c) {
            case 'w': case 'W': kb_dir_up();    break;
            case 's': case 'S': kb_dir_down();  break;
            case 'a': case 'A': kb_dir_left();  break;
            case 'd': case 'D': kb_dir_right(); break;
            case 'e': case 'E':
            case '\r': case '\n': kb_enter();   break;
#ifdef USE_SIMULATOR
            case 't': case 'T':
                if (timer) {
                    lv_timer_delete(timer);
                    timer = NULL;
                } else {
                    timer = lv_timer_create(simulate_traffic, 50, NULL);
                }
                break;
#endif
            case 'q': case 'Q':
                raise(SIGINT);
                break;
        }
    }
}

// Custom function to simulate keyboard input
static void virtual_keyboard_read(lv_indev_t * indev, lv_indev_data_t * data) {
    static bool key_sent = false;  // Track if a key event was sent

#ifndef USE_SIMULATOR
    handle_input(); // poll the selected button backend (gpio or evdev)
#endif

    if (next_key != LV_KEY_END) {
        data->key = next_key;
        data->state = next_key_pressed ? LV_INDEV_STATE_PR : LV_INDEV_STATE_REL;

        next_key_pressed = !next_key_pressed;  // Toggle state

        if (next_key != LV_KEY_ENTER)
            toggle_screen();

        if (!next_key_pressed) {  
            next_key = LV_KEY_END;  // Reset key after release event
        }

        key_sent = true;  // Mark that a key was sent
    } else if (key_sent) {
        data->state = LV_INDEV_STATE_REL;  // Ensure release event is sent
        key_sent = false;  // Reset the flag
    } else {
        data->state = LV_INDEV_STATE_REL;
    }
}

// Function to create the virtual keyboard
lv_indev_t * create_virtual_keyboard() {

    set_stdin_nonblock(); // setup keyboard input from stdin
#ifndef USE_SIMULATOR
    setup_input(config);         // Initialize selected button backend (gpio/evdev)
#endif
    lv_indev_t * indev_drv = lv_indev_create();
    lv_indev_set_type(indev_drv, LV_INDEV_TYPE_KEYPAD);
    lv_indev_set_read_cb(indev_drv, virtual_keyboard_read);

    lv_indev_enable(indev_drv, true);

    return indev_drv;
}
