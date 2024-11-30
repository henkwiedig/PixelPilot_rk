#pragma once
#include <cairo.h>
#include <vector>
#include <memory>
#include <string>
#include "yaml-cpp/yaml.h"

class MenuOption {
public:
    std::string name;
    std::vector<std::shared_ptr<MenuOption>> submenu;

    MenuOption(const std::string& name) : name(name) {}
};

class Menu {
    std::vector<std::shared_ptr<MenuOption>> menuOptions;
    int currentIndex = 0;
    Menu* parent = nullptr;

public:
    Menu(const std::vector<std::shared_ptr<MenuOption>>& options, Menu* parentMenu = nullptr)
        : menuOptions(options), parent(parentMenu) {}

    void drawMenu(struct modeset_buf *buf) const;
    Menu* handleInput(char input);
    bool hasSubmenu() const;
    Menu* enterSubmenu();
    Menu* exitToParent();
};

// Declare the loader functions
std::vector<std::shared_ptr<MenuOption>> loadMenuOptions(const YAML::Node& node);
Menu* loadMenuFromYaml(const std::string& filepath);