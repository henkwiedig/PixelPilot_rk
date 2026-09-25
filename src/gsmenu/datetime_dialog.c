#include "datetime_dialog.h"

#include "styles.h"
#include "../input.h"   /* control_mode */
#include <errno.h>
#include <fcntl.h>
#include <linux/rtc.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <time.h>
#include <unistd.h>

extern gsmenu_control_mode_t control_mode;
extern lv_indev_t * indev_drv;

/*
 * Date & Time dialog, driven entirely by the 5-way joystick: left/right move
 * between the fields and the Set/Cancel buttons, up/down change the focused
 * value, center activates. lv_calendar isn't used on purpose -- its month/year
 * header needs focus keys the keyboard control mode doesn't send, and LVGL has
 * no time picker. Values are local time (/etc/localtime); Set updates the
 * system clock and writes UTC to /dev/rtc0 so it survives a power cycle where
 * the RTC is battery-backed.
 */

enum { F_YEAR, F_MONTH, F_DAY, F_HOUR, F_MIN, F_SET, F_CANCEL, F_COUNT };

#define YEAR_MIN 2020
#define YEAR_MAX 2099

typedef struct {
    lv_obj_t *   overlay;
    lv_obj_t *   panel;
    lv_obj_t *   field[F_COUNT];
    lv_group_t * group;
    lv_group_t * prev_group;
    int          v[5];          /* year, month, day, hour, minute */
    int          focus;
    void       (*on_done)(bool changed);
} dt_ctx_t;

static int days_in_month(int y, int m)
{
    static const int d[] = { 31, 28, 31, 30, 31, 30, 31, 31, 30, 31, 30, 31 };
    bool leap = (y % 4 == 0 && y % 100 != 0) || y % 400 == 0;
    return m == 2 && leap ? 29 : d[m - 1];
}

static void refresh(dt_ctx_t * c)
{
    static const char * const fmt[] = { "%04d", "%02d", "%02d", "%02d", "%02d" };
    for(int i = 0; i < F_COUNT; i++) {
        if(i < F_SET) lv_label_set_text_fmt(c->field[i], fmt[i], c->v[i]);
        bool f = i == c->focus;
        lv_obj_set_style_bg_opa(c->field[i], f ? LV_OPA_COVER : LV_OPA_TRANSP, 0);
        lv_obj_set_style_bg_color(c->field[i], lv_color_hex(0x4c60d8), 0);
    }
}

static void step(dt_ctx_t * c, int dir)
{
    int * v = &c->v[c->focus];
    switch(c->focus) {
    case F_YEAR:  *v = *v + dir > YEAR_MAX ? YEAR_MIN : *v + dir < YEAR_MIN ? YEAR_MAX : *v + dir; break;
    case F_MONTH: *v = (*v - 1 + dir + 12) % 12 + 1; break;
    case F_DAY: {
        int n = days_in_month(c->v[F_YEAR], c->v[F_MONTH]);
        *v = (*v - 1 + dir + n) % n + 1;
        break;
    }
    case F_HOUR:  *v = (*v + dir + 24) % 24; break;
    case F_MIN:   *v = (*v + dir + 60) % 60; break;
    default: return;
    }
    /* a shorter month or Feb in a non-leap year clamps the day */
    int n = days_in_month(c->v[F_YEAR], c->v[F_MONTH]);
    if(c->v[F_DAY] > n) c->v[F_DAY] = n;
}

/* System clock + RTC. mktime() interprets the fields in the local zone. */
static int apply_time(const dt_ctx_t * c)
{
    struct tm tm = { 0 };
    tm.tm_year  = c->v[F_YEAR] - 1900;
    tm.tm_mon   = c->v[F_MONTH] - 1;
    tm.tm_mday  = c->v[F_DAY];
    tm.tm_hour  = c->v[F_HOUR];
    tm.tm_min   = c->v[F_MIN];
    tm.tm_isdst = -1;
    time_t t = mktime(&tm);
    if(t == (time_t)-1) return -1;

    struct timespec ts = { .tv_sec = t, .tv_nsec = 0 };
    if(clock_settime(CLOCK_REALTIME, &ts) != 0) {
        LV_LOG_WARN("datetime: clock_settime: %s", strerror(errno));
        return -1;
    }

    struct tm utc;
    gmtime_r(&t, &utc);
    struct rtc_time rt = {
        .tm_sec = utc.tm_sec, .tm_min = utc.tm_min, .tm_hour = utc.tm_hour,
        .tm_mday = utc.tm_mday, .tm_mon = utc.tm_mon, .tm_year = utc.tm_year,
    };
    int fd = open("/dev/rtc0", O_RDONLY);
    if(fd < 0 || ioctl(fd, RTC_SET_TIME, &rt) != 0)
        LV_LOG_WARN("datetime: /dev/rtc0 not updated: %s", strerror(errno));
    if(fd >= 0) close(fd);
    return 0;
}

static void close_async(void * p)
{
    dt_ctx_t * c = p;
    lv_indev_set_group(indev_drv, c->prev_group);
    lv_group_delete(c->group);
    lv_obj_delete(c->overlay);
    control_mode = GSMENU_CONTROL_MODE_NAV;
    free(c);
}

static void finish(dt_ctx_t * c, bool set)
{
    bool changed = set && apply_time(c) == 0;
    if(c->on_done) c->on_done(changed);
    lv_async_call(close_async, c);   /* we are inside the panel's own event */
}

static void key_cb(lv_event_t * e)
{
    dt_ctx_t * c = lv_event_get_user_data(e);
    switch(lv_event_get_key(e)) {
    case LV_KEY_LEFT:  c->focus = (c->focus + F_COUNT - 1) % F_COUNT; break;
    case LV_KEY_RIGHT: c->focus = (c->focus + 1) % F_COUNT; break;
    case LV_KEY_UP:    step(c, +1); break;
    case LV_KEY_DOWN:  step(c, -1); break;
    case LV_KEY_ESC:   finish(c, false); return;
    case LV_KEY_ENTER:
        if(c->focus == F_CANCEL) { finish(c, false); return; }
        if(c->focus == F_SET)    { finish(c, true);  return; }
        c->focus = F_SET;        /* center on a value jumps to Set */
        break;
    default: return;
    }
    refresh(c);
}

static lv_obj_t * add_field(lv_obj_t * parent, const char * text, int min_w)
{
    lv_obj_t * l = lv_label_create(parent);
    lv_label_set_text(l, text);
    lv_obj_set_style_min_width(l, min_w, 0);
    lv_obj_set_style_text_align(l, LV_TEXT_ALIGN_CENTER, 0);
    lv_obj_set_style_pad_hor(l, 8, 0);
    lv_obj_set_style_pad_ver(l, 4, 0);
    lv_obj_set_style_radius(l, 6, 0);
    return l;
}

static void add_sep(lv_obj_t * parent, const char * text)
{
    lv_obj_t * l = lv_label_create(parent);
    lv_label_set_text(l, text);
    lv_obj_set_style_text_color(l, lv_color_hex(0x8a93a6), 0);
}

static lv_obj_t * add_row(lv_obj_t * panel)
{
    lv_obj_t * row = lv_obj_create(panel);
    lv_obj_set_size(row, LV_PCT(100), LV_SIZE_CONTENT);
    lv_obj_set_style_bg_opa(row, LV_OPA_TRANSP, 0);
    lv_obj_set_style_border_width(row, 0, 0);
    lv_obj_set_style_pad_all(row, 4, 0);
    lv_obj_set_style_pad_column(row, 6, 0);
    lv_obj_set_flex_flow(row, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(row, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    lv_obj_clear_flag(row, LV_OBJ_FLAG_SCROLLABLE);
    return row;
}

void datetime_dialog_open(void (*on_done)(bool changed))
{
    dt_ctx_t * c = calloc(1, sizeof(*c));
    c->on_done = on_done;

    time_t now = time(NULL);
    struct tm tm;
    localtime_r(&now, &tm);
    c->v[F_YEAR]  = tm.tm_year + 1900;
    if(c->v[F_YEAR] < YEAR_MIN || c->v[F_YEAR] > YEAR_MAX) c->v[F_YEAR] = YEAR_MIN;
    c->v[F_MONTH] = tm.tm_mon + 1;
    c->v[F_DAY]   = tm.tm_mday;
    c->v[F_HOUR]  = tm.tm_hour;
    c->v[F_MIN]   = tm.tm_min;

    /* dim backdrop + centred panel, same look as the keyboard prompt */
    c->overlay = lv_obj_create(lv_layer_top());
    lv_obj_set_size(c->overlay, LV_PCT(100), LV_PCT(100));
    lv_obj_set_style_bg_color(c->overlay, lv_color_hex(0x000000), 0);
    lv_obj_set_style_bg_opa(c->overlay, LV_OPA_50, 0);
    lv_obj_set_style_border_width(c->overlay, 0, 0);
    lv_obj_clear_flag(c->overlay, LV_OBJ_FLAG_SCROLLABLE);

    lv_obj_t * panel = lv_obj_create(c->overlay);
    c->panel = panel;
    lv_obj_set_size(panel, 560, LV_SIZE_CONTENT);
    lv_obj_center(panel);
    lv_obj_set_style_bg_color(panel, lv_color_hex(0x0b0e14), 0);
    lv_obj_set_style_bg_opa(panel, LV_OPA_COVER, 0);
    lv_obj_set_style_radius(panel, 10, 0);
    lv_obj_set_style_border_color(panel, lv_color_hex(0x4c60d8), 0);
    lv_obj_set_style_border_width(panel, 2, 0);
    lv_obj_set_style_text_color(panel, lv_color_hex(0xffffff), 0);
    lv_obj_set_flex_flow(panel, LV_FLEX_FLOW_COLUMN);
    lv_obj_clear_flag(panel, LV_OBJ_FLAG_SCROLLABLE);

    char zone[64] = "";
    strftime(zone, sizeof(zone), "%Z", &tm);
    lv_obj_t * title = lv_label_create(panel);
    lv_label_set_text_fmt(title, "Date & Time (%s)", zone);
    lv_obj_set_style_text_color(title, lv_color_hex(0x8a93a6), 0);

    lv_obj_t * r1 = add_row(panel);
    c->field[F_YEAR]  = add_field(r1, "", 70);  add_sep(r1, "-");
    c->field[F_MONTH] = add_field(r1, "", 44);  add_sep(r1, "-");
    c->field[F_DAY]   = add_field(r1, "", 44);  add_sep(r1, "  ");
    c->field[F_HOUR]  = add_field(r1, "", 44);  add_sep(r1, ":");
    c->field[F_MIN]   = add_field(r1, "", 44);

    lv_obj_t * r2 = add_row(panel);
    c->field[F_SET]    = add_field(r2, "Set", 110);
    c->field[F_CANCEL] = add_field(r2, "Cancel", 110);

    lv_obj_t * hint = lv_label_create(panel);
    lv_label_set_text(hint, LV_SYMBOL_LEFT LV_SYMBOL_RIGHT " field   " LV_SYMBOL_UP LV_SYMBOL_DOWN " change   OK set");
    lv_obj_set_style_text_color(hint, lv_color_hex(0x8a93a6), 0);

    /* own group: the panel takes every key (see key_cb) */
    lv_obj_add_event_cb(panel, key_cb, LV_EVENT_KEY, c);
    c->prev_group = lv_indev_get_group(indev_drv);
    c->group = lv_group_create();
    lv_group_add_obj(c->group, panel);
    lv_indev_set_group(indev_drv, c->group);
    lv_group_focus_obj(panel);
    control_mode = GSMENU_CONTROL_MODE_KEYBOARD;   /* raw up/down/left/right/enter */

    refresh(c);
}
