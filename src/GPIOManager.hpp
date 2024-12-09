#ifndef GPIOMANAGER_H
#define GPIOMANAGER_H

#include <gpiod.h>
#include <string>
#include <map>
#include <chrono>

class GPIOManager {
public:
    struct Line {
        gpiod_chip* chip;
        gpiod_line* line;
    };

    struct PressState {
        bool isPressed = false;                      // Tracks if the button is currently pressed
        std::chrono::steady_clock::time_point pressStart; // Tracks the time when the press started
        std::chrono::steady_clock::time_point lastDebounceTime; // Last debounce time
        int lastStableValue = -1;                    // Last stable GPIO value (after debouncing)
        int lastStableState = -1;                    // The stable state of the GPIO pin (either 0 or 1)
    };

    ~GPIOManager();

    void addLine(const std::string& name, const std::string& chip_name, int line_num, bool activeHigh);
    int getValue(const std::string& name);

    // Non-blocking press detection
    std::string detectPressNonBlocking(const std::string& name, int longPressThresholdMs = 2000);

private:
    std::map<std::string, Line> lines;
    std::map<std::string, PressState> states;
    std::map<std::string, bool> linePolarity;
};

#endif // GPIOMANAGER_H
