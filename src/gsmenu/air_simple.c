#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "executor.h"
#include "styles.h"

#include "lvgl/lvgl.h"
#include "helper.h"

extern lv_group_t * default_group;
extern uint32_t MY_EVENT_1;
extern lv_obj_t * air_adaptivelink;

#define ENTRIES 7
lv_obj_t * simple_video_mode;
lv_obj_t * simple_wlan_adapter;
lv_obj_t * simple_datalink_mode;
lv_obj_t * simple_preset;
lv_obj_t * simple_bandwidth;
lv_obj_t * simple_channel;
lv_obj_t * simple_txpower;

void simple_datalink_mode_loader_finished_cb(lv_event_t *e) {
    lv_obj_t *dropdown = lv_obj_get_child_by_type(simple_datalink_mode,0,&lv_dropdown_class);
    int current_option = lv_dropdown_get_selected(dropdown);
    if (current_option == 0) { // Manual
        lv_obj_remove_flag(simple_bandwidth,LV_OBJ_FLAG_HIDDEN);
    } else if (current_option == 1) { // Alink
        lv_obj_add_flag(simple_bandwidth,LV_OBJ_FLAG_HIDDEN);
    }
}

void simple_datalink_mode_cb(lv_event_t *e) {
    simple_datalink_mode_loader_finished_cb(e);
    lv_obj_t *dropdown = lv_obj_get_child_by_type(simple_datalink_mode,0,&lv_dropdown_class);
    int current_option = lv_dropdown_get_selected(dropdown);
    lv_obj_t * alink = lv_obj_get_child_by_type(air_adaptivelink,0,&lv_switch_class);
    lv_obj_set_state(alink, LV_STATE_CHECKED, current_option); // Dropdwon needs to match switch boolean
    lv_obj_send_event(alink,LV_EVENT_VALUE_CHANGED,NULL);
}

extern lv_obj_t * rec_fps;
void simple_video_mode_cb(lv_event_t *e) {
    char val[100] = "";
    char fps_str[10] = "";
    int element_count = 0;

    lv_obj_t *ta = lv_event_get_target(e);
    lv_dropdown_get_selected_str(ta, val, sizeof(val) - 1);

    // Parse the third element (expecting format like "4:3 1080p 90")
    char *token = strtok(val, " ");
    while (token != NULL && element_count < 3) {
        element_count++;
        if (element_count == 3) {
            strncpy(fps_str, token, sizeof(fps_str) - 1);
            fps_str[sizeof(fps_str) - 1] = '\0'; // Ensure null termination
            break;
        }
        token = strtok(NULL, " ");
    }

    if (element_count < 3) {
        // Handle error - string didn't have three elements
        return;
    }

    lv_obj_t *obj = lv_obj_get_child_by_type(rec_fps, 0, &lv_dropdown_class);
    int index = lv_dropdown_get_option_index(obj,fps_str);
    lv_dropdown_set_selected(obj, index);
    lv_obj_send_event(obj,LV_EVENT_VALUE_CHANGED,NULL);
}

void create_air_simple_menu(lv_obj_t * parent) {

    menu_page_data_t *menu_page_data = malloc(sizeof(menu_page_data_t) + sizeof(PageEntry) * ENTRIES);
    strcpy(menu_page_data->type, "air");
    strcpy(menu_page_data->page, "simple");
    menu_page_data->page_load_callback = generic_page_load_callback;
    menu_page_data->indev_group = lv_group_create();
    menu_page_data->entry_count = ENTRIES;
    lv_group_set_default(menu_page_data->indev_group);
    lv_obj_set_user_data(parent,menu_page_data);

    lv_obj_t * cont;
    lv_obj_t * section;

    section = lv_menu_section_create(parent);
    lv_obj_add_style(section, &style_openipc_section, 0);
    cont = lv_menu_cont_create(section);
    lv_obj_set_flex_flow(cont, LV_FLEX_FLOW_COLUMN);

    simple_video_mode = create_dropdown(cont,LV_SYMBOL_SETTINGS, "Video mode","","video_mode",menu_page_data,false);
    lv_obj_add_event_cb(lv_obj_get_child_by_type(simple_video_mode,0,&lv_dropdown_class), simple_video_mode_cb, LV_EVENT_VALUE_CHANGED,simple_video_mode);
    simple_wlan_adapter = create_dropdown(cont,LV_SYMBOL_SETTINGS, "WLAN Adapter","","wlan_adapter",menu_page_data,false);
    simple_datalink_mode = create_dropdown(cont,LV_SYMBOL_SETTINGS, "Datalink mode","","datalink_mode",menu_page_data,false);
    simple_preset = create_dropdown(cont,LV_SYMBOL_SETTINGS, "Preset","","preset",menu_page_data,false);
    lv_obj_add_event_cb(lv_obj_get_child_by_type(simple_datalink_mode,0,&lv_dropdown_class),simple_datalink_mode_cb,LV_EVENT_VALUE_CHANGED,simple_datalink_mode);
    lv_obj_add_event_cb(simple_datalink_mode,simple_datalink_mode_loader_finished_cb,MY_EVENT_1,NULL);
    lv_obj_remove_event_cb(lv_obj_get_child_by_type(simple_datalink_mode,0,&lv_dropdown_class),generic_dropdown_event_cb);

    simple_bandwidth = create_dropdown(cont,LV_SYMBOL_SETTINGS, "Banwdith","","bandwidth",menu_page_data,false);
    simple_channel  = create_dropdown(cont,LV_SYMBOL_SETTINGS, "Channel","","channel",menu_page_data,false);
    simple_txpower = create_dropdown(cont,LV_SYMBOL_SETTINGS, "TX Power","","txpower",menu_page_data,false);

    PageEntry entries[] = {
        { "Loading video_mode ...", simple_video_mode, reload_dropdown_value },
        { "Loading wlan_adapter ...", simple_wlan_adapter, reload_dropdown_value },
        { "Loading datalink_mode ...", simple_datalink_mode, reload_dropdown_value },
        { "Loading preset ...", simple_preset, reload_dropdown_value },
        { "Loading bandwidth ...", simple_bandwidth, reload_dropdown_value },
        { "Loading channel ...", simple_channel, reload_dropdown_value },
        { "Loading txpower ...", simple_txpower, reload_dropdown_value },
    };
    memcpy(menu_page_data->page_entries, entries, sizeof(entries));

    lv_group_set_default(default_group);
}