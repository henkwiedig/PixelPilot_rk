// osd_editor.h
//
// C bridge between the gsmenu OSD editor UI (src/gsmenu/gs_osd.c) and the
// C++ OSD engine (src/osd.cpp). All functions operate on the live OSD config
// and run in the OSD thread, so they may only be called from gsmenu callbacks
// (which execute via lv_task_handler in that same thread).
//
// Strings returned as `const char*` point into a small rotating set of static
// buffers. They are valid until a few subsequent calls return strings; copy
// them (e.g. into an LVGL widget) before making many more calls.

#ifndef OSD_EDITOR_H
#define OSD_EDITOR_H

#ifdef __cplusplus
extern "C" {
#endif

// ---- widget enumeration ----
int         osd_ed_count(void);
const char *osd_ed_type(int idx);
const char *osd_ed_name(int idx);
const char *osd_ed_label(int idx);   // "name (type)" for list rows

// ---- position ----
void osd_ed_get_xy(int idx, int *x, int *y);
void osd_ed_set_xy(int idx, int x, int y);

// On-screen position mode: target selection + relative nudges.
void osd_ed_position_set_target(int idx);
int  osd_ed_position_target(void);
void osd_ed_nudge(int dx, int dy);   // moves the position target

// ---- integer scalar fields (width/height/timeout_ms/...) ----
int         osd_ed_num_count(int idx);
const char *osd_ed_num_key(int idx, int f);
long        osd_ed_num_value(int idx, int f);
void        osd_ed_set_num(int idx, const char *key, long val);

// ---- color object (returns 1 if present); components are 0..100 ----
int  osd_ed_has_color(int idx);
void osd_ed_get_color(int idx, int *r, int *g, int *b, int *a);
void osd_ed_set_color(int idx, int r, int g, int b, int a);

// ---- ranges_and_icons (IconSelectorWidget): per-entry [min,max] + icon ----
int         osd_ed_has_ranges(int idx);
int         osd_ed_range_count(int idx);
long        osd_ed_range_min(int idx, int r);
long        osd_ed_range_max(int idx, int r);
void        osd_ed_set_range_min(int idx, int r, long v);
void        osd_ed_set_range_max(int idx, int r, long v);
const char *osd_ed_range_icon(int idx, int r);
void        osd_ed_set_range_icon(int idx, int r, const char *icon);
void        osd_ed_add_range(int idx);
void        osd_ed_del_range(int idx, int r);

// ---- string fields (text/template/icon_path/...) ----
int         osd_ed_str_count(int idx);
const char *osd_ed_str_key(int idx, int f);
const char *osd_ed_str_value(int idx, int f);
const char *osd_ed_get_string(int idx, const char *key);
void        osd_ed_set_string(int idx, const char *key, const char *val);
// Newline-joined list of *.png files in the assets dir (for icon dropdowns).
const char *osd_ed_reg_icons(void);

// ---- facts (dropdown-driven) ----
int         osd_ed_fact_count(int idx);
const char *osd_ed_fact_name(int idx, int f);
void        osd_ed_set_fact_name(int idx, int f, const char *name);
int         osd_ed_fact_tag_count(int idx, int f);
const char *osd_ed_fact_tag_key(int idx, int f, int t);
const char *osd_ed_fact_tag_value(int idx, int f, int t);
void        osd_ed_set_fact_tag(int idx, int f, const char *key, const char *val);
void        osd_ed_add_fact(int idx);
void        osd_ed_del_fact(int idx, int f);
void        osd_ed_add_fact_tag(int idx, int f, const char *key);
void        osd_ed_del_fact_tag(int idx, int f, const char *key);
const char *osd_ed_avail_tag_keys(int idx, int f);   // tag keys not yet on this fact
const char *osd_ed_fact_convert(int idx, int f);
void        osd_ed_set_fact_convert(int idx, int f, const char *expr);

// ---- live registry of published facts/tags (newline-joined dropdown lists) ----
const char *osd_ed_reg_fact_names(void);
const char *osd_ed_reg_tag_keys(const char *fact);
const char *osd_ed_reg_tag_values(const char *fact, const char *key);

// ---- presets for "Add widget" ----
int         osd_ed_preset_count(void);
const char *osd_ed_preset_label(int i);
int         osd_ed_add_preset(int i);   // returns new widget index, or -1

// ---- structural edits ----
int  osd_ed_move(int idx, int dir);     // dir -1 (up) / +1 (down); returns new index
void osd_ed_delete(int idx);

// ---- persistence ----
void osd_ed_save(void);

#ifdef __cplusplus
}
#endif

#endif // OSD_EDITOR_H
