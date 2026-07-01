// osd_editor_sim.c
//
// Lightweight stub implementation of the OSD editor C bridge for the SDL
// simulator build, which does not link the real C++ OSD engine (osd.cpp).
// It serves a couple of fake widgets and a fake fact registry so the gsmenu
// OSD editor can be navigated and laid out on the desktop.

#include <stdio.h>
#include <string.h>
#include "osd_editor.h"

#define MAX_W 16

typedef struct {
    char name[64];
    char type[64];
    int  x, y;
    char fact[96];
    char tag_key[32];
    char tag_val[32];
    int  has_tag;
} sim_widget_t;

static sim_widget_t g_w[MAX_W] = {
    { "Bitrate", "VideoBitrateWidget", -250, 95, "gstreamer.received_bytes", "", "", 0 },
    { "RSSI",    "IconSelectorWidget", -250,  0, "wfbcli.rx.ant_stats.rssi_avg", "ant_id", "0", 1 },
};
static int g_count = 2;
static int g_position_target = -1;

static const char *g_preset_labels[] = {
    "Text label", "Box / background", "Templated text", "RSSI signal icon",
    "Video bitrate", "Battery cell voltage",
};
static const int g_preset_count = (int)(sizeof(g_preset_labels) / sizeof(g_preset_labels[0]));

static int valid(int i) { return i >= 0 && i < g_count; }

int osd_ed_count(void) { return g_count; }
const char *osd_ed_type(int idx) { return valid(idx) ? g_w[idx].type : ""; }
const char *osd_ed_name(int idx) { return valid(idx) ? g_w[idx].name : ""; }
const char *osd_ed_label(int idx) {
    static char buf[160];
    if (!valid(idx)) return "";
    snprintf(buf, sizeof(buf), "%s  (%s)", g_w[idx].name, g_w[idx].type);
    return buf;
}

void osd_ed_get_xy(int idx, int *x, int *y) {
    if (x) *x = valid(idx) ? g_w[idx].x : 0;
    if (y) *y = valid(idx) ? g_w[idx].y : 0;
}
void osd_ed_set_xy(int idx, int x, int y) {
    if (valid(idx)) { g_w[idx].x = x; g_w[idx].y = y; }
}
void osd_ed_position_set_target(int idx) { g_position_target = idx; }
int  osd_ed_position_target(void) { return g_position_target; }
void osd_ed_nudge(int dx, int dy) {
    if (valid(g_position_target)) { g_w[g_position_target].x += dx; g_w[g_position_target].y += dy; }
}

int osd_ed_num_count(int idx) { (void)idx; return 0; }
const char *osd_ed_num_key(int idx, int f) { (void)idx; (void)f; return ""; }
long osd_ed_num_value(int idx, int f) { (void)idx; (void)f; return 0; }
void osd_ed_set_num(int idx, const char *key, long val) { (void)idx; (void)key; (void)val; }

int osd_ed_has_color(int idx) { (void)idx; return 0; }
void osd_ed_get_color(int idx, int *r, int *g, int *b, int *a) {
    (void)idx; if (r) *r = 0; if (g) *g = 0; if (b) *b = 0; if (a) *a = 100;
}
void osd_ed_set_color(int idx, int r, int g, int b, int a) { (void)idx; (void)r; (void)g; (void)b; (void)a; }

// ranges_and_icons: sim widget 1 (RSSI/IconSelector) exposes 2 fake ranges.
static int sim_has_ranges(int idx) { return valid(idx) && strcmp(g_w[idx].type, "IconSelectorWidget") == 0; }
static long g_range[2][2] = { {-60, 1}, {-90, -61} };
int osd_ed_has_ranges(int idx) { return sim_has_ranges(idx) ? 1 : 0; }
int osd_ed_range_count(int idx) { return sim_has_ranges(idx) ? 2 : 0; }
long osd_ed_range_min(int idx, int r) { (void)idx; return (r >= 0 && r < 2) ? g_range[r][0] : 0; }
long osd_ed_range_max(int idx, int r) { (void)idx; return (r >= 0 && r < 2) ? g_range[r][1] : 0; }
void osd_ed_set_range_min(int idx, int r, long v) { (void)idx; if (r >= 0 && r < 2) g_range[r][0] = v; }
void osd_ed_set_range_max(int idx, int r, long v) { (void)idx; if (r >= 0 && r < 2) g_range[r][1] = v; }
const char *osd_ed_range_icon(int idx, int r) { (void)idx; return (r == 0) ? "signal1.png" : "signal8.png"; }
void osd_ed_set_range_icon(int idx, int r, const char *icon) { (void)idx; (void)r; (void)icon; }
void osd_ed_add_range(int idx) { (void)idx; }
void osd_ed_del_range(int idx, int r) { (void)idx; (void)r; }

int osd_ed_str_count(int idx) { return valid(idx) ? 1 : 0; }
const char *osd_ed_str_key(int idx, int f) { (void)idx; (void)f; return "template"; }
const char *osd_ed_str_value(int idx, int f) { (void)idx; (void)f; return "%f Mbps"; }
static char g_str_ret[128];
const char *osd_ed_get_string(int idx, const char *key) {
    if (!valid(idx) || !key) return "";
    if (strcmp(key, "name") == 0) return g_w[idx].name;
    if (strcmp(key, "template") == 0) return "%f Mbps";
    if (strcmp(key, "text") == 0) return "Label";
    if (strcmp(key, "icon_path") == 0) return "network.png";
    g_str_ret[0] = 0;
    return g_str_ret;
}
void osd_ed_set_string(int idx, const char *key, const char *val) {
    if (valid(idx) && key && val && strcmp(key, "name") == 0)
        strncpy(g_w[idx].name, val, sizeof(g_w[idx].name) - 1);
}
const char *osd_ed_reg_icons(void) {
    return "framerate.png\nnetwork.png\nlatency.png\nmemory.png\nsdcard-white.png\nsignal1.png\nsignal8.png";
}

int osd_ed_fact_count(int idx) { return valid(idx) ? 1 : 0; }
const char *osd_ed_fact_name(int idx, int f) { (void)f; return valid(idx) ? g_w[idx].fact : ""; }
void osd_ed_set_fact_name(int idx, int f, const char *name) {
    (void)f;
    if (valid(idx) && name) { strncpy(g_w[idx].fact, name, sizeof(g_w[idx].fact) - 1); g_w[idx].has_tag = 0; }
}
int osd_ed_fact_tag_count(int idx, int f) { (void)f; return (valid(idx) && g_w[idx].has_tag) ? 1 : 0; }
const char *osd_ed_fact_tag_key(int idx, int f, int t) { (void)f; (void)t; return valid(idx) ? g_w[idx].tag_key : ""; }
const char *osd_ed_fact_tag_value(int idx, int f, int t) { (void)f; (void)t; return valid(idx) ? g_w[idx].tag_val : ""; }
void osd_ed_set_fact_tag(int idx, int f, const char *key, const char *val) {
    (void)f; (void)key;
    if (valid(idx) && val) strncpy(g_w[idx].tag_val, val, sizeof(g_w[idx].tag_val) - 1);
}
void osd_ed_add_fact(int idx) { (void)idx; }
void osd_ed_del_fact(int idx, int f) { (void)idx; (void)f; }
void osd_ed_add_fact_tag(int idx, int f, const char *key) {
    (void)f;
    if (valid(idx) && key) { strncpy(g_w[idx].tag_key, key, sizeof(g_w[idx].tag_key) - 1); g_w[idx].has_tag = 1; g_w[idx].tag_val[0] = 0; }
}
void osd_ed_del_fact_tag(int idx, int f, const char *key) {
    (void)f; (void)key;
    if (valid(idx)) g_w[idx].has_tag = 0;
}
const char *osd_ed_avail_tag_keys(int idx, int f) { (void)idx; (void)f; return "ant_id\nid"; }
const char *osd_ed_fact_convert(int idx, int f) { (void)idx; (void)f; return ""; }
void osd_ed_set_fact_convert(int idx, int f, const char *expr) { (void)idx; (void)f; (void)expr; }

const char *osd_ed_reg_fact_names(void) {
    return "gstreamer.received_bytes\nwfbcli.rx.ant_stats.rssi_avg\nvideo.width\nvideo.height";
}
const char *osd_ed_reg_tag_keys(const char *fact) { (void)fact; return "ant_id\nid"; }
const char *osd_ed_reg_tag_values(const char *fact, const char *key) {
    (void)fact;
    if (key && strcmp(key, "id") == 0) return "video rx\ntunnel rx";
    return "0\n1\n256\n257";
}

int osd_ed_preset_count(void) { return g_preset_count; }
const char *osd_ed_preset_label(int i) { return (i >= 0 && i < g_preset_count) ? g_preset_labels[i] : ""; }
int osd_ed_add_preset(int i) {
    if (g_count >= MAX_W) return -1;
    snprintf(g_w[g_count].name, sizeof(g_w[g_count].name), "%s",
             (i >= 0 && i < g_preset_count) ? g_preset_labels[i] : "New");
    snprintf(g_w[g_count].type, sizeof(g_w[g_count].type), "TplTextWidget");
    g_w[g_count].x = 50; g_w[g_count].y = 50;
    g_w[g_count].fact[0] = 0; g_w[g_count].has_tag = 0;
    return g_count++;
}

int osd_ed_move(int idx, int dir) {
    int j = idx + dir;
    if (!valid(idx) || !valid(j)) return idx;
    sim_widget_t tmp = g_w[idx]; g_w[idx] = g_w[j]; g_w[j] = tmp;
    return j;
}
void osd_ed_delete(int idx) {
    if (!valid(idx)) return;
    for (int i = idx; i < g_count - 1; i++) g_w[i] = g_w[i + 1];
    g_count--;
}
void osd_ed_save(void) { printf("[sim] osd_ed_save()\n"); }
