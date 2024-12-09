#include "GPIOManager.hpp"
#include "spdlog.h"
#include <stdexcept>
#include <thread>
#include <chrono>

constexpr int DEBOUNCE_DELAY_MS = 80;

// Add a GPIO line
void GPIOManager::addLine(const std::string& name, const std::string& chip_name, int line_num, bool activeHigh) {
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
    linePolarity[name] = activeHigh; // Store the polarity
}


int GPIOManager::getValue(const std::string& name) {
    auto it = lines.find(name);
    if (it == lines.end()) {
        throw std::runtime_error("No such line: " + name);
    }

    auto stateIt = states.find(name);
    if (stateIt == states.end()) {
        throw std::runtime_error("No state found for line: " + name);
    }

    Line& line = it->second;
    PressState& state = stateIt->second;

    int rawValue = gpiod_line_get_value(line.line);
    if (rawValue < 0) {
        throw std::runtime_error("Failed to read GPIO line: " + name);
    }

    // Adjust value based on active state
    bool isActiveLow = linePolarity[name];
    int adjustedValue = isActiveLow ? !rawValue : rawValue;
    auto now = std::chrono::steady_clock::now();
    auto elapsedSinceLastChange = std::chrono::duration_cast<std::chrono::milliseconds>(now - state.lastDebounceTime).count();

    if (adjustedValue != state.lastStableValue) {
        state.lastDebounceTime = now;
    }

    if (elapsedSinceLastChange > DEBOUNCE_DELAY_MS) {
        if (adjustedValue != state.lastStableValue) {
            state.lastStableValue = adjustedValue;
        }
    }

    return state.lastStableValue;
}


// Non-blocking press detection with debounce
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
    int rawValue = gpiod_line_get_value(line.line);
    if (rawValue < 0) {
        throw std::runtime_error("Failed to read GPIO line: " + name);
    }

    // Adjust based on active state
    bool isActiveLow = linePolarity[name];
    int adjustedValue = isActiveLow ? !rawValue : rawValue;
    auto now = std::chrono::steady_clock::now();
    auto elapsedSinceLastChange = std::chrono::duration_cast<std::chrono::milliseconds>(now - state.lastDebounceTime).count();

    // Handle debounce
    if (adjustedValue != state.lastStableValue) {
        // Value changed, start debounce timer
        state.lastDebounceTime = now;
    }

    if (elapsedSinceLastChange > DEBOUNCE_DELAY_MS) {
        // If the stable state has changed after debounce time, update the last stable value
        if (adjustedValue != state.lastStableState) {
            state.lastStableState = adjustedValue;

            // Handle state transitions
            if (adjustedValue == 0 && !state.isPressed) {
                // Button just pressed
                state.isPressed = true;
                state.pressStart = now;
                return ""; // No press detected yet
            } else if (adjustedValue == 1 && state.isPressed) {
                // Button just released
                state.isPressed = false;

                auto pressDurationMs = std::chrono::duration_cast<std::chrono::milliseconds>(now - state.pressStart).count();

                // Classify press type
                return (pressDurationMs >= longPressThresholdMs) ? "long" : "short";
            }
        }
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
