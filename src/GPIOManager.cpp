#include "GPIOManager.hpp"
#include <stdexcept>
#include <thread>
#include <chrono>

// Add a GPIO line
void GPIOManager::addLine(const std::string& name, const std::string& chip_name, int line_num) {
    gpiod_chip* chip = gpiod_chip_open_by_name(chip_name.c_str());
    if (!chip) {
        throw std::runtime_error("Failed to open GPIO chip: " + chip_name);
    }

    gpiod_line* line = gpiod_chip_get_line(chip, line_num);
    if (!line) {
        gpiod_chip_close(chip);
        throw std::runtime_error("Failed to get GPIO line: " + std::to_string(line_num));
    }

    if (gpiod_line_request_input(line, "gpio_manager") < 0) {
        gpiod_chip_close(chip);
        throw std::runtime_error("Failed to request input for GPIO line");
    }

    lines[name] = {chip, line};
    states[name] = {}; // Initialize the press state
}

// Get the value of a GPIO line
int GPIOManager::getValue(const std::string& name) const {
    auto it = lines.find(name);
    if (it == lines.end()) {
        throw std::runtime_error("No such line: " + name);
    }
    return gpiod_line_get_value(it->second.line);
}

// Non-blocking press detection
std::string GPIOManager::detectPressNonBlocking(const std::string& name, int longPressThresholdMs) {
    auto lineIt = lines.find(name);
    if (lineIt == lines.end()) {
        throw std::runtime_error("No such line: " + name);
    }

    auto stateIt = states.find(name);
    if (stateIt == states.end()) {
        throw std::runtime_error("No state found for line: " + name);
    }

    Line& line = lineIt->second;
    PressState& state = stateIt->second;

    // Read current line value (0 = pressed, 1 = released)
    int value = gpiod_line_get_value(line.line);
    if (value < 0) {
        throw std::runtime_error("Failed to read GPIO line: " + name);
    }

    // Handle state transitions
    if (value == 0 && !state.isPressed) {
        // Button just pressed
        state.isPressed = true;
        state.pressStart = std::chrono::steady_clock::now();
        return ""; // No press detected yet
    } else if (value == 1 && state.isPressed) {
        // Button just released
        state.isPressed = false;

        auto now = std::chrono::steady_clock::now();
        auto pressDurationMs = std::chrono::duration_cast<std::chrono::milliseconds>(now - state.pressStart).count();

        // Classify press type
        return (pressDurationMs >= longPressThresholdMs) ? "long" : "short";
    }

    // No state change detected
    return "";
}

// Destructor to release resources
GPIOManager::~GPIOManager() {
    for (auto& [name, line] : lines) {
        gpiod_chip_close(line.chip);
    }
}
