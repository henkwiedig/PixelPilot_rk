#ifndef GPIO_H
#define GPIO_H

#include <gpiod.h>
#include <string>
#include <iostream>
#include <unistd.h>

class GPIO {
public:
    GPIO(const std::string& chipName, unsigned int lineNum);
    ~GPIO();

    void listenForChanges();

private:
    struct gpiod_chip *chip;  // Pointer to GPIO chip
    struct gpiod_line *line;   // Pointer to GPIO line
};

#endif // GPIO_H
