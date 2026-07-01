#pragma once

#include "lvgl/lvgl.h"
#include "ui.h"

// Build the OSD editor into the given menu page. Mirrors the gs_scripts /
// gs_actions pattern: sets up menu_page_data and renders a dynamic view.
void create_gs_osd_menu(lv_obj_t *parent);

// page_load_callback: re-read the OSD config and rebuild the list each time the
// page is opened.
void gs_osd_page_refresh(lv_obj_t *page);
