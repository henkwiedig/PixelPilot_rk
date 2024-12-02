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
        bool isPressed = false;
        std::chrono::steady_clock::time_point pressStart;
    };

    ~GPIOManager();

    void addLine(const std::string& name, const std::string& chip_name, int line_num);
    int getValue(const std::string& name) const;

    // Non-blocking press detection
    std::string detectPressNonBlocking(const std::string& name, int longPressThresholdMs = 2000);

private:
    std::map<std::string, Line> lines;
    std::map<std::string, PressState> states;
};

#endif // GPIOMANAGER_H
