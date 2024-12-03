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

#define MAX_LINE_LENGTH 1024

struct DRMMode {
    uint width;
    uint height;
    uint refresh_rate;
    uint16_t flags : 2; // Only 2 bits required: 1 for is_preferred, 1 for is_current

    // Helper functions for clarity
    bool is_preferred() const { return flags & 0x1; }
    bool is_current() const { return flags & 0x2; }    
};

extern int drm_fd;

class Menu {
public:
    Menu(){
        SPDLOG_INFO("Menu opened ...");
        YAML::Node config = YAML::LoadFile("menu.yml");
        spdlog::debug("Loaded YAML file");
        gpioConfig = config["gpio"].as<GPIOConfig>();

        //wfb-ng channels
        wfbChannels = config["menu"]["wfb_channels"].as<std::vector<WLANChannel>>();
        int current_channel_value = read_wifi_channel("/etc/wifibroadcast.cfg");
        if (current_channel_value == -1) {
            spdlog::error("Failed to read the current WiFi channel");
        }
        // Find the index of the current channel in wfbChannels
        current_channel = -1;
        for (size_t i = 0; i < wfbChannels.size(); ++i) {
            if (wfbChannels[i].channel == current_channel_value) {
                current_channel = static_cast<int>(i); // Found the matching index
                break;
            }
        }
        if (current_channel == -1) {
            spdlog::error("Current channel {} not found in wfbChannels");
        } else {
            spdlog::debug("Current channel index: {}", current_channel);
        }
        

        // screen-mode
        drmModes = get_supported_modes(drm_fd);


        // nuklear setup
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

    // wfb_ng channel
    char* concatChannels(const std::vector<WLANChannel>& wfbChannels);
    static void wlan_channel_changed(struct nk_console* button, void* user_data);
    void execute_command(const char *command);
    void update_config(const char *config_path, int channel);
    void read_and_process_nics(const char *default_path, int channel);
    void process_interfaces(const char *interfaces, int channel);
    int read_wifi_channel(const char *config_path);
    std::vector<WLANChannel> wfbChannels;
    int current_channel = 1;

    //screen mode
    std::vector<DRMMode> get_supported_modes(int fd);
    char* concatModes(const std::vector<DRMMode>& drmModes);
    static void drmmode_changed(struct nk_console* button, void* user_data);
    std::vector<DRMMode> drmModes;
    int current_drmmode = 1;
   
    // const int textedit_buffer_size = 256;
    // char textedit_buffer[256] = "123456ABFCD";
    // nk_bool radio_option = nk_false;
    // char file_path_buffer[1024] = {0};
    // int file_path_buffer_size = 1024;    
};
