#pragma once
#include <cairo.h>
#include <cstddef>
#include <vector>
#include <memory>
#include <string>
#include "spdlog.h"
#include "yaml-cpp/yaml.h"
#include "nuklear_settings.h"
#include "MenuConfig.hpp"

class Menu {
public:
    Menu(){
        SPDLOG_INFO("Menu opened ...");
        YAML::Node config = YAML::LoadFile("menu.yml");
        spdlog::debug("Loaded YAML file");
        gpioConfig = config["gpio"].as<GPIOConfig>();
        wfbChannels = config["menu"]["wfb_channels"].as<std::vector<WLANChannel>>();

        default_font.userdata.ptr = NULL; // No additional font data
        default_font.height = 23.0f; // Font height
        default_font.width = font_width_calculator; // Text width calculation function
        nk_init(&ctx, &allocator, &default_font);
    };
    ~Menu() {
        nk_console_free(console);
        nk_free(&ctx);
    }
    void drawMenu(struct modeset_buf *buf);
    void handleInput(char input);
    void initMenu();
    void releaseKeys(void);
    GPIOConfig gpioConfig;
private:

    // Wrapper functions for the custom allocator
    static void* custom_alloc(nk_handle, void*, nk_size size) {
        return malloc(size);    
    }

    static void custom_free(nk_handle, void* ptr) {
        free(ptr);
    }

    // Dummy font width calculation function (replace with your logic if necessary)
    static float font_width_calculator(nk_handle handle, float height, const char *text, int len) {
        (void)handle; // Handle is unused here  
        return len * height * 0.5f; // Example width calculation
    }

    struct nk_allocator allocator = {
        .alloc = custom_alloc,
        .free = custom_free
    };
    // Create a font
    struct nk_user_font default_font;

    void nk_cairo_render(cairo_t *cr, struct nk_context *ctx) const;
    struct nk_context ctx;
    nk_console* console;

    // init gui state
    std::vector<WLANChannel> wfbChannels;
   
    enum {EASY, HARD};
    int op = EASY;
    float value = 0.6f;
    int i =  1;
    int j =  7;
    int wlan_channel = 161;
    const int textedit_buffer_size = 256;
    char textedit_buffer[256] = "123456ABFCD";
    nk_bool radio_option = nk_false;
};
