#include <sys/reboot.h>
#include <linux/reboot.h> // For specific LINUX_REBOOT_CMD_* macros
#include <cstdlib> // for malloc and free
#include <fstream>
#include <regex>


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
extern void sig_handler(int signum);
const int textedit_buffer_size = 256;

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

void Menu::drmmode_changed(struct nk_console* button, void* user_data) {
    Menu* menu_instance = static_cast<Menu*>(user_data);

    const DRMMode& current_mode = menu_instance->drmModes[menu_instance->current_drmmode];

    spdlog::info("Changing screen mode to {}x{}@{}", 
        current_mode.width,
        current_mode.height,
        current_mode.refresh_rate);

    // Write the new screen mode to /config/scripts/screen-mode
    std::ofstream mode_file("/config/scripts/screen-mode");
    if (mode_file.is_open()) {
        mode_file << current_mode.width << "x" << current_mode.height 
                  << "@" << current_mode.refresh_rate << "\n";
        mode_file.close();
        spdlog::info("Updated /config/scripts/screen-mode successfully.");
    } else {
        spdlog::error("Failed to open /config/scripts/screen-mode for writing.");
    }
}

char* Menu::concatChannels(const std::vector<WFBNGChannel>& wfbChannels) {
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

char* Menu::concatModes(const std::vector<DRMMode>& drmModes) {
        std::ostringstream oss;

        for (const auto& mode : drmModes) {
            oss << mode.width << "x" 
                << mode.height << "@" 
                << mode.refresh_rate
                << (mode.is_preferred() ? " (Preferred)" : "")
                << (mode.is_current() ? " (Current)" : "")
                << ";";
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

char* concatWLANs(const std::vector<WLAN>& wlans) {
    std::ostringstream oss;

    for (const auto& wlan : wlans) {
        oss << wlan.ssid
            << " (" << wlan.security << ");";
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


// ToDO: Hot reset screenmode
void apply_clicked(struct nk_console* button, void* user_data) {
    spdlog::info("Apply screen-mdode. Restart PixelPilot_rk");
    sig_handler(1);
}

void Menu::wlan_apply_clicked(struct nk_console* button, void* user_data) {
    spdlog::info("wlan_apply_clicked");
    Menu* menu_instance = static_cast<Menu*>(user_data);
    spdlog::debug("New WLAN Settings WLan SSID: {} Password: {} Enabled:{}", menu_instance->availableWlans[menu_instance->current_wlan_index].ssid,menu_instance->password,menu_instance->wlan_enabled);
    spdlog::debug("New Ad-Hoc Settings SSID: {} Password: {} Enabled: {}", menu_instance->ad_hoc_ssid,menu_instance->password,menu_instance->ad_hoc_enabled);


    if (menu_instance->wlan_enabled) {

        std::ofstream configFile("/config/config.txt");
        if (!configFile.is_open()) {
            spdlog::error("Failed to open the file for writing: /config/config.txt");
            return;
        }
        // Write the Wi-Fi connection line
        configFile << "# rsetup config file\n";
        configFile << "# Configurations here will be applied on every boot.\n";
        configFile << "#\n";
        configFile << "connect_wi-fi " << menu_instance->availableWlans[menu_instance->current_wlan_index].ssid << " " << menu_instance->password << "\n";
        configFile.close();
    }


    // Construct the nmcli command
    std::string command = "nmcli dev wifi connect \"" + 
        menu_instance->availableWlans[menu_instance->current_wlan_index].ssid + 
        "\" password \"" + menu_instance->password + "\"";

    spdlog::debug("nmcli command: {}", command);

    // Execute the nmcli command
    FILE *fp = popen(command.c_str(), "r");
    if (!fp) {
        spdlog::error("Failed to execute nmcli command.");
        return;
    }

    // Read the output from the command if needed
    char buffer[128];
    while (fgets(buffer, sizeof(buffer), fp) != nullptr) {
        spdlog::info("nmcli output: {}", buffer);
    }

    // Close the file pointer
    pclose(fp);

}

void wlan_toggled(struct nk_console* button, void* user_data) {
    spdlog::info("wlan_toggled triggered");
    Menu* menu_instance = static_cast<Menu*>(user_data);
    menu_instance->ad_hoc_enabled = nk_false;
    nk_console* wlan_settings= static_cast<nk_console*>(menu_instance->console->children[3]);
    wlan_settings->children[2]->visible = nk_true;
    wlan_settings->children[3]->visible = nk_true;
    wlan_settings->children[4]->visible = nk_true;
    wlan_settings->children[5]->visible = nk_false;
    wlan_settings->children[6]->visible = nk_false;
}

void hd_hoc_toggled(struct nk_console* button, void* user_data) {
    spdlog::info("ah_hoc_toggled triggered");
    Menu* menu_instance = static_cast<Menu*>(user_data);
    menu_instance->wlan_enabled = nk_false;
    nk_console* wlan_settings= static_cast<nk_console*>(menu_instance->console->children[3]);
    wlan_settings->children[2]->visible = nk_false;
    wlan_settings->children[3]->visible = nk_false;
    wlan_settings->children[4]->visible = nk_false;
    wlan_settings->children[5]->visible = nk_true;
    wlan_settings->children[6]->visible = nk_true;
}

// Function to update the combobox with a new string
void nk_console_combobox_update(nk_console* combobox, const char* new_items_separated_by_separator) {
    if (combobox && combobox->data) {
        nk_console_combobox_data* data = (nk_console_combobox_data*)combobox->data;
        
        // Update the string that holds the items
        data->items_separated_by_separator = new_items_separated_by_separator;
        
        // Rebuild the combobox buttons based on the new list of items
        int separator = data->separator;
        const char* button_text_start = new_items_separated_by_separator;
        int text_length = 0;
        
        // Manually clear the combobox's children (old buttons)
        if (combobox->children != NULL) {
            // Free each old child (button) widget
            for (int i = 0; i < (int)cvector_size(combobox->children); i++) {
                nk_console* old_button = combobox->children[i];
                // Assuming nk_console_free exists or is implemented to free the widget
                nk_console_free(old_button);  // Free memory allocated for the old button
            }
            // Reset the children vector to an empty state
            combobox->children = NULL;
        }

        // Re-Add Back button
        nk_console* backbutton = nk_console_button_onclick(combobox, "SSID", &nk_console_combobox_button_click);
        nk_console_button_set_symbol(backbutton, NK_SYMBOL_TRIANGLE_UP);
        
        // Re-add the new buttons based on the updated string
        for (int i = 0; new_items_separated_by_separator[i] != 0; i++) {
            text_length++;
            if (new_items_separated_by_separator[i] == (char)separator) {
                nk_console_button_onclick(combobox, button_text_start, &nk_console_combobox_button_click)
                    ->label_length = text_length - 1;
                text_length = 0;
                button_text_start = new_items_separated_by_separator + i + 1;
            }
        }

        // Add the last item
        nk_console_button_onclick(combobox, button_text_start, &nk_console_combobox_button_click)
            ->label_length = text_length;
        
        // Optionally update selected index
        if (data->selected && *data->selected >= 0) {
            combobox->label = combobox->children[*data->selected + 1]->label;
            combobox->label_length = combobox->children[*data->selected + 1]->label_length;
        }
    }
}

#define MAX_LINE_LEN 1024

void rescan_clicked(struct nk_console* button, void* user_data) {
    Menu* menu_instance = static_cast<Menu*>(user_data);
    std::vector<WLAN> availableWlans;
    int i = 0;

    spdlog::info("rescan triggered");

    // Execute the nmcli command
    FILE *fp = popen("nmcli device wifi list --rescan auto", "r");
    if (!fp) {
        spdlog::error("Failed to execute nmcli command.");
        return;
    }

    char buffer[MAX_LINE_LEN];
    bool isHeaderSkipped = false;

    // Read the output line by line
    while (fgets(buffer, sizeof(buffer), fp)) {
        std::string line = buffer;
        spdlog::debug("line: {}", line);

        // Skip the header line
        if (!isHeaderSkipped) {
            isHeaderSkipped = true;
            spdlog::debug("Header skipped");
            continue;
        }

        // Skip empty lines
        if (line.empty() || line.find_first_not_of(" \t\n") == std::string::npos) {
            continue;
        }

        // Parse the line into WLAN structure
        std::istringstream iss(line);
        WLAN wlan;

        std::string inUse;
        iss >> inUse;  // Skip "IN-USE" column, we don't need it
        
        // Handle cases where there's a '*' in front of BSSID
        if (inUse == "*") {
            iss >> wlan.bssid;
            menu_instance->current_wlan_index = i;
        } else {
            wlan.bssid = inUse;  // If no *, treat first value as BSSID
            i++;
        }

        // Extract the SSID: Take everything until we hit the mode (Infra/Ad-Hoc)
        std::string ssidPart;
        while (iss >> ssidPart) {
            // Stop when the mode column is reached (typically "Infra" or "Ad-Hoc")
            if (ssidPart == "Infra" || ssidPart == "Ad-Hoc") {
                break;
            }
            wlan.ssid += ssidPart + " ";  // Add SSID part
        }
        wlan.ssid = wlan.ssid.substr(0, wlan.ssid.size() - 1);  // Remove the trailing space

        // Skip unnecessary columns
        std::string mode, channel, rate, signalStrength, bars, security;
        iss >> mode >> channel >> rate >> signalStrength >> bars;

        // Handle signal strength safely
        try {
            wlan.signal_strength = std::stoi(signalStrength); // Parse signal percentage
        } catch (const std::invalid_argument& e) {
            spdlog::warn("Invalid signal strength value: {}. Skipping entry.", signalStrength);
            continue;
        }

        // Extract the security types
        std::getline(iss, security);  // Security column (rest of the line after the bars)

        // Trim whitespace from security type
        security.erase(0, security.find_first_not_of(" \t"));
        security.erase(security.find_last_not_of(" \t") + 1);
        wlan.security = security;

        // Add WLAN to the list if valid
        if (!wlan.bssid.empty() && !wlan.ssid.empty()) {
            availableWlans.push_back(wlan);
        } else {
            spdlog::warn("Skipping invalid WLAN entry: BSSID or SSID is empty.");
        }
    }

    // Update the available WLANs in the menu instance
    menu_instance->availableWlans = availableWlans;

    spdlog::debug("Available WLANs:");
    for (const auto& wlan : availableWlans) {
        spdlog::debug("BSSID: {}, SSID: {}, Signal: {}%, Channel: {}, Security: {}", wlan.bssid, wlan.ssid, wlan.signal_strength, wlan.channel, wlan.security);
    }

    pclose(fp);

    // update the nuklear with found wlans
    nk_console* wlan_settings= static_cast<nk_console*>(menu_instance->console->children[3]);
    nk_console* nk_current_wlan_index = wlan_settings->children[2];

    const char* updated_items = concatWLANs(availableWlans);
    // Assuming combobox is already created with nk_console_combobox earlier
    nk_console_combobox_update(nk_current_wlan_index, updated_items);

}



void Menu::initMenu() {

    // Set up the console within the Nuklear context
    console = nk_console_init(&ctx);

    // row spacewing for screen mode
    nk_console* drmmode_options = nk_console_combobox(console, "Resolution", concatModes(drmModes), ';', &current_drmmode);
    drmmode_options->tooltip = "Select display resolution";
    nk_console_add_event_handler(drmmode_options, NK_CONSOLE_EVENT_CHANGED, &drmmode_changed,this,NULL);
   
    nk_console_button_onclick(console, "Apply", &apply_clicked);

    // wfb-ng channel
    nk_console* wlan_channel_options = nk_console_combobox(console, "Channel",  concatChannels(wfbChannels), ';', &current_channel);
    wlan_channel_options->tooltip = "Select wfb channel";
    nk_console_add_event_handler(wlan_channel_options, NK_CONSOLE_EVENT_CHANGED, &wlan_channel_changed,this,NULL);


    // WLAN Settings
    nk_console* wlan_settings = nk_console_button(console, "WLAN Settings");
    {

        nk_console* nk_wlan_enabled = nk_console_checkbox(wlan_settings, "WLAN Enabled", &wlan_enabled);
        nk_console_add_event_handler(nk_wlan_enabled, NK_CONSOLE_EVENT_CHANGED, &wlan_toggled, this, NULL);
        nk_console* nk_ad_hoc_enabled = nk_console_checkbox(wlan_settings, "Ad-Hoc Enabled", &ad_hoc_enabled);
        nk_console_add_event_handler(nk_ad_hoc_enabled, NK_CONSOLE_EVENT_CHANGED, &hd_hoc_toggled, this, NULL);


        nk_console* nk_current_wlan_index = nk_console_combobox(wlan_settings, "SSID", concatWLANs(availableWlans), ';', &current_wlan_index);
        nk_console* nk_password = nk_console_textedit(wlan_settings, "Password", password, textedit_buffer_size);
        nk_console* rescan_button = nk_console_button(wlan_settings, "Rescan WLANs");
        nk_console_add_event_handler(rescan_button, NK_CONSOLE_EVENT_CLICKED, &rescan_clicked,this,NULL);
        nk_current_wlan_index->visible = wlan_enabled;
        nk_password->visible = wlan_enabled;
        rescan_button->visible = wlan_enabled;

        nk_console* nk_ad_hoc_ssid = nk_console_textedit(wlan_settings, "Ad-Hoc SSID", ad_hoc_ssid, textedit_buffer_size);
        nk_console* nk_ad_hoc_password = nk_console_textedit(wlan_settings, "Ad-Hoc Password", ad_hoc_password, textedit_buffer_size);
        nk_ad_hoc_ssid->visible = ad_hoc_enabled;
        nk_ad_hoc_password->visible = ad_hoc_enabled;

        nk_console* wlan_apply_button = nk_console_button(wlan_settings, "Apply");
        nk_console_add_event_handler(wlan_apply_button, NK_CONSOLE_EVENT_CLICKED, &wlan_apply_clicked,this,NULL);

        nk_console_button_onclick(wlan_settings, "Back", &nk_console_button_back);
    }    

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


std::vector<DRMMode> Menu::get_supported_modes(int fd) {
    std::vector<DRMMode> modes;
	uint prev_h, prev_v, prev_refresh = 0;

    drmModeRes* res = drmModeGetResources(fd);

    if (!res) {
        perror("Failed to get DRM resources");
        return modes; // Return empty vector on error
    }

    for (int i = 0; i < res->count_connectors; ++i) {
        drmModeConnector* conn = drmModeGetConnector(fd, res->connectors[i]);
        if (!conn) continue;

        drmModeCrtc* crtc = nullptr;
        if (conn->encoder_id) {
            drmModeEncoder* encoder = drmModeGetEncoder(fd, conn->encoder_id);
            if (encoder) {
                crtc = drmModeGetCrtc(fd, encoder->crtc_id);
                drmModeFreeEncoder(encoder);
            }
        }

        for (int j = 0; j < conn->count_modes; ++j) {
            drmModeModeInfo mode_info = conn->modes[j];
            DRMMode mode = {
                mode_info.hdisplay,
                mode_info.vdisplay,
                mode_info.vrefresh,
                0 // Initialize flags to 0
            };

            // Set the preferred flag
            if (mode_info.type & DRM_MODE_TYPE_PREFERRED) {
                mode.flags |= 0x1; // Set bit 0
            }

            // Check if this mode is the current mode (if CRTC is active)
            if (crtc && crtc->mode_valid &&
                crtc->mode.hdisplay == mode_info.hdisplay &&
                crtc->mode.vdisplay == mode_info.vdisplay &&
                crtc->mode.vrefresh == mode_info.vrefresh) {
                mode.flags |= 0x2; // Set bit 1
            }

            if (mode_info.hdisplay == prev_h && mode_info.vdisplay == prev_v && mode_info.vrefresh == prev_refresh)
				continue;
            prev_h = mode_info.hdisplay;
			prev_v = mode_info.vdisplay;
			prev_refresh = mode_info.vrefresh;

            modes.push_back(mode);
        }

        if (crtc) {
            drmModeFreeCrtc(crtc);
        }
        drmModeFreeConnector(conn);
    }

    drmModeFreeResources(res);
    return modes;
}

void Menu::parseWLANConfigFile() {
    spdlog::debug("Parseing /config/config.txt");
    std::ifstream configFile("/config/config.txt");
    if (!configFile.is_open()) {
        spdlog::error("Failed to open the file: /config/config.txt");
        return;
    }

    std::string line;
    std::regex connectRegex(R"(^connect_wi-fi\s+(\S+)\s+(\S+)$)");
    std::smatch match;

    while (std::getline(configFile, line)) {
        // Skip comments and empty lines
        if (line.empty() || line[0] == '#') {
            continue;
        }

        // Check for Wi-Fi connection command
        if (std::regex_search(line, match, connectRegex)) {
            std::string networkName = match[1];
            std::string networkPassword = match[2];
            spdlog::debug("line: {} , networkName: {} , networkPassword: {}",line, networkName, networkPassword);
            strncpy(password, networkPassword.c_str(), 256); // Copy password into the buffer
            password[255] = '\0'; // Ensure null termination
            availableWlans.clear();
            availableWlans.push_back({"00:11:22:33:44:55", networkName, -1, 0, "unknown"});
            continue;
        }

        // Parse WLAN definitions (e.g., stub for future extensions)
        // Example parsing logic could go here if WLANs are defined explicitly in the file
        // For now, we'll assume WLANs are not directly listed in this specific file.
    }

    configFile.close();

    // Example: Adding a stub WLAN to `availableWlans` for demonstration purposes
}