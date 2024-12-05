#include <spdlog/spdlog.h>
#include <yaml-cpp/yaml.h>
#include <iostream>
#include <string>
#include <vector>

// Define a struct to hold the GPIO configuration for each direction
struct GPIO {
    std::string chip;
    int line;
};

// Define a struct to represent the full GPIO configuration
struct GPIOConfig {
    GPIO up;
    GPIO down;
    GPIO left;
    GPIO right;
    GPIO ok;
};

// Define a struct to hold the WLAN channel and frequency
struct WFBNGChannel {
    int channel;
    int frequency;
};

// Specialize YAML::convert for GPIO
namespace YAML {
    template<>
    struct convert<GPIO> {
        static bool decode(const Node& node, GPIO& gpio) {
            if (node.IsMap()) {
                gpio.chip = node["chip"].as<std::string>();
                gpio.line = node["line"].as<int>();
                spdlog::debug("Loaded chip {} line {}", gpio.chip, gpio.line);
                return true;
            }
            return false;
        }
    };

    // Specialize for the full GPIOConfig
    template<>
    struct convert<GPIOConfig> {
        static bool decode(const Node& node, GPIOConfig& config) {
            if (node.IsMap()) {
                config.up = node["up"].as<GPIO>();
                config.down = node["down"].as<GPIO>();
                config.left = node["left"].as<GPIO>();
                config.right = node["right"].as<GPIO>();
                config.ok = node["ok"].as<GPIO>();
                return true;
            }
            return false;
        }
    };

    // Specialize YAML::convert for WLANChannel
    template<>
    struct convert<WFBNGChannel> {
        static bool decode(const Node& node, WFBNGChannel& wlanChannel) {
            if (node.IsMap()) {
                wlanChannel.channel = node["channel"].as<int>();
                wlanChannel.frequency = node["frequency"].as<int>();
                spdlog::debug("Loaded channel {} frequency {}", wlanChannel.channel, wlanChannel.frequency);

                return true;
            }
            return false;
        }
    };

    // Specialize for the list of WLANChannels
    template<>
    struct convert<std::vector<WFBNGChannel>> {
        static bool decode(const Node& node, std::vector<WFBNGChannel>& wlanChannels) {
            if (node.IsSequence()) {
                for (const auto& item : node) {
                    WFBNGChannel wlanChannel = item.as<WFBNGChannel>();
                    wlanChannels.push_back(wlanChannel);
                }
                return true;
            }
            return false;
        }
    };
}