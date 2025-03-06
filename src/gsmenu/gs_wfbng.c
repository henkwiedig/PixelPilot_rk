#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "gs_wfbng.h"
#include "air_wfbng.h"
#include "lvgl/lvgl.h"
#include "helper.h"
#include "executor.h"
#include "styles.h"

lv_obj_t * gs_channel;
lv_obj_t * bandwidth;
lv_obj_t * adaptivelink;


void gs_wfbng_page_load_callback(lv_obj_t * page)
{
    reload_dropdown_value(page,gs_channel);
    reload_dropdown_value(page,bandwidth);
    reload_switch_value(page,adaptivelink);
}

void create_gs_wfbng_menu(lv_obj_t * parent) {

    menu_page_data_t* menu_page_data = malloc(sizeof(menu_page_data_t));
    strcpy(menu_page_data->type, "gs");
    strcpy(menu_page_data->page, "wfbng");
    menu_page_data->page_load_callback = gs_wfbng_page_load_callback;
    lv_obj_set_user_data(parent,menu_page_data);


    lv_obj_t * cont;
    lv_obj_t * section;

    create_text(parent, NULL, "WFB-NG", LV_MENU_ITEM_BUILDER_VARIANT_1);
    section = lv_menu_section_create(parent);
    lv_obj_add_style(section, &style_openipc_section, 0);
    cont = lv_menu_cont_create(section);
    lv_obj_set_flex_flow(cont, LV_FLEX_FLOW_COLUMN);    

    gs_channel = create_dropdown(cont,LV_SYMBOL_SETTINGS, "Channel", "","gs_channel",menu_page_data,false);
    bandwidth = create_dropdown(cont,LV_SYMBOL_SETTINGS, "bandwidth", "","bandwidth",menu_page_data,false);

    create_text(parent, NULL, "Adaptive Link", LV_MENU_ITEM_BUILDER_VARIANT_1);
    section = lv_menu_section_create(parent);
    lv_obj_add_style(section, &style_openipc_section, 0);
    cont = lv_menu_cont_create(section);
    lv_obj_set_flex_flow(cont, LV_FLEX_FLOW_COLUMN);    

    adaptivelink = create_switch(cont,LV_SYMBOL_SETTINGS,"Enabled","adaptivelink", menu_page_data,false);

}