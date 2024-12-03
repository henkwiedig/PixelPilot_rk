#include <sys/reboot.h>
#include <linux/reboot.h> // For specific LINUX_REBOOT_CMD_* macros
#include <cstdlib> // for malloc and free

#include "menu_system.hpp"
#include "drm.h"
#include "spdlog.h"
#include "osd.h"

#define NK_IMPLEMENTATION
#define NK_GAMEPAD_IMPLEMENTATION
#define NK_CONSOLE_IMPLEMENTATION
#include "nuklear_settings.h"

extern bool menuActive;
extern struct osd_vars osd_vars;

void Menu::nk_cairo_render(cairo_t *cr, struct nk_context *ctx) const {
    const struct nk_command *cmd;

    // Iterate over all commands in the Nuklear command buffer
    nk_foreach(cmd, ctx) {
        switch (cmd->type) {
            case NK_COMMAND_NOP:
                break;
            case NK_COMMAND_LINE: {
                const struct nk_command_line *line = (const struct nk_command_line *)cmd;
                cairo_set_source_rgb(cr,
                                     line->color.r / 255.0,
                                     line->color.g / 255.0,
                                     line->color.b / 255.0);
                cairo_set_line_width(cr, line->line_thickness);
                cairo_move_to(cr, line->begin.x, line->begin.y);
                cairo_line_to(cr, line->end.x, line->end.y);
                cairo_stroke(cr);
                break;
            }
            case NK_COMMAND_RECT: {
                const struct nk_command_rect *rect = (const struct nk_command_rect *)cmd;
                cairo_set_source_rgb(cr,
                                     rect->color.r / 255.0,
                                     rect->color.g / 255.0,
                                     rect->color.b / 255.0);
                cairo_set_line_width(cr, rect->line_thickness);
                cairo_rectangle(cr, rect->x, rect->y, rect->w, rect->h);
                cairo_stroke(cr);
                break;
            }
            case NK_COMMAND_RECT_FILLED: {
                const struct nk_command_rect_filled *rect = (const struct nk_command_rect_filled *)cmd;
                cairo_set_source_rgb(cr,
                                     rect->color.r / 255.0,
                                     rect->color.g / 255.0,
                                     rect->color.b / 255.0);
                cairo_rectangle(cr, rect->x, rect->y, rect->w, rect->h);
                cairo_fill(cr);
                break;
            }
            case NK_COMMAND_TEXT: {
                const struct nk_command_text *text = (const struct nk_command_text *)cmd;
                cairo_select_font_face(cr, "sans-serif", CAIRO_FONT_SLANT_NORMAL, CAIRO_FONT_WEIGHT_NORMAL);
                cairo_set_font_size(cr, text->font->height);
                cairo_set_source_rgb(cr,
                                     text->foreground.r / 255.0,
                                     text->foreground.g / 255.0,
                                     text->foreground.b / 255.0);
                cairo_move_to(cr, text->x, text->y + text->font->height);
                cairo_show_text(cr, text->string);
                break;
            }
            case NK_COMMAND_CIRCLE_FILLED: {
                const struct nk_command_circle_filled *circle = (const struct nk_command_circle_filled *)cmd;
                cairo_set_source_rgb(cr,
                                     circle->color.r / 255.0,
                                     circle->color.g / 255.0,
                                     circle->color.b / 255.0);
                cairo_arc(cr,
                          circle->x + circle->w / 2.0,
                          circle->y + circle->h / 2.0,
                          circle->w / 2.0, 0, 2 * M_PI);
                cairo_fill(cr);
                break;
            }
            case NK_COMMAND_CIRCLE: {
                const struct nk_command_circle *circle = (const struct nk_command_circle *)cmd;
                cairo_set_source_rgb(cr,
                                     circle->color.r / 255.0,
                                     circle->color.g / 255.0,
                                     circle->color.b / 255.0);
                cairo_set_line_width(cr, circle->line_thickness);
                cairo_arc(cr,
                          circle->x + circle->w / 2.0,
                          circle->y + circle->h / 2.0,
                          circle->w / 2.0, 0, 2 * M_PI);
                cairo_stroke(cr);
                break;
            }
            case NK_COMMAND_IMAGE: {
                const struct nk_command_image *image = (const struct nk_command_image *)cmd;
                // Assuming Cairo supports loading the image into a cairo_surface_t
                cairo_surface_t *img = (cairo_surface_t *)image->img.handle.ptr;
                cairo_set_source_surface(cr, img, image->x, image->y);
                cairo_rectangle(cr, image->x, image->y, image->w, image->h);
                cairo_fill(cr);
                break;
            }
            case NK_COMMAND_SCISSOR:
            {
                const struct nk_command_scissor *s = (const struct nk_command_scissor *)cmd;
                cairo_reset_clip(cr);
                if (s->x >= 0) {
                    cairo_rectangle(cr, s->x - 1, s->y - 1, s->w + 2, s->h + 2);
                    cairo_clip(cr);
                }
                break;
            }
            case NK_COMMAND_TRIANGLE:
            {
                const struct nk_command_triangle *t = (const struct nk_command_triangle *)cmd;
                cairo_set_line_width(cr, t->line_thickness);
                cairo_move_to(cr, t->a.x, t->a.y);
                cairo_line_to(cr, t->b.x, t->b.y);
                cairo_line_to(cr, t->c.x, t->c.y);
                cairo_close_path(cr);
                cairo_stroke(cr);
                break;
            }
            case NK_COMMAND_TRIANGLE_FILLED:
            {
                const struct nk_command_triangle_filled *t = (const struct nk_command_triangle_filled *)cmd;
                cairo_move_to(cr, t->a.x, t->a.y);
                cairo_line_to(cr, t->b.x, t->b.y);
                cairo_line_to(cr, t->c.x, t->c.y);
                cairo_close_path(cr);
                cairo_fill(cr);
                break;
            }
            default:
                SPDLOG_INFO("Unsupportet Nuklear command {}",cmd->type);
                break;
        }
    }
}

void reboot_clicked(struct nk_console* button, void* user_data) {
    // Sync filesystem to ensure all changes are written
    sync();

    // Trigger a system reboot
    if (reboot(LINUX_REBOOT_CMD_RESTART) != 0) {
        perror("Reboot failed");
    }    
 
}

void exit_clicked(struct nk_console* button, void* user_data) {
    menuActive = false;
    osd_vars.refresh_frequency_ms = 1000;
}

void Menu::wlan_channel_changed(struct nk_console* button, void* user_data) {
    Menu* menu_instance = static_cast<Menu*>(user_data);

    spdlog::info("Changeing wlan channel to chan {} freq {}", 
        menu_instance->wfbChannels[menu_instance->current_channel].channel,
        menu_instance->wfbChannels[menu_instance->current_channel].frequency);

    menu_instance->read_and_process_nics("/etc/default/wifibroadcast", menu_instance->wfbChannels[menu_instance->current_channel].channel);
    menu_instance->update_config("/etc/wifibroadcast.cfg", menu_instance->wfbChannels[menu_instance->current_channel].channel);        
 
 }

char* Menu::concatChannels(const std::vector<WLANChannel>& wfbChannels) {
    std::ostringstream oss;

    for (const auto& wlanChannel : wfbChannels) {
        oss << wlanChannel.channel
            << " (" << wlanChannel.frequency << " MHz);";
    }

    // Convert the string stream to a std::string
    std::string result = oss.str();

    // Remove the last semicolon if the result is not empty
    if (!result.empty() && result.back() == ';') {
        result.pop_back();
    }

    // Allocate memory for the C-string and copy the content
    char* cStr = new char[result.size() + 1];
    std::strcpy(cStr, result.c_str());

    return cStr; // Caller must free this memory with `delete[]`
}


void Menu::initMenu() {

    // Set up the console within the Nuklear context
    console = nk_console_init(&ctx);

    // nk_console_combobox(console, "Resolution", "1920x1080@60;1280x720@50;1600x1200@60", ';', &i)
    //     ->tooltip = "Select display resolution";

    nk_console* wlan_channel_options = nk_console_combobox(console, "Channel",  concatChannels(wfbChannels), ';', &current_channel);
    wlan_channel_options->tooltip = "Select wfb channel";
    nk_console_add_event_handler(wlan_channel_options, NK_CONSOLE_EVENT_CHANGED, &wlan_channel_changed,this,NULL);

    // setup theme dark
    struct nk_color table[NK_COLOR_COUNT];
    table[NK_COLOR_TEXT] = nk_rgba(210, 210, 210, 255);
    table[NK_COLOR_WINDOW] = nk_rgba(57, 67, 71, 215);
    table[NK_COLOR_HEADER] = nk_rgba(51, 51, 56, 220);
    table[NK_COLOR_BORDER] = nk_rgba(46, 46, 46, 255);
    table[NK_COLOR_BUTTON] = nk_rgba(48, 83, 111, 255);
    table[NK_COLOR_BUTTON_HOVER] = nk_rgba(58, 93, 121, 255);
    table[NK_COLOR_BUTTON_ACTIVE] = nk_rgba(63, 98, 126, 255);
    table[NK_COLOR_TOGGLE] = nk_rgba(50, 58, 61, 255);
    table[NK_COLOR_TOGGLE_HOVER] = nk_rgba(45, 53, 56, 255);
    table[NK_COLOR_TOGGLE_CURSOR] = nk_rgba(48, 83, 111, 255);
    table[NK_COLOR_SELECT] = nk_rgba(57, 67, 61, 255);
    table[NK_COLOR_SELECT_ACTIVE] = nk_rgba(48, 83, 111, 255);
    table[NK_COLOR_SLIDER] = nk_rgba(50, 58, 61, 255);
    table[NK_COLOR_SLIDER_CURSOR] = nk_rgba(48, 83, 111, 245);
    table[NK_COLOR_SLIDER_CURSOR_HOVER] = nk_rgba(53, 88, 116, 255);
    table[NK_COLOR_SLIDER_CURSOR_ACTIVE] = nk_rgba(58, 93, 121, 255);
    table[NK_COLOR_PROPERTY] = nk_rgba(50, 58, 61, 255);
    table[NK_COLOR_EDIT] = nk_rgba(50, 58, 61, 225);
    table[NK_COLOR_EDIT_CURSOR] = nk_rgba(210, 210, 210, 255);
    table[NK_COLOR_COMBO] = nk_rgba(50, 58, 61, 255);
    table[NK_COLOR_CHART] = nk_rgba(50, 58, 61, 255);
    table[NK_COLOR_CHART_COLOR] = nk_rgba(48, 83, 111, 255);
    table[NK_COLOR_CHART_COLOR_HIGHLIGHT] = nk_rgba(255, 0, 0, 255);
    table[NK_COLOR_SCROLLBAR] = nk_rgba(50, 58, 61, 255);
    table[NK_COLOR_SCROLLBAR_CURSOR] = nk_rgba(48, 83, 111, 255);
    table[NK_COLOR_SCROLLBAR_CURSOR_HOVER] = nk_rgba(53, 88, 116, 255);
    table[NK_COLOR_SCROLLBAR_CURSOR_ACTIVE] = nk_rgba(58, 93, 121, 255);
    table[NK_COLOR_TAB_HEADER] = nk_rgba(48, 83, 111, 255);
    table[NK_COLOR_KNOB] = table[NK_COLOR_SLIDER];
    table[NK_COLOR_KNOB_CURSOR] = table[NK_COLOR_SLIDER_CURSOR];
    table[NK_COLOR_KNOB_CURSOR_HOVER] = table[NK_COLOR_SLIDER_CURSOR_HOVER];
    table[NK_COLOR_KNOB_CURSOR_ACTIVE] = table[NK_COLOR_SLIDER_CURSOR_ACTIVE];
    nk_style_from_table(&ctx, table);    

    nk_console_button_onclick(console, "Reboot VRX", &reboot_clicked);
    nk_console_button_onclick(console, "Exit Menu", &exit_clicked);

}

void Menu::drawMenu(struct modeset_buf *buf) {

	cairo_t* cr;
	cairo_surface_t *surface;
	surface = cairo_image_surface_create_for_data(buf->map, CAIRO_FORMAT_ARGB32, buf->width, buf->height, buf->stride);
	cr = cairo_create (surface);

	// https://www.cairographics.org/FAQ/#clear_a_surface
	cairo_save(cr);
	cairo_set_operator(cr, CAIRO_OPERATOR_CLEAR);
	cairo_paint(cr);
	cairo_restore(cr);
    cairo_set_source_rgba(cr, 0, 0, 0, 0); // Transparent black (alpha = 0)
    cairo_paint(cr);

    nk_console_render_window(console, "VRX Settings", nk_rect(200, 200, 800, 600), NK_WINDOW_TITLE);

    // Render Nuklear commands using Cairo
    nk_cairo_render(cr, &ctx);

    // Cleanup
    nk_clear(&ctx);
    cairo_destroy(cr);
    cairo_surface_destroy(surface);
}

void Menu::handleInput(char input) {
    nk_input_begin(&ctx);  // Start input processing
    switch (input) {
        case 'e':
            nk_input_key(&ctx, NK_KEY_ENTER, true);
            break;
        case 'w': 
            nk_input_key(&ctx, NK_KEY_UP, true);
            break;  
        case 's': 
            nk_input_key(&ctx, NK_KEY_DOWN, true);
            break;  
        case 'a': 
            nk_input_key(&ctx, NK_KEY_LEFT, true);
            break;  
        case 'd': 
            nk_input_key(&ctx, NK_KEY_RIGHT, true);
            break;
        case '\0': 
            break;  
        default:
            SPDLOG_WARN("Unhandled key: {}", std::string(1, input));  // Log unknown input
            break;
    }

    nk_input_end(&ctx);  // End input processing
}

void Menu::releaseKeys(void) {
    nk_input_begin(&ctx);  // Start input processing
    nk_input_key(&ctx, NK_KEY_ENTER, false);
    nk_input_key(&ctx, NK_KEY_UP, false);
    nk_input_key(&ctx, NK_KEY_DOWN, false);
    nk_input_key(&ctx, NK_KEY_LEFT, false);
    nk_input_key(&ctx, NK_KEY_RIGHT, false);
    nk_input_end(&ctx);  // End input processing
}



void Menu::execute_command(const char *command) {
    if (system(command) != 0) {
        fprintf(stderr, "Error executing command: %s\n", command);
    }
}

void Menu::update_config(const char *config_path, int channel) {
    FILE *file = fopen(config_path, "r");
    if (!file) {
        fprintf(stderr, "Error opening config file: %s\n", config_path);
        return;
    }

    char temp_path[] = "/tmp/wifibroadcast.cfgXXXXXX";
    int temp_fd = mkstemp(temp_path);
    if (temp_fd == -1) {
        fprintf(stderr, "Error creating temporary file\n");
        fclose(file);
        return;
    }

    FILE *temp_file = fdopen(temp_fd, "w");
    if (!temp_file) {
        fprintf(stderr, "Error opening temporary file for writing\n");
        fclose(file);
        return;
    }

    char line[MAX_LINE_LENGTH];
    int updated = 0;

    while (fgets(line, sizeof(line), file)) {
        if (strstr(line, "wifi_channel =") != NULL) {
            fprintf(temp_file, "wifi_channel = %d\n", channel);
            updated = 1;
        } else {
            fputs(line, temp_file);
        }
    }

    if (!updated) {
        fprintf(temp_file, "wifi_channel = %d\n", channel);
    }

    fclose(file);
    fclose(temp_file);

    if (rename(temp_path, config_path) != 0) {
        fprintf(stderr, "Error updating config file: %s\n", config_path);
        remove(temp_path);
    }
}

void Menu::process_interfaces(const char *interfaces, int channel) {
    char *nics = strdup(interfaces);
    if (!nics) {
        fprintf(stderr, "Memory allocation error\n");
        return;
    }

    char *nic = strtok(nics, " ");
    while (nic) {
        char command[256];
        snprintf(command, sizeof(command), "iw %s set channel %d", nic, channel);
        execute_command(command);
        nic = strtok(NULL, " ");
    }

    free(nics);
}

void Menu::read_and_process_nics(const char *default_path, int channel) {
    FILE *file = fopen(default_path, "r");
    if (!file) {
        fprintf(stderr, "Error opening file: %s\n", default_path);
        return;
    }

    char line[MAX_LINE_LENGTH];
    while (fgets(line, sizeof(line), file)) {
        if (strstr(line, "WFB_NICS=") == line) {
            char *start = strchr(line, '"');
            char *end = strrchr(line, '"');
            if (start && end && end > start) {
                *end = '\0';
                process_interfaces(start + 1, channel);
            }
        }
    }

    fclose(file);
}

int Menu::read_wifi_channel(const char *config_path) {
    FILE *file = fopen(config_path, "r");
    if (!file) {
        perror("Error opening config file");
        return -1; // Return -1 on error
    }

    char line[MAX_LINE_LENGTH];
    int wifi_channel = -1;

    while (fgets(line, sizeof(line), file)) {
        if (strstr(line, "wifi_channel =") != NULL) {
            // Extract the channel value
            char *equals_sign = strchr(line, '=');
            if (equals_sign) {
                wifi_channel = atoi(equals_sign + 1); // Convert to integer
                break; // Exit the loop once the channel is found
            }
        }
    }

    fclose(file);

    if (wifi_channel == -1) {
        fprintf(stderr, "wifi_channel not found in config file\n");
    }

    return wifi_channel;
}
