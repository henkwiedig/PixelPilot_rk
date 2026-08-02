#ifndef WIFI_MONITOR_H
#define WIFI_MONITOR_H

#include <chrono>
#include <filesystem>
#include <string>
#include <vector>

extern "C" {
#include "osd.h"
}

/*
 * Link stats of the directly attached WiFi adapters, published as os_mon.wifi.*
 * facts. Used in APFPV mode, where wfb-ng (and with it the wfbcli.* facts) does
 * not run.
 *
 * The Realtek rtlwifi-family drivers expose per-interface stats under
 * /proc/net/<driver>/<interface>/:
 *   trx_info_debug - RSSI per RF path and link state
 *   thermal_state  - RF chip temperature per RF path
 * <driver> is the driver name and therefore differs per card family (rtl88x2eu,
 * rtl88x2cu, rtl88x2bu, rtl8812au, ...), so every rtl* directory is scanned
 * instead of hardcoding one. Files that a given driver does not provide are
 * skipped (thermal_state only exists on the libc0607-derived 88x2 drivers).
 *
 * Every fact carries an `adapter` tag, the index of the card in the (sorted, so
 * stable) list of all adapters found. Without it a widget can only match on
 * `interface`, whose name is derived from the card's MAC - so an OSD binding
 * would have to be written per ground station, and a binding that leaves the
 * interface open would be fed by all cards in turn. This is the APFPV
 * counterpart of the `ant_id` tag wfb-ng puts on the wfbcli.* facts.
 */
class WiFiMonitor {
public:
    WiFiMonitor();
    void run();
    void publish_reset();

private:
    struct WiFiStats {
        int rssi_a;
        int rssi_b;
        int rssi_min;
        int rssi_percent;
        bool is_linked;

        WiFiStats();
    };

    /* One RF path of a card; temperature is in degrees C, as computed by the driver. */
    struct ThermalStats {
        int rf_path;
        int temperature;
    };

    /* Reading thermal_state makes the driver trigger an ADC conversion on the RF
     * chip, so it is sampled far slower than run() is called. Matches wfb-ng's
     * temp_measurement_interval default. */
    static constexpr std::chrono::seconds THERMAL_INTERVAL{10};

    /* Published for an adapter that is not linked. Deliberately outside the 0..100
     * percentage range, so an icon widget hides unless the config maps it. */
    static constexpr int RSSI_NONE = -1;

    std::string base_path_;
    std::chrono::steady_clock::time_point last_thermal_read_;
    bool warned_no_driver_;
    std::vector<std::string> known_interfaces_;  // adapters seen on the previous run()

    /* Number of tags every fact carries: `interface` and `adapter`, plus the one
     * describing the value itself (`type` / `rf_path`). Kept identical for all
     * facts of the same name so a widget can filter on any of them. */
    static constexpr int BASE_TAGS = 2;
    static constexpr int N_TAGS = BASE_TAGS + 1;

    /* All /proc/net/<rtl driver>/<interface> directories currently present, sorted
     * by interface name so the adapter index of a card doesn't depend on readdir
     * order (it does change when adapters are added or removed, though). */
    std::vector<std::filesystem::path> find_interfaces();
    void flush_if_interfaces_changed(const std::vector<std::filesystem::path>& interfaces);
    WiFiStats parse_interface_stats(const std::string& file_path);
    std::vector<ThermalStats> parse_thermal_state(const std::string& file_path);
    void make_base_tags(osd_tag* tags, const std::string& interface_name, int adapter);
    void add_interface_stats_to_batch(void* batch, const osd_tag* base_tags, const WiFiStats& stats);
    void add_rssi_fact_to_batch(void* batch, const osd_tag* base_tags, const std::string& rssi_type, int value);
    void add_temperature_fact_to_batch(void* batch, const osd_tag* base_tags, int rf_path, int value);
    void publish_interface_reset(void* batch, const std::filesystem::path& interface_path, int adapter);

};

#endif
