// adc_input.cpp
//
// Reader for the Ascent VRX Pro's physical buttons.
//
// Earlier work in this project chased a UART "FSMP" protocol found in
// stock firmware's ar_ldy_gnd binary (serial_fsmp_receive_thread, a whole
// framed protocol with headers/checksums/trailers) as the button
// mechanism. That code is real and does run on real hardware, but it isn't
// what reports the physical joystick/OK/REC/BACK buttons -- confirmed by:
//   1. Decompiling ar_ldy_gnd's Fxn_key_detect (0x00191198), which reads
//      /sys/bus/iio/devices/iio:device0/in_voltage6_raw (the SARADC),
//      converts raw code to millivolts (raw * 1800 / 1024, i.e. a 10-bit
//      ADC against a 1.8V reference), takes a 10-sample trimmed mean
//      (drops the single highest and lowest reading, averages the rest --
//      see FUN_00190b24), and matches the result against a fixed 7-entry
//      voltage-ladder table with +/-80mV tolerance -- entirely separate
//      code from the UART parser, with its own dedicated thread.
//   2. Directly polling that same sysfs node on real hardware while
//      physically pressing each button and watching the millivolt reading
//      land within a few mV of the table below -- see the table comment.
// The two mechanisms coexist in the same binary; the UART one is most
// likely for the wireless/RF link (interleaved "GND->SKY" traffic appears
// in the same debug log stream), not the local physical buttons.
//
// Voltage ladder (key_id, name, table mV) as read out of ar_ldy_gnd's
// .rodata at 0x00414150 (7 entries x 16 bytes: int32 key_id, int32
// voltage_mv, char* name), cross-checked against real button presses:
//
//   key_id  name   table mV   observed mV (real hardware)
//   0       up      800        827
//   2       left   1000        1030, 1035
//   3       right  1200        1232
//   4       ok     1400        1432
//   1       down   1600        1636
//   6       rec     390        420
//   7       esc    1780        1798
//
// Only "up/down/left/right/center/rec" are wired to a PixelPilot action,
// through the same dispatch_named_key_event() path GPIO buttons use (see
// input.cpp) -- stock's "ok" maps to our "center". stock's "esc" (the
// physical BACK button) is aliased to "left" (back/escape-a-step), the
// same choice made for GPIO boards that lack a dedicated back button.
// Unlike GPIO boards, this hardware has dedicated OK and REC buttons, so
// there's no need to repurpose any button's long-press for confirm or
// record-toggle here -- every button dispatches immediately on press with
// standard auto-repeat, and none of them go through
// dispatch_named_long_press(). In particular, holding BACK does nothing
// extra here: on stock firmware a long-held BACK reboots the goggle unit's
// GUI process (confirmed by decompiling GlassesUI::slotKeyPress) --
// deliberately not replicated.
//
// Other single-channel ladders (the BetaFPV VR04's: up/down/left/right,
// enter, back, record and bind on one channel) use the same reader with
// their own table from config["gsmenu"]["vrx_adc"]: keys, scale_mv,
// idle_mv, tolerance_mv; a "bind" key on the ladder opens the bind dialog
// like the VRX Pro's separate bind button. Left out, every setting is the
// VRX Pro's above.

#include <cerrno>
#include <climits>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <ctime>
#include <string>
#include <vector>
#include <fcntl.h>
#include <unistd.h>
#include <yaml-cpp/yaml.h>

#include "input.h"
#include "adc_input.h"
#include "gsmenu/bind_dialog.h"

extern YAML::Node config;

namespace {

constexpr int kDebounceMs = 50;           // matches DEBOUNCE_DELAY_MS in input.cpp
constexpr int kInitialRepeatDelayMs = 500; // matches INITIAL_REPEAT_DELAY_MS
constexpr int kRepeatRateMs = 100;         // matches REPEAT_RATE_MS

constexpr int kIdleThresholdMv = 135; // below this: no button pressed (stock's 0x87)
constexpr int kMatchToleranceMv = 80; // +/-0x50, same window stock uses
constexpr double kMvPerCount = 1800.0 / 1024; // 10-bit SARADC against 1.8 V

enum class VrxKey { None, Up, Down, Left, Right, Center, Rec, Bind };

struct AdcKeyEntry {
    VrxKey key;
    int voltage_mv;
};

constexpr AdcKeyEntry kAdcKeys[] = {
    { VrxKey::Up,      800 },
    { VrxKey::Left,   1000 },
    { VrxKey::Right,  1200 },
    { VrxKey::Center, 1400 },
    { VrxKey::Down,   1600 },
    { VrxKey::Rec,     390 },
    { VrxKey::Left,   1780 }, // stock "esc" / physical BACK button, aliased to left
};

const char *key_name(VrxKey k) {
    switch (k) {
        case VrxKey::Up:     return "up";
        case VrxKey::Down:   return "down";
        case VrxKey::Left:   return "left";
        case VrxKey::Right:  return "right";
        case VrxKey::Center: return "center";
        case VrxKey::Rec:    return "rec";
        default:             return nullptr;
    }
}

// A key name from the config: the dispatch names, plus the buttons' own
// names where they differ. "back" is "left" (back/escape a step), as the
// VRX Pro's BACK button is above.
VrxKey key_from_name(const std::string &n) {
    if (n == "up") return VrxKey::Up;
    if (n == "down") return VrxKey::Down;
    if (n == "left" || n == "back" || n == "esc") return VrxKey::Left;
    if (n == "right") return VrxKey::Right;
    if (n == "center" || n == "ok" || n == "enter") return VrxKey::Center;
    if (n == "rec" || n == "record") return VrxKey::Rec;
    if (n == "bind") return VrxKey::Bind;
    return VrxKey::None;
}

bool g_enabled = false;
std::string g_device;
std::vector<AdcKeyEntry> g_keys(std::begin(kAdcKeys), std::end(kAdcKeys));
double g_mv_per_count = kMvPerCount;
int g_idle_mv = kIdleThresholdMv;
int g_tolerance_mv = kMatchToleranceMv;

// The "bind" button (in_voltage0_raw by default) is a separate SARADC
// channel from the joystick ladder above: it's a plain momentary switch, not
// a voltage ladder, so it's read as a raw ADC code rather than millivolts --
// full-scale (~1023) when released, near zero when pressed.
constexpr int kBindPressedRawThreshold = 15; // raw code below this = pressed (released reads ~1023)

bool g_bind_enabled = false;
std::string g_bind_device;
bool g_bind_last_seen_pressed = false;
bool g_bind_confirmed_pressed = false;

// Debounce state: a raw reading must repeat on two consecutive polls before
// it's accepted, which rejects the single-sample noise/mid-transition
// voltages an analog ladder is prone to right as a button is pressed or
// released (a clean digital GPIO edge doesn't need this).
VrxKey g_last_seen = VrxKey::None;
VrxKey g_confirmed = VrxKey::None;
long g_repeat_time = 0;

long now_ms() {
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return ts.tv_sec * 1000L + ts.tv_nsec / 1000000L;
}

bool read_raw(const std::string &path, int *out_raw) {
    int fd = open(path.c_str(), O_RDONLY);
    if (fd < 0) return false;
    char buf[32] = {0};
    ssize_t n = read(fd, buf, sizeof(buf) - 1);
    close(fd);
    if (n <= 0) return false;
    *out_raw = atoi(buf);
    return true;
}

bool read_millivolts(int *out_mv) {
    int raw;
    if (!read_raw(g_device, &raw)) return false;
    *out_mv = (int)(raw * g_mv_per_count);
    return true;
}

// The IIO channel's own scale (mV per count), from in_voltage_scale or
// in_voltageN_scale next to the in_voltageN_raw being read.
bool read_iio_scale(const std::string &raw_path, double *out) {
    const size_t slash = raw_path.rfind('/');
    const size_t suffix = raw_path.rfind("_raw");
    if (slash == std::string::npos || suffix == std::string::npos) return false;
    const std::string dir = raw_path.substr(0, slash + 1);
    const std::string channel = raw_path.substr(slash + 1, suffix - slash - 1); // in_voltageN
    for (const std::string &name : {channel + "_scale", std::string("in_voltage_scale")}) {
        FILE *f = fopen((dir + name).c_str(), "r");
        if (!f) continue;
        double v = 0;
        const bool ok = fscanf(f, "%lf", &v) == 1 && v > 0;
        fclose(f);
        if (ok) {
            *out = v;
            return true;
        }
    }
    return false;
}

bool sample_bind_pressed() {
    int raw;
    if (!read_raw(g_bind_device, &raw)) return false;
    return raw < kBindPressedRawThreshold;
}

VrxKey sample_key() {
    int mv;
    if (!read_millivolts(&mv)) return VrxKey::None;
    if (mv < g_idle_mv) return VrxKey::None;
    for (const auto &entry : g_keys) {
        if (std::abs(mv - entry.voltage_mv) <= g_tolerance_mv) return entry.key;
    }
    return VrxKey::None;
}

void fire_press(VrxKey key, bool holding) {
    const char *name = key_name(key);
    if (name == NULL) return;
    dispatch_named_key_event(name);
    if (dispatch_has_pending_key()) {
        printf("VRX ADC %s: %s\n", holding ? "Holding" : "Pressed", name);
    }
}

} // namespace

void setup_vrx_adc(void) {
    YAML::Node adc_config = config["gsmenu"]["vrx_adc"];
    if (!adc_config || adc_config.IsNull()) return;

    bool enabled = adc_config["enabled"] ? adc_config["enabled"].as<bool>() : true;
    if (!enabled) return;

    g_device = adc_config["device"]
        ? adc_config["device"].as<std::string>()
        : "/sys/bus/iio/devices/iio:device0/in_voltage6_raw";

    if (adc_config["scale_mv"]) {
        const std::string scale = adc_config["scale_mv"].as<std::string>();
        if (scale == "auto") {
            if (!read_iio_scale(g_device, &g_mv_per_count))
                fprintf(stderr, "VRX ADC: no IIO scale next to %s, %.4f mV per count\n",
                        g_device.c_str(), g_mv_per_count);
        } else {
            g_mv_per_count = adc_config["scale_mv"].as<double>();
        }
    }
    if (adc_config["idle_mv"]) g_idle_mv = adc_config["idle_mv"].as<int>();
    if (adc_config["tolerance_mv"]) g_tolerance_mv = adc_config["tolerance_mv"].as<int>();
    if (adc_config["keys"] && adc_config["keys"].IsSequence()) {
        g_keys.clear();
        for (const auto &k : adc_config["keys"]) {
            const std::string name = k["name"] ? k["name"].as<std::string>() : "";
            const VrxKey key = key_from_name(name);
            if (key == VrxKey::None || !k["mv"]) {
                fprintf(stderr, "VRX ADC: ignoring key '%s' (needs a known name and mv)\n", name.c_str());
                continue;
            }
            g_keys.push_back({key, k["mv"].as<int>()});
        }
    }

    int mv;
    if (!read_millivolts(&mv)) {
        fprintf(stderr, "VRX ADC: failed to read %s: %s\n", g_device.c_str(), strerror(errno));
        return;
    }

    g_enabled = true;
    printf("VRX ADC: polling %s (%zu keys, %.4f mV per count)\n", g_device.c_str(), g_keys.size(),
           g_mv_per_count);

    // An empty bind_device: no separate bind button (a ladder may carry one).
    g_bind_device = adc_config["bind_device"]
        ? adc_config["bind_device"].as<std::string>()
        : "/sys/bus/iio/devices/iio:device0/in_voltage0_raw";
    if (g_bind_device.empty()) return;

    int bind_raw;
    if (!read_raw(g_bind_device, &bind_raw)) {
        fprintf(stderr, "VRX ADC: failed to read bind button %s: %s\n", g_bind_device.c_str(), strerror(errno));
        return;
    }

    g_bind_enabled = true;
    printf("VRX ADC: polling bind button %s\n", g_bind_device.c_str());
}

void handle_vrx_adc_input(void) {
    if (!g_enabled) return;

    VrxKey seen = sample_key();
    if (seen != g_last_seen) {
        g_last_seen = seen;
        return; // wait one more tick to confirm this isn't a noise transient
    }

    long now = now_ms();

    if (seen == g_confirmed) {
        if (seen != VrxKey::None && now >= g_repeat_time) {
            fire_press(seen, true);
            g_repeat_time = now + kRepeatRateMs;
        }
        return;
    }

    const VrxKey released = g_confirmed;
    g_confirmed = seen;
    if (seen == VrxKey::None) {
        if (released != VrxKey::Bind)
            dispatch_release_key();
    } else if (seen == VrxKey::Bind) {
        // Like the separate bind button: once per press, no repeat.
        printf("VRX ADC: bind button pressed\n");
        bind_dialog_trigger();
        g_repeat_time = LONG_MAX;
    } else {
        fire_press(seen, false);
        g_repeat_time = now + kInitialRepeatDelayMs;
    }
}

void handle_vrx_bind_input(void) {
    if (!g_bind_enabled) return;

    bool seen = sample_bind_pressed();
    if (seen != g_bind_last_seen_pressed) {
        g_bind_last_seen_pressed = seen;
        return; // wait one more tick to confirm this isn't a noise transient
    }

    if (seen == g_bind_confirmed_pressed) return;
    g_bind_confirmed_pressed = seen;

    if (seen) {
        printf("VRX ADC: bind button pressed\n");
        bind_dialog_trigger();
    }
}

void cleanup_vrx_adc(void) {
    g_enabled = false;
    g_last_seen = VrxKey::None;
    g_confirmed = VrxKey::None;

    g_bind_enabled = false;
    g_bind_last_seen_pressed = false;
    g_bind_confirmed_pressed = false;
}
