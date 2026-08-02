#include "WiFiMonitor.hpp"
#include <algorithm>
#include <cstring>
#include <fstream>
#include <regex>
#include <sstream>
#include <string>
#include <unordered_map>
#include <vector>
#include "spdlog/spdlog.h"

extern "C" {
#include "osd.h"
}

namespace fs = std::filesystem;

namespace {

/* Realtek drivers register their proc directory under the driver name:
 * rtl88x2eu, rtl88x2cu, rtl88x2bu, rtl8812au, ... */
constexpr const char* DRIVER_DIR_PREFIX = "rtl";

void set_tag(osd_tag* tag, const char* key, const std::string& val) {
    strncpy(tag->key, key, TAG_MAX_LEN - 1);
    strncpy(tag->val, val.c_str(), TAG_MAX_LEN - 1);
    tag->key[TAG_MAX_LEN - 1] = '\0';
    tag->val[TAG_MAX_LEN - 1] = '\0';
}

std::string trim(const std::string& s) {
    const auto begin = s.find_first_not_of(" \t\r\n");
    if (begin == std::string::npos) {
        return "";
    }
    return s.substr(begin, s.find_last_not_of(" \t\r\n") - begin + 1);
}

}  // namespace

// Constructor implementation
WiFiMonitor::WiFiMonitor()
    : base_path_("/proc/net"), last_thermal_read_(), warned_no_driver_(false) {}

// WiFiStats constructor implementation
WiFiMonitor::WiFiStats::WiFiStats() : rssi_a(0), rssi_b(0), rssi_min(0), rssi_percent(0), is_linked(false) {}

std::vector<fs::path> WiFiMonitor::find_interfaces() {
    std::vector<fs::path> interfaces;
    std::error_code ec;

    // directory_iterator(ec) yields end() instead of throwing when /proc/net or a
    // driver directory disappears (USB adapter unplugged) while we walk it.
    for (const auto& driver : fs::directory_iterator(base_path_, ec)) {
        if (!driver.is_directory(ec)) continue;
        if (driver.path().filename().string().rfind(DRIVER_DIR_PREFIX, 0) != 0) continue;

        for (const auto& interface : fs::directory_iterator(driver.path(), ec)) {
            if (interface.is_directory(ec)) {
                interfaces.push_back(interface.path());
            }
        }
    }

    // The position in this list is published as the `adapter` tag, so sort by
    // interface name: readdir order is not guaranteed, and cards may sit below
    // different drivers.
    std::sort(interfaces.begin(), interfaces.end(),
              [](const fs::path& a, const fs::path& b) { return a.filename() < b.filename(); });

    return interfaces;
}

void WiFiMonitor::run() {

    std::vector<fs::path> interfaces = find_interfaces();
    if (interfaces.empty()) {
        if (!warned_no_driver_) {
            spdlog::error("No Realtek WiFi driver found below {}, no wifi stats will be published", base_path_);
            warned_no_driver_ = true;
        }
        return;
    }
    warned_no_driver_ = false;

    // Temperature is sampled on its own, slower schedule (see THERMAL_INTERVAL)
    const auto now = std::chrono::steady_clock::now();
    const bool read_thermal = (now - last_thermal_read_) >= THERMAL_INTERVAL;
    if (read_thermal) {
        last_thermal_read_ = now;
    }

    // Initialize batch - estimate 6 facts per interface
    void* batch = osd_batch_init(24);

    // Collect the stats of every interface of every driver
    for (size_t adapter = 0; adapter < interfaces.size(); adapter++) {
        const fs::path& interface = interfaces[adapter];

        osd_tag base_tags[BASE_TAGS];
        make_base_tags(base_tags, interface.filename(), (int)adapter);

        std::string debug_file = interface / "trx_info_debug";
        if (fs::exists(debug_file)) {
            WiFiStats stats = parse_interface_stats(debug_file);
            add_interface_stats_to_batch(batch, base_tags, stats);
        }

        std::string thermal_file = interface / "thermal_state";
        if (read_thermal && fs::exists(thermal_file)) {
            for (const auto& thermal : parse_thermal_state(thermal_file)) {
                add_temperature_fact_to_batch(batch, base_tags, thermal.rf_path, thermal.temperature);
            }
        }
    }

    // Publish all collected facts
    osd_publish_batch(batch);
}

WiFiMonitor::WiFiStats WiFiMonitor::parse_interface_stats(const std::string& file_path) {

    WiFiStats stats;
    std::ifstream file(file_path);

    if (!file.is_open()) {
        return stats;
    }

    std::string line;
    while (std::getline(file, line)) {
        // Parse RSSI A and B
        if (line.find("rssi_a =") != std::string::npos) {
            std::regex rssi_ab_regex(R"(rssi_a\s*=\s*(\d+)\(%\),\s*rssi_b\s*=\s*(\d+)\(%\))");
            std::smatch match;
            if (std::regex_search(line, match, rssi_ab_regex) && match.size() == 3) {
                stats.rssi_a = std::stoi(match[1]);
                stats.rssi_b = std::stoi(match[2]);
            }
        }
        // Parse RSSI percentage
        else if (line.find("rssi :") != std::string::npos) {
            std::regex rssi_regex(R"(rssi\s*:\s*(\d+)\s*\(\%\))");
            std::smatch match;
            if (std::regex_search(line, match, rssi_regex) && match.size() > 1) {
                stats.rssi_percent = std::stoi(match[1]);
            }
        }
        else if (line.find("is_linked =") != std::string::npos) {
            std::regex linked_regex(R"(is_linked\s*=\s*(\d+))");
            std::smatch match;
            if (std::regex_search(line, match, linked_regex) && match.size() > 1) {
                stats.is_linked = (std::stoi(match[1]) == 1);
            }
        }
    }

    file.close();
    return stats;
}

std::vector<WiFiMonitor::ThermalStats> WiFiMonitor::parse_thermal_state(const std::string& file_path) {
    // One line per RF path, e.g.
    //   rf_path: 0, thermal_value: 53, offset: 21, temperature: 112
    // Parsed as generic comma-separated "key: value" pairs (like wfb-ng does), so
    // additional or reordered fields don't break the parser.
    std::vector<ThermalStats> thermals;
    std::ifstream file(file_path);

    if (!file.is_open()) {
        return thermals;
    }

    std::string line;
    while (std::getline(file, line)) {
        std::unordered_map<std::string, int> fields;
        std::stringstream fields_stream(line);
        std::string field;

        while (std::getline(fields_stream, field, ',')) {
            const auto colon = field.find(':');
            if (colon == std::string::npos) continue;
            try {
                fields[trim(field.substr(0, colon))] = std::stoi(field.substr(colon + 1));
            } catch (const std::exception&) {
                // Not a numeric field, ignore it
            }
        }

        const auto rf_path = fields.find("rf_path");
        const auto temperature = fields.find("temperature");
        if (rf_path != fields.end() && temperature != fields.end()) {
            thermals.push_back({rf_path->second, temperature->second});
        }
    }

    file.close();
    return thermals;
}

void WiFiMonitor::make_base_tags(osd_tag* tags, const std::string& interface_name, int adapter) {
    set_tag(&tags[0], "interface", interface_name);
    set_tag(&tags[1], "adapter", std::to_string(adapter));
}

void WiFiMonitor::add_interface_stats_to_batch(void* batch, const osd_tag* base_tags, const WiFiStats& stats) {
    if (!stats.is_linked) {
        return; // Don't publish stats for disconnected interfaces
    }

    // Publish RSSI A
    add_rssi_fact_to_batch(batch, base_tags, "rssi_a", stats.rssi_a);

    // Publish RSSI B
    add_rssi_fact_to_batch(batch, base_tags, "rssi_b", stats.rssi_b);

    // Publish RSSI Overall Percentage
    add_rssi_fact_to_batch(batch, base_tags, "rssi_percent", stats.rssi_percent);

    // Publish connection status
    add_rssi_fact_to_batch(batch, base_tags, "connected", 1);
}

void WiFiMonitor::add_rssi_fact_to_batch(void* batch, const osd_tag* base_tags, const std::string& rssi_type, int value) {
    osd_tag tags[N_TAGS];

    // Copy interface and adapter tags
    memcpy(tags, base_tags, BASE_TAGS * sizeof(osd_tag));

    // Add type tag
    set_tag(&tags[BASE_TAGS], "type", rssi_type);

    // Add fact to batch
    osd_add_int_fact(batch, "os_mon.wifi.rssi", tags, N_TAGS, value);
}

void WiFiMonitor::add_temperature_fact_to_batch(void* batch, const osd_tag* base_tags, int rf_path, int value) {
    osd_tag tags[N_TAGS];

    // Copy interface and adapter tags
    memcpy(tags, base_tags, BASE_TAGS * sizeof(osd_tag));

    // Add the RF path the temperature was measured on
    set_tag(&tags[BASE_TAGS], "rf_path", std::to_string(rf_path));

    // Add fact to batch, degrees C
    osd_add_int_fact(batch, "os_mon.wifi.temperature", tags, N_TAGS, value);
}

void WiFiMonitor::publish_reset() {
    std::vector<fs::path> interfaces = find_interfaces();
    if (interfaces.empty()) {
        spdlog::warn("No Realtek WiFi driver found below {} for reset", base_path_);
        return;
    }

    // Initialize batch - estimate 6 facts per interface
    void* batch = osd_batch_init(24);

    // Publish reset values for all interfaces of all drivers
    for (size_t adapter = 0; adapter < interfaces.size(); adapter++) {
        publish_interface_reset(batch, interfaces[adapter], (int)adapter);
    }

    // Publish all reset facts
    osd_publish_batch(batch);

    spdlog::debug("Published WiFi reset values for all interfaces");
}

void WiFiMonitor::publish_interface_reset(void* batch, const fs::path& interface_path, int adapter) {
    // Prepare common tags
    osd_tag base_tags[BASE_TAGS];
    make_base_tags(base_tags, interface_path.filename(), adapter);

    // Publish all RSSI values as -1 (reset/error value)
    add_rssi_fact_to_batch(batch, base_tags, "rssi_a", -1);
    add_rssi_fact_to_batch(batch, base_tags, "rssi_b", -1);
    add_rssi_fact_to_batch(batch, base_tags, "rssi_min", -1);
    add_rssi_fact_to_batch(batch, base_tags, "rssi_percent", -1);
    add_rssi_fact_to_batch(batch, base_tags, "connected", 0);  // 0 = disconnected

    // Same for the temperature of every RF path the card reports
    std::string thermal_file = interface_path / "thermal_state";
    if (fs::exists(thermal_file)) {
        for (const auto& thermal : parse_thermal_state(thermal_file)) {
            add_temperature_fact_to_batch(batch, base_tags, thermal.rf_path, -1);
        }
    }
}

// C-callable function implementations
extern "C" {

void wifi_monitor_reset(void) {
    static WiFiMonitor monitor;
    monitor.publish_reset();
}

} // extern "C"
