#ifndef GPIOMANAGER_H
#define GPIOMANAGER_H

#include <gpiod.h>
#include <string>
#include <vector>
#include <map>
#include <stdexcept>

// A struct to manage GPIO lines across multiple chips
class GPIOManager {
public:
    // Struct to store chip and line information
    struct Line {
        gpiod_chip* chip;       // Pointer to the GPIO chip
        gpiod_line* line;       // Pointer to the GPIO line
    };

    // Destructor to clean up resources
    ~GPIOManager() {
        for (auto& [name, line] : lines) {
            gpiod_chip_close(line.chip);
        }
    }

    // Add a GPIO line to the manager
    void addLine(const std::string& name, const std::string& chip_name, int line_num);
    // Get the value of a GPIO line
    int getValue(const std::string& name) const;

private:
    std::map<std::string, Line> lines; // Map of line names to Line structs
};

#endif // GPIOMANAGER_H
