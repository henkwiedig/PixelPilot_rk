#include "GPIOManager.hpp"
#include <stdexcept>

// Add a GPIO line to the manager
void GPIOManager::addLine(const std::string& name, const std::string& chip_name, int line_num) {
    // Open the GPIO chip
    gpiod_chip* chip = gpiod_chip_open_by_name(chip_name.c_str());
    if (!chip) {
        throw std::runtime_error("Failed to open GPIO chip: " + chip_name);
    }

    // Get the GPIO line
    gpiod_line* line = gpiod_chip_get_line(chip, line_num);
    if (!line) {
        gpiod_chip_close(chip);
        throw std::runtime_error("Failed to get GPIO line: " + std::to_string(line_num));
    }

    // Request the line as input
    if (gpiod_line_request_input(line, "gpio_manager") < 0) {
        gpiod_chip_close(chip);
        throw std::runtime_error("Failed to request input for GPIO line");
    }

    // Store the chip and line in the map
    lines[name] = {chip, line};
}

// Get the value of a GPIO line
int GPIOManager::getValue(const std::string& name) const {
    // Find the line by name
    auto it = lines.find(name);
    if (it == lines.end()) {
        throw std::runtime_error("No such line: " + name);
    }

    // Get the value of the line (0 or 1)
    return gpiod_line_get_value(it->second.line);
}
