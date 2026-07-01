#include <stdio.h>
#include <stdlib.h>
#include <stdint.h>
#include <string.h>

#include "lvgl/lvgl.h"

#include "../input.h"
#include "../osd_editor.h"
#include "helper.h"
#include "styles.h"
#include "gs_osd.h"

extern lv_group_t *default_group;
extern lv_indev_t *indev_drv;
extern gsmenu_control_mode_t control_mode;

// ─── Views ──────────────────────────────────────────────────────────────────
typedef enum {
    OSD_VIEW_LIST = 0,   // list of widgets + add / save
    OSD_VIEW_DETAIL,     // edit a single widget
    OSD_VIEW_ADD,        // pick a preset to add
} osd_view_t;

static lv_obj_t *g_parent_page;
static menu_page_data_t *g_menu_page_data;
static lv_obj_t *g_section;
static lv_obj_t *g_keyboard;         // shared on-screen keyboard (hidden until used)
static osd_view_t g_view = OSD_VIEW_LIST;
static int g_current_idx = -1;
static bool g_rebuild_pending = false;

static void build_view(void);

// What a text field commits to when the keyboard closes.
typedef enum { TXT_STRING = 0, TXT_CONVERT } osd_text_kind_t;

// Per-control context, freed automatically when its widget is deleted.
typedef struct {
    int  idx;              // widget index
    int  f;                // fact index (fact controls)
    char key[64];          // numeric/string field key, or tag key
    long val;              // current numeric value (+/- rows)
    long step;
    long minv, maxv;
    int  color_comp;       // 0..3 for r/g/b/a, else -1
    osd_text_kind_t tkind; // for text rows
    lv_obj_t *vlabel;      // value label to update in place
} osd_ctx_t;

static void free_ud_cb(lv_event_t *e) { free(lv_event_get_user_data(e)); }

static osd_ctx_t *mk_ctx(void) {
    osd_ctx_t *c = calloc(1, sizeof(osd_ctx_t));
    if (c) c->color_comp = -1;
    return c;
}

// ─── Deferred rebuild ───────────────────────────────────────────────────────
// Structural edits delete the very objects whose event we are handling, so we
// always rebuild from an lv_async_call after the event returns.
static void rebuild_async(void *unused) {
    (void)unused;
    g_rebuild_pending = false;
    build_view();
}
static void request_rebuild(void) {
    if (g_rebuild_pending) return;
    g_rebuild_pending = true;
    lv_async_call(rebuild_async, NULL);
}

// ─── Small styled building blocks ───────────────────────────────────────────
static lv_obj_t *osd_button(lv_obj_t *parent, const char *txt, lv_event_cb_t cb, void *ud) {
    lv_obj_t *row = create_button(parent, txt);
    lv_obj_t *btn = lv_obj_get_child_by_type(row, 0, &lv_button_class);
    if (cb) lv_obj_add_event_cb(btn, cb, LV_EVENT_CLICKED, ud);
    return btn;
}

// Build a dropdown option list that always contains `current`, and select it.
static void dropdown_apply(lv_obj_t *dd, const char *options, const char *current) {
    static char buf[4096];
    size_t pos = 0;
    int idx = 0, found = -1;
    bool cur_valid = current && current[0];

    buf[0] = 0;
    if (!cur_valid) {
        pos += snprintf(buf + pos, sizeof(buf) - pos, "%s", "(choose)");
        found = 0;
        idx = 1;
    }
    const char *p = options ? options : "";
    while (*p) {
        const char *nl = strchr(p, '\n');
        size_t len = nl ? (size_t)(nl - p) : strlen(p);
        if (pos && pos < sizeof(buf) - 1) buf[pos++] = '\n';
        if (pos + len < sizeof(buf)) {
            memcpy(buf + pos, p, len);
            pos += len;
            buf[pos] = 0;
        }
        if (cur_valid && found < 0 && len == strlen(current) && strncmp(p, current, len) == 0)
            found = idx;
        idx++;
        if (!nl) break;
        p = nl + 1;
    }
    if (cur_valid && found < 0) { // current value not in the live registry
        if (pos && pos < sizeof(buf) - 1) buf[pos++] = '\n';
        pos += snprintf(buf + pos, sizeof(buf) - pos, "%s", current);
        found = idx;
    }
    if (buf[0] == 0) snprintf(buf, sizeof(buf), "(none available)");
    lv_dropdown_set_options(dd, buf);
    if (found >= 0) lv_dropdown_set_selected(dd, found);
}

static lv_obj_t *osd_dropdown(lv_obj_t *parent, const char *label_txt,
                              const char *options, const char *current,
                              lv_event_cb_t cb, void *ud) {
    lv_obj_t *obj = lv_menu_cont_create(parent);

    lv_obj_t *label = lv_label_create(obj);
    lv_label_set_text(label, label_txt);
    lv_obj_set_flex_grow(label, 1);

    lv_obj_t *dd = lv_dropdown_create(obj);
    // Open the list to the LEFT so it renders on-screen (rows sit at the right
    // edge; a right-opening list would fall off the display).
    lv_dropdown_set_dir(dd, LV_DIR_LEFT);
    lv_dropdown_set_symbol(dd, LV_SYMBOL_LEFT);
    lv_obj_set_width(dd, 320);
    lv_obj_add_style(dd, &style_openipc_outline, LV_PART_MAIN | LV_STATE_FOCUS_KEY);
    lv_obj_add_style(dd, &style_openipc_dark_background, LV_PART_MAIN | LV_STATE_DEFAULT);
    lv_obj_t *list = lv_dropdown_get_list(dd);
    lv_obj_add_style(list, &style_openipc, LV_PART_SELECTED | LV_STATE_CHECKED);
    lv_obj_add_style(list, &style_openipc_dark_background, LV_PART_MAIN | LV_STATE_DEFAULT);

    dropdown_apply(dd, options, current);

    // dropdown_event_handler drives the EDIT/NAV control mode so the joystick
    // can open and scroll the list; generic_back_event_handler handles HOME.
    lv_obj_add_event_cb(dd, dropdown_event_handler, LV_EVENT_ALL, NULL);
    lv_obj_add_event_cb(dd, generic_back_event_handler, LV_EVENT_KEY, NULL);
    lv_obj_add_event_cb(dd, on_focus, LV_EVENT_FOCUSED, NULL);
    if (cb) lv_obj_add_event_cb(dd, cb, LV_EVENT_VALUE_CHANGED, ud);
    if (ud) lv_obj_add_event_cb(dd, free_ud_cb, LV_EVENT_DELETE, ud);
    return dd;
}

// ─── Text input (on-screen keyboard) ────────────────────────────────────────
static void kb_event_cb(lv_event_t *e) {
    lv_event_code_t code = lv_event_get_code(e);
    lv_obj_t *kb = lv_event_get_user_data(e);

    if (code == LV_EVENT_FOCUSED) {
        control_mode = GSMENU_CONTROL_MODE_KEYBOARD;
    } else if (code == LV_EVENT_DEFOCUSED) {
        control_mode = GSMENU_CONTROL_MODE_NAV;
    } else if (code == LV_EVENT_READY || code == LV_EVENT_CANCEL) {
        lv_obj_t *ta = lv_keyboard_get_textarea(kb);
        if (code == LV_EVENT_READY && ta) {
            osd_ctx_t *c = lv_obj_get_user_data(ta);
            const char *text = lv_textarea_get_text(ta);
            if (c) {
                if (c->tkind == TXT_CONVERT) osd_ed_set_fact_convert(c->idx, c->f, text);
                else                          osd_ed_set_string(c->idx, c->key, text);
            }
        }
        lv_obj_add_flag(kb, LV_OBJ_FLAG_HIDDEN);
        control_mode = GSMENU_CONTROL_MODE_NAV;
        // The edit may change the row layout, so rebuild the whole view.
        request_rebuild();
    }
}

// Keyboard button: bind the shared keyboard to this row's textarea and show it.
static void kb_open_cb(lv_event_t *e) {
    if (lv_event_get_code(e) != LV_EVENT_CLICKED) return;
    lv_obj_t *btn = lv_event_get_target(e);
    lv_obj_t *ta = lv_obj_get_user_data(btn);
    if (!ta || !g_keyboard) return;
    lv_keyboard_set_textarea(g_keyboard, ta);
    lv_obj_remove_flag(g_keyboard, LV_OBJ_FLAG_HIDDEN);
    lv_obj_move_foreground(g_keyboard);   // draw over the (scrolled) content
    lv_obj_scroll_to_view_recursive(ta, LV_ANIM_OFF);
    lv_indev_wait_release(lv_event_get_param(e));
    lv_group_focus_obj(g_keyboard);
}

// A row: "<label>"  [ textarea (read-only display) ]  [ keyboard ]
static void add_text_row(lv_obj_t *parent, const char *label_txt, const char *value,
                         osd_ctx_t *c) {
    lv_obj_t *obj = lv_menu_cont_create(parent);

    lv_obj_t *label = lv_label_create(obj);
    lv_label_set_text(label, label_txt);

    lv_obj_t *ta = lv_textarea_create(obj);
    lv_textarea_set_one_line(ta, true);
    lv_textarea_set_text(ta, value ? value : "");
    lv_obj_set_flex_grow(ta, 1);
    lv_obj_add_state(ta, LV_STATE_DISABLED);
    lv_obj_add_style(ta, &style_openipc_outline, LV_PART_MAIN | LV_STATE_FOCUS_KEY);
    lv_obj_add_style(ta, &style_openipc_lightdark_background, LV_PART_MAIN | LV_STATE_DEFAULT);
    lv_obj_set_user_data(ta, c);
    lv_obj_add_event_cb(ta, free_ud_cb, LV_EVENT_DELETE, c);

    lv_obj_t *btn = lv_button_create(obj);
    lv_obj_set_user_data(btn, ta);
    lv_obj_add_style(btn, &style_openipc, LV_PART_MAIN | LV_STATE_DEFAULT);
    lv_obj_add_style(btn, &style_openipc_outline, LV_PART_MAIN | LV_STATE_FOCUS_KEY);
    lv_label_set_text(lv_label_create(btn), LV_SYMBOL_KEYBOARD);
    lv_obj_add_event_cb(btn, kb_open_cb, LV_EVENT_CLICKED, NULL);
    lv_obj_add_event_cb(btn, generic_back_event_handler, LV_EVENT_KEY, NULL);
    lv_obj_add_event_cb(btn, on_focus, LV_EVENT_FOCUSED, NULL);
}

// ─── Numeric / color +/- rows ───────────────────────────────────────────────
static void numctx_apply(osd_ctx_t *c) {
    if (c->color_comp >= 0) {
        int rgba[4];
        osd_ed_get_color(c->idx, &rgba[0], &rgba[1], &rgba[2], &rgba[3]);
        rgba[c->color_comp] = (int)c->val;
        osd_ed_set_color(c->idx, rgba[0], rgba[1], rgba[2], rgba[3]);
    } else if (strcmp(c->key, "x") == 0 || strcmp(c->key, "y") == 0) {
        int x = 0, y = 0;
        osd_ed_get_xy(c->idx, &x, &y);
        if (c->key[0] == 'x') x = (int)c->val; else y = (int)c->val;
        osd_ed_set_xy(c->idx, x, y);
    } else if (strcmp(c->key, "rmin") == 0) {
        osd_ed_set_range_min(c->idx, c->f, c->val);   // c->f = range index
    } else if (strcmp(c->key, "rmax") == 0) {
        osd_ed_set_range_max(c->idx, c->f, c->val);
    } else {
        osd_ed_set_num(c->idx, c->key, c->val);
    }
    if (c->vlabel) {
        char b[32];
        snprintf(b, sizeof(b), "%ld", c->val);
        lv_label_set_text(c->vlabel, b);
    }
}
static void num_dec_cb(lv_event_t *e) {
    osd_ctx_t *c = lv_event_get_user_data(e);
    c->val -= c->step;
    if (c->val < c->minv) c->val = c->minv;
    numctx_apply(c);
}
static void num_inc_cb(lv_event_t *e) {
    osd_ctx_t *c = lv_event_get_user_data(e);
    c->val += c->step;
    if (c->val > c->maxv) c->val = c->maxv;
    numctx_apply(c);
}

// A row: "<title>"  [ - ]  <value>  [ + ]
static void add_num_row(lv_obj_t *parent, const char *title, osd_ctx_t *c) {
    lv_obj_t *row = lv_menu_cont_create(parent);

    lv_obj_t *label = lv_label_create(row);
    lv_label_set_text(label, title);
    lv_obj_set_flex_grow(label, 1);

    lv_obj_t *minus = lv_btn_create(row);
    lv_obj_add_style(minus, &style_openipc, LV_PART_MAIN | LV_STATE_DEFAULT);
    lv_obj_add_style(minus, &style_openipc_outline, LV_PART_MAIN | LV_STATE_FOCUS_KEY);
    lv_label_set_text(lv_label_create(minus), LV_SYMBOL_MINUS);
    lv_obj_add_event_cb(minus, num_dec_cb, LV_EVENT_CLICKED, c);
    lv_obj_add_event_cb(minus, num_dec_cb, LV_EVENT_LONG_PRESSED_REPEAT, c);
    lv_obj_add_event_cb(minus, generic_back_event_handler, LV_EVENT_KEY, NULL);
    lv_obj_add_event_cb(minus, on_focus, LV_EVENT_FOCUSED, NULL);

    c->vlabel = lv_label_create(row);
    char b[32];
    snprintf(b, sizeof(b), "%ld", c->val);
    lv_label_set_text(c->vlabel, b);

    lv_obj_t *plus = lv_btn_create(row);
    lv_obj_add_style(plus, &style_openipc, LV_PART_MAIN | LV_STATE_DEFAULT);
    lv_obj_add_style(plus, &style_openipc_outline, LV_PART_MAIN | LV_STATE_FOCUS_KEY);
    lv_label_set_text(lv_label_create(plus), LV_SYMBOL_PLUS);
    lv_obj_add_event_cb(plus, num_inc_cb, LV_EVENT_CLICKED, c);
    lv_obj_add_event_cb(plus, num_inc_cb, LV_EVENT_LONG_PRESSED_REPEAT, c);
    lv_obj_add_event_cb(plus, generic_back_event_handler, LV_EVENT_KEY, NULL);
    lv_obj_add_event_cb(plus, on_focus, LV_EVENT_FOCUSED, NULL);

    // The context is owned here; free it when the value label is destroyed.
    lv_obj_add_event_cb(c->vlabel, free_ud_cb, LV_EVENT_DELETE, c);
}

// Reasonable step/range per numeric key.
static void num_range_for(const char *key, long *step, long *minv, long *maxv) {
    *step = 1; *minv = -10000; *maxv = 10000;
    if (!strcmp(key, "width") || !strcmp(key, "height")) { *step = 5; *minv = 0; *maxv = 4000; }
    else if (!strcmp(key, "timeout_ms")) { *step = 250; *minv = 0; *maxv = 60000; }
    else if (!strcmp(key, "per_second_window_s")) { *step = 1; *minv = 1; *maxv = 30; }
    else if (!strcmp(key, "per_second_bucket_ms")) { *step = 10; *minv = 10; *maxv = 2000; }
    else if (!strcmp(key, "num_buckets")) { *step = 1; *minv = 1; *maxv = 200; }
    else if (!strcmp(key, "window_s")) { *step = 1; *minv = 1; *maxv = 60; }
}

// ─── Callbacks: navigation / structure ──────────────────────────────────────
static void open_detail_cb(lv_event_t *e) {
    g_current_idx = (int)(intptr_t)lv_event_get_user_data(e);
    g_view = OSD_VIEW_DETAIL;
    request_rebuild();
}
static void back_to_list_cb(lv_event_t *e) {
    (void)e;
    g_view = OSD_VIEW_LIST;
    request_rebuild();
}
static void open_add_cb(lv_event_t *e) {
    (void)e;
    g_view = OSD_VIEW_ADD;
    request_rebuild();
}
static void add_preset_cb(lv_event_t *e) {
    int preset = (int)(intptr_t)lv_event_get_user_data(e);
    int idx = osd_ed_add_preset(preset);
    if (idx >= 0) { g_current_idx = idx; g_view = OSD_VIEW_DETAIL; }
    else          { g_view = OSD_VIEW_LIST; }
    request_rebuild();
}
static void save_cb(lv_event_t *e) {
    (void)e;
    osd_ed_save();
    show_restart_notice();
}
static void position_cb(lv_event_t *e) {
    (void)e;
    osd_ed_position_set_target(g_current_idx);
    // request_rebuild refreshes the x/y labels once we return from position mode.
    osd_position_enter(g_menu_page_data->indev_group, request_rebuild);
}
static void move_up_cb(lv_event_t *e) {
    (void)e;
    g_current_idx = osd_ed_move(g_current_idx, -1);
    request_rebuild();
}
static void move_down_cb(lv_event_t *e) {
    (void)e;
    g_current_idx = osd_ed_move(g_current_idx, +1);
    request_rebuild();
}
static void delete_cb(lv_event_t *e) {
    (void)e;
    osd_ed_delete(g_current_idx);
    g_view = OSD_VIEW_LIST;
    request_rebuild();
}

// ─── Callbacks: string / icon / facts ───────────────────────────────────────
static void icon_cb(lv_event_t *e) {
    osd_ctx_t *c = lv_event_get_user_data(e);
    lv_obj_t *dd = lv_event_get_target(e);
    char sel[128];
    lv_dropdown_get_selected_str(dd, sel, sizeof(sel));
    if (sel[0] == '(') return;
    osd_ed_set_string(c->idx, c->key, sel);
}
static void range_icon_cb(lv_event_t *e) {
    osd_ctx_t *c = lv_event_get_user_data(e);
    lv_obj_t *dd = lv_event_get_target(e);
    char sel[128];
    lv_dropdown_get_selected_str(dd, sel, sizeof(sel));
    if (sel[0] == '(') return;
    osd_ed_set_range_icon(c->idx, c->f, sel);
}
static void add_range_cb(lv_event_t *e) {
    (void)e;
    osd_ed_add_range(g_current_idx);
    request_rebuild();
}
static void del_range_cb(lv_event_t *e) {
    osd_ctx_t *c = lv_event_get_user_data(e);
    osd_ed_del_range(c->idx, c->f);
    request_rebuild();
}
static void fact_name_cb(lv_event_t *e) {
    osd_ctx_t *c = lv_event_get_user_data(e);
    lv_obj_t *dd = lv_event_get_target(e);
    char sel[128];
    lv_dropdown_get_selected_str(dd, sel, sizeof(sel));
    if (sel[0] == '(') return;
    osd_ed_set_fact_name(c->idx, c->f, sel);
    request_rebuild(); // tag rows depend on the chosen fact
}
static void fact_tag_cb(lv_event_t *e) {
    osd_ctx_t *c = lv_event_get_user_data(e);
    lv_obj_t *dd = lv_event_get_target(e);
    char sel[128];
    lv_dropdown_get_selected_str(dd, sel, sizeof(sel));
    if (sel[0] == '(') return;
    osd_ed_set_fact_tag(c->idx, c->f, c->key, sel);
}
static void add_tag_cb(lv_event_t *e) {
    osd_ctx_t *c = lv_event_get_user_data(e);
    lv_obj_t *dd = lv_event_get_target(e);
    char sel[128];
    lv_dropdown_get_selected_str(dd, sel, sizeof(sel));
    if (sel[0] == '(') return;
    osd_ed_add_fact_tag(c->idx, c->f, sel);
    request_rebuild();
}
static void del_tag_cb(lv_event_t *e) {
    osd_ctx_t *c = lv_event_get_user_data(e);
    osd_ed_del_fact_tag(c->idx, c->f, c->key);
    request_rebuild();
}
static void add_fact_cb(lv_event_t *e) {
    (void)e;
    osd_ed_add_fact(g_current_idx);
    request_rebuild();
}
static void del_fact_cb(lv_event_t *e) {
    osd_ctx_t *c = lv_event_get_user_data(e);
    osd_ed_del_fact(c->idx, c->f);
    request_rebuild();
}

// ─── View builders ──────────────────────────────────────────────────────────
static void build_list(void) {
    create_text(g_section, LV_SYMBOL_LIST, "OSD Widgets", NULL, NULL, false,
                LV_MENU_ITEM_BUILDER_VARIANT_1);

    osd_button(g_section, LV_SYMBOL_PLUS "  Add widget", open_add_cb, NULL);

    int n = osd_ed_count();
    if (n == 0)
        create_text(g_section, LV_SYMBOL_WARNING, "No widgets", NULL, NULL, false,
                    LV_MENU_ITEM_BUILDER_VARIANT_1);
    for (int i = 0; i < n; i++) {
        char label[160];
        snprintf(label, sizeof(label), LV_SYMBOL_EDIT "  %s", osd_ed_label(i));
        osd_button(g_section, label, open_detail_cb, (void *)(intptr_t)i);
    }

    osd_button(g_section, LV_SYMBOL_SAVE "  Save to file", save_cb, NULL);
}

static void build_add(void) {
    create_text(g_section, LV_SYMBOL_PLUS, "Add widget", NULL, NULL, false,
                LV_MENU_ITEM_BUILDER_VARIANT_1);
    osd_button(g_section, LV_SYMBOL_LEFT "  Back", back_to_list_cb, NULL);

    int n = osd_ed_preset_count();
    for (int i = 0; i < n; i++)
        osd_button(g_section, osd_ed_preset_label(i), add_preset_cb, (void *)(intptr_t)i);
}

static void build_fact(int idx, int f) {
    // Fact name
    char curname[128];
    strncpy(curname, osd_ed_fact_name(idx, f), sizeof(curname) - 1);
    curname[sizeof(curname) - 1] = 0;

    osd_ctx_t *fctx = mk_ctx();
    fctx->idx = idx; fctx->f = f;
    char lbl[32];
    snprintf(lbl, sizeof(lbl), "Fact %d name", f + 1);
    osd_dropdown(g_section, lbl, osd_ed_reg_fact_names(), curname, fact_name_cb, fctx);

    // Existing tags: value dropdown + remove button
    int tc = osd_ed_fact_tag_count(idx, f);
    for (int t = 0; t < tc; t++) {
        char key[64], curval[128];
        strncpy(key, osd_ed_fact_tag_key(idx, f, t), sizeof(key) - 1);
        key[sizeof(key) - 1] = 0;
        strncpy(curval, osd_ed_fact_tag_value(idx, f, t), sizeof(curval) - 1);
        curval[sizeof(curval) - 1] = 0;

        osd_ctx_t *tctx = mk_ctx();
        tctx->idx = idx; tctx->f = f;
        strncpy(tctx->key, key, sizeof(tctx->key) - 1);
        char taglbl[96];
        snprintf(taglbl, sizeof(taglbl), "  tag %s", key);
        osd_dropdown(g_section, taglbl, osd_ed_reg_tag_values(curname, key), curval, fact_tag_cb, tctx);

        osd_ctx_t *rmctx = mk_ctx();
        rmctx->idx = idx; rmctx->f = f;
        strncpy(rmctx->key, key, sizeof(rmctx->key) - 1);
        char rmlbl[96];
        snprintf(rmlbl, sizeof(rmlbl), LV_SYMBOL_MINUS "  remove tag %s", key);
        lv_obj_t *rmbtn = osd_button(g_section, rmlbl, del_tag_cb, rmctx);
        lv_obj_add_event_cb(rmbtn, free_ud_cb, LV_EVENT_DELETE, rmctx);
    }

    // Add-tag dropdown (only keys the live fact exposes that aren't set yet)
    const char *avail = osd_ed_avail_tag_keys(idx, f);
    if (avail && avail[0]) {
        osd_ctx_t *atctx = mk_ctx();
        atctx->idx = idx; atctx->f = f;
        osd_dropdown(g_section, "  add tag", avail, "", add_tag_cb, atctx);
    }

    // Convert expression (text)
    osd_ctx_t *cvctx = mk_ctx();
    cvctx->idx = idx; cvctx->f = f; cvctx->tkind = TXT_CONVERT;
    add_text_row(g_section, "  convert", osd_ed_fact_convert(idx, f), cvctx);

    // Remove this fact
    osd_ctx_t *dfctx = mk_ctx();
    dfctx->idx = idx; dfctx->f = f;
    lv_obj_t *dfbtn = osd_button(g_section, LV_SYMBOL_TRASH "  Remove fact", del_fact_cb, dfctx);
    lv_obj_add_event_cb(dfbtn, free_ud_cb, LV_EVENT_DELETE, dfctx);
}

// IconSelectorWidget: edit each [min,max] range and its icon.
static void build_ranges(int idx) {
    create_text(g_section, LV_SYMBOL_IMAGE, "Icon ranges (value -> icon)", NULL, NULL, false,
                LV_MENU_ITEM_BUILDER_VARIANT_1);
    int rc = osd_ed_range_count(idx);
    for (int r = 0; r < rc; r++) {
        char hdr[32];
        snprintf(hdr, sizeof(hdr), "Range %d", r + 1);
        create_text(g_section, NULL, hdr, NULL, NULL, false, LV_MENU_ITEM_BUILDER_VARIANT_1);

        osd_ctx_t *cmin = mk_ctx();
        cmin->idx = idx; cmin->f = r; strcpy(cmin->key, "rmin");
        cmin->val = osd_ed_range_min(idx, r); cmin->step = 1; cmin->minv = -300; cmin->maxv = 300;
        add_num_row(g_section, "  min", cmin);

        osd_ctx_t *cmax = mk_ctx();
        cmax->idx = idx; cmax->f = r; strcpy(cmax->key, "rmax");
        cmax->val = osd_ed_range_max(idx, r); cmax->step = 1; cmax->minv = -300; cmax->maxv = 300;
        add_num_row(g_section, "  max", cmax);

        char curicon[128];
        strncpy(curicon, osd_ed_range_icon(idx, r), sizeof(curicon) - 1);
        curicon[sizeof(curicon) - 1] = 0;
        osd_ctx_t *cic = mk_ctx();
        cic->idx = idx; cic->f = r;
        osd_dropdown(g_section, "  icon", osd_ed_reg_icons(), curicon, range_icon_cb, cic);

        osd_ctx_t *cdel = mk_ctx();
        cdel->idx = idx; cdel->f = r;
        lv_obj_t *db = osd_button(g_section, LV_SYMBOL_MINUS "  remove range", del_range_cb, cdel);
        lv_obj_add_event_cb(db, free_ud_cb, LV_EVENT_DELETE, cdel);
    }
    osd_button(g_section, LV_SYMBOL_PLUS "  Add range", add_range_cb, NULL);
}

static void build_detail(int idx) {
    if (idx < 0 || idx >= osd_ed_count()) {
        g_view = OSD_VIEW_LIST;
        build_list();
        return;
    }

    create_text(g_section, LV_SYMBOL_EDIT, osd_ed_label(idx), NULL, NULL, false,
                LV_MENU_ITEM_BUILDER_VARIANT_1);
    osd_button(g_section, LV_SYMBOL_LEFT "  Back to list", back_to_list_cb, NULL);

    // Name (text)
    osd_ctx_t *namectx = mk_ctx();
    namectx->idx = idx; namectx->tkind = TXT_STRING; strcpy(namectx->key, "name");
    add_text_row(g_section, "Name", osd_ed_name(idx), namectx);

    // Position
    int x = 0, y = 0;
    osd_ed_get_xy(idx, &x, &y);
    osd_button(g_section, LV_SYMBOL_GPS "  Move on screen", position_cb, NULL);
    {
        osd_ctx_t *cx = mk_ctx();
        cx->idx = idx; strcpy(cx->key, "x"); cx->val = x; cx->step = 1; cx->minv = -4000; cx->maxv = 4000;
        add_num_row(g_section, "X", cx);
        osd_ctx_t *cy = mk_ctx();
        cy->idx = idx; strcpy(cy->key, "y"); cy->val = y; cy->step = 1; cy->minv = -4000; cy->maxv = 4000;
        add_num_row(g_section, "Y", cy);
    }

    // Numeric scalar fields
    int nn = osd_ed_num_count(idx);
    for (int f = 0; f < nn; f++) {
        char keybuf[64];
        strncpy(keybuf, osd_ed_num_key(idx, f), sizeof(keybuf) - 1);
        keybuf[sizeof(keybuf) - 1] = 0;
        osd_ctx_t *c = mk_ctx();
        c->idx = idx;
        strncpy(c->key, keybuf, sizeof(c->key) - 1);
        c->val = osd_ed_num_value(idx, f);
        num_range_for(keybuf, &c->step, &c->minv, &c->maxv);
        add_num_row(g_section, keybuf, c);
    }

    // Color
    if (osd_ed_has_color(idx)) {
        int rgba[4];
        osd_ed_get_color(idx, &rgba[0], &rgba[1], &rgba[2], &rgba[3]);
        const char *names[4] = {"Red %", "Green %", "Blue %", "Alpha %"};
        for (int comp = 0; comp < 4; comp++) {
            osd_ctx_t *c = mk_ctx();
            c->idx = idx; c->color_comp = comp; c->val = rgba[comp];
            c->step = 5; c->minv = 0; c->maxv = 100;
            add_num_row(g_section, names[comp], c);
        }
    }

    // String fields: icon_path -> dropdown of assets; others -> text input.
    int sc = osd_ed_str_count(idx);
    for (int f = 0; f < sc; f++) {
        char key[64], val[192];
        strncpy(key, osd_ed_str_key(idx, f), sizeof(key) - 1);
        key[sizeof(key) - 1] = 0;
        strncpy(val, osd_ed_str_value(idx, f), sizeof(val) - 1);
        val[sizeof(val) - 1] = 0;

        if (strcmp(key, "icon_path") == 0) {
            osd_ctx_t *ic = mk_ctx();
            ic->idx = idx; strcpy(ic->key, "icon_path");
            osd_dropdown(g_section, "icon", osd_ed_reg_icons(), val, icon_cb, ic);
        } else {
            osd_ctx_t *tc = mk_ctx();
            tc->idx = idx; tc->tkind = TXT_STRING;
            strncpy(tc->key, key, sizeof(tc->key) - 1);
            add_text_row(g_section, key, val, tc);
        }
    }

    // Icon ranges (IconSelectorWidget)
    if (osd_ed_has_ranges(idx)) build_ranges(idx);

    // Facts
    create_text(g_section, LV_SYMBOL_SHUFFLE, "Facts", NULL, NULL, false,
                LV_MENU_ITEM_BUILDER_VARIANT_1);
    int fc = osd_ed_fact_count(idx);
    for (int f = 0; f < fc; f++) build_fact(idx, f);
    osd_button(g_section, LV_SYMBOL_PLUS "  Add fact", add_fact_cb, NULL);

    // Order / delete
    osd_button(g_section, LV_SYMBOL_UP "  Move up (z-order)", move_up_cb, NULL);
    osd_button(g_section, LV_SYMBOL_DOWN "  Move down (z-order)", move_down_cb, NULL);
    osd_button(g_section, LV_SYMBOL_TRASH "  Delete widget", delete_cb, NULL);
}

static void build_view(void) {
    control_mode = GSMENU_CONTROL_MODE_NAV;

    if (g_section && lv_obj_is_valid(g_section)) {
        lv_obj_del(g_section);
        g_section = NULL;
    }

    // New focusable objects auto-register into the page group (and so receive
    // HOME/back key events) while it is the default group.
    lv_group_set_default(g_menu_page_data->indev_group);

    g_section = lv_menu_section_create(g_parent_page);
    lv_obj_add_style(g_section, &style_openipc_section, 0);

    switch (g_view) {
        case OSD_VIEW_ADD:    build_add(); break;
        case OSD_VIEW_DETAIL: build_detail(g_current_idx); break;
        case OSD_VIEW_LIST:
        default:              build_list(); break;
    }

    lv_group_set_default(default_group);

    lv_obj_t *first = find_first_focusable_obj(g_section);
    if (first) lv_group_focus_obj(first);
}

// ─── Public entry points ────────────────────────────────────────────────────
void gs_osd_page_refresh(lv_obj_t *page) {
    (void)page;
    g_view = OSD_VIEW_LIST;
    g_current_idx = -1;
    build_view();
}

void create_gs_osd_menu(lv_obj_t *parent) {
    menu_page_data_t *menu_page_data = malloc(sizeof(menu_page_data_t));
    strcpy(menu_page_data->type, "gs");
    strcpy(menu_page_data->page, "osd");
    menu_page_data->page_load_callback = gs_osd_page_refresh;
    menu_page_data->indev_group = lv_group_create();
    menu_page_data->entry_count = 0;
    menu_page_data->page_entries = NULL;
    lv_obj_set_user_data(parent, menu_page_data);

    g_parent_page = parent;
    g_menu_page_data = menu_page_data;
    g_section = NULL;
    g_view = OSD_VIEW_LIST;

    // One shared keyboard for all text fields; auto-registers into the page
    // group (created while it is the default group), starts hidden. Styling and
    // wiring mirror the WiFi / APFPV pages so it looks and behaves the same.
    lv_group_set_default(menu_page_data->indev_group);
    g_keyboard = lv_keyboard_create(parent);
    lv_obj_add_flag(g_keyboard, LV_OBJ_FLAG_HIDDEN);
    lv_obj_add_flag(g_keyboard, LV_OBJ_FLAG_SCROLL_ON_FOCUS);
    // Keep it pinned to the visible bottom of the page instead of scrolling
    // away with the (long) content above it.
    lv_obj_add_flag(g_keyboard, LV_OBJ_FLAG_FLOATING);
    lv_obj_align(g_keyboard, LV_ALIGN_BOTTOM_MID, 0, 0);
    lv_obj_add_style(g_keyboard, &style_openipc_outline, LV_PART_MAIN | LV_STATE_FOCUS_KEY);
    lv_obj_add_style(g_keyboard, &style_openipc, LV_PART_ITEMS | LV_STATE_FOCUS_KEY);
    lv_obj_add_style(g_keyboard, &style_openipc_dark_background, LV_PART_ITEMS | LV_STATE_DEFAULT);
    lv_obj_add_style(g_keyboard, &style_openipc_textcolor, LV_PART_ITEMS | LV_STATE_FOCUS_KEY);
    lv_obj_add_style(g_keyboard, &style_openipc_lightdark_background, LV_PART_MAIN | LV_STATE_DEFAULT);
    lv_obj_add_event_cb(g_keyboard, kb_event_cb, LV_EVENT_ALL, g_keyboard);
    lv_keyboard_set_textarea(g_keyboard, NULL);
    lv_group_set_default(default_group);

    build_view();
}
