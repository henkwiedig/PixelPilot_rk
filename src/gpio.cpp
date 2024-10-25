#include "gpio.h"

GPIO::GPIO(const std::string& chipName, unsigned int lineNum) {
    // Open the GPIO chip
    chip = gpiod_chip_open_by_name(chipName.c_str());
    if (!chip) {
        perror("Failed to open GPIO chip");
        exit(EXIT_FAILURE);
    }

    // Get the GPIO line
    line = gpiod_chip_get_line(chip, lineNum);
    if (!line) {
        perror("Failed to get GPIO line");
        gpiod_chip_close(chip);
        exit(EXIT_FAILURE);
    }

    // Request the GPIO line as an input with edge detection
    int ret = gpiod_line_request_both_edges_events(line, "gpiomon");
    if (ret < 0) {
        perror("Failed to request GPIO line");
        gpiod_chip_close(chip);
        exit(EXIT_FAILURE);
    }
}

GPIO::~GPIO() {
    if (line) {
        gpiod_line_release(line);
    }
    if (chip) {
        gpiod_chip_close(chip);
    }
}

void GPIO::listenForChanges() {
    struct gpiod_line_event event;
    while (true) {
        // Wait for an event on the GPIO line
        int ret = gpiod_line_event_wait(line, NULL);
        if (ret < 0) {
            perror("Error waiting for GPIO event");
            break;
        }

        // Read the event
        ret = gpiod_line_event_read(line, &event);
        if (ret < 0) {
            perror("Error reading GPIO event");
            break;
        }

        // React to the GPIO event
        std::cout << "GPIO line event: ";
        if (event.event_type == GPIOD_LINE_EVENT_RISING_EDGE) {
            std::cout << "RISING" << std::endl;
        } else if (event.event_type == GPIOD_LINE_EVENT_FALLING_EDGE) {
            std::cout << "FALLING" << std::endl;
        }
    }
}
