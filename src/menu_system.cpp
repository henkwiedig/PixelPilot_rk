#include "menu_system.hpp"
#include "drm.h"
#include "spdlog.h"


std::vector<std::shared_ptr<MenuOption>> loadMenuOptions(const YAML::Node& node) {
    std::vector<std::shared_ptr<MenuOption>> options;

    for (const auto& item : node) {
        auto option = std::make_shared<MenuOption>(item["name"].as<std::string>());
        SPDLOG_INFO("{}",option->name.c_str());

        if (item["submenu"]) {
            option->submenu = loadMenuOptions(item["submenu"]);
        }

        options.push_back(option);
    }

    return options;
}

Menu* loadMenuFromYaml(const std::string& filepath) {
    YAML::Node config = YAML::LoadFile(filepath);
    auto options = loadMenuOptions(config["menu"]);
    return new Menu(options);
}

void Menu::drawMenu(struct modeset_buf *buf) const {

	cairo_t* cr;
	cairo_surface_t *surface;

    const int optionHeight = 40;
    int yOffset = 50; // Start drawing from this Y-offset
    int width, height;

    int osd_x = buf->width - 300;
	surface = cairo_image_surface_create_for_data(buf->map, CAIRO_FORMAT_ARGB32, buf->width, buf->height, buf->stride);
	cr = cairo_create (surface);

	// https://www.cairographics.org/FAQ/#clear_a_surface
	cairo_save(cr);
	cairo_set_operator(cr, CAIRO_OPERATOR_CLEAR);
	cairo_paint(cr);
	cairo_restore(cr);

    cairo_set_source_rgba(cr, 0, 0, 0, 0); // Transparent black (alpha = 0)
    cairo_paint(cr);

    for (size_t i = 0; i < menuOptions.size(); ++i) {
        if (i == currentIndex) {
            cairo_set_source_rgb(cr, 0.2, 0.6, 1); // Highlight color
            cairo_rectangle(cr, 20, yOffset, width - 40, optionHeight);
            cairo_fill(cr);

            cairo_set_source_rgb(cr, 1, 1, 1); // Text color for highlighted option
        } else {
            cairo_set_source_rgb(cr, 0, 0, 0); // Text color
        }

        cairo_move_to(cr, 30, yOffset + optionHeight / 2 + 5);
        cairo_set_font_size(cr, 20);
        cairo_show_text(cr, menuOptions[i]->name.c_str());
        yOffset += optionHeight + 10; // Space between options
    }
    cairo_destroy(cr);
}

Menu* Menu::handleInput(char input) {
    if (input == 'w' && currentIndex > 0) {
        --currentIndex;
    } else if (input == 's' && currentIndex < menuOptions.size() - 1) {
        ++currentIndex;
    } else if (input == 'd' && hasSubmenu()) {
        return enterSubmenu();
    } else if (input == 'a' && parent) {
        return exitToParent();
    }
    return this;
}

bool Menu::hasSubmenu() const {
    return !menuOptions[currentIndex]->submenu.empty();
}

Menu* Menu::enterSubmenu() {
    return new Menu(menuOptions[currentIndex]->submenu, this);
}

Menu* Menu::exitToParent() {
    return parent;
}
