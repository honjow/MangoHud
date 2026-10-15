#include "battery.h"
#include <filesystem.h>
#include <cmath>
#include <fstream>
#include <functional>
#include <iostream>
#include <stdexcept>
#include <string>
#include <unistd.h>

namespace fs = ghc::filesystem;
using Clock = std::chrono::steady_clock;
static Clock::time_point fixture_time{};
static Clock::time_point now() { return fixture_time; }

struct BatteryTestAccess {
    static void configure(BatteryStats& stats, const fs::path& root) {
        stats.power_supply_path = (root / "power_supply").string();
        stats.deckard_charger_path = (root / "deckardcharger").string();
        stats.deckard_power_path = (root / "power-monitor/power").string();
        stats.sample_clock = now;
    }
};

static int assertions = 0;
static void check(bool ok, const std::string& message) {
    ++assertions;
    if (!ok)
        throw std::runtime_error(message);
}
static void near(float got, float expected, const std::string& message, float tolerance = 0.001f) {
    check(std::isfinite(got) && std::fabs(got - expected) <= tolerance,
          message + ": got " + std::to_string(got) + ", expected " + std::to_string(expected));
}
static void write(const fs::path& path, const std::string& value) {
    fs::create_directories(path.parent_path());
    std::ofstream file(path.string());
    if (!(file << value << '\n'))
        throw std::runtime_error("Could not write fixture " + path.string());
}
static void advance(double seconds) {
    fixture_time += std::chrono::duration_cast<Clock::duration>(std::chrono::duration<double>(seconds));
}
struct Fixture {
    fs::path root;
    BatteryStats stats;
    Fixture() {
        static unsigned sequence = 0;
        root = fs::temp_directory_path() /
            ("mangohud-battery-" + std::to_string(getpid()) + "-" + std::to_string(sequence++));
        check(!fs::exists(root), "Fixture path must be new");
        fs::create_directories(root);
        fixture_time = Clock::time_point{};
        BatteryTestAccess::configure(stats, root);
        stats.batt_check = true;
    }
    ~Fixture() {
        std::error_code ec;
        fs::remove_all(root, ec);
    }
    fs::path add(const std::string& name = "BAT0", const std::string& status = "Discharging") {
        check(stats.batt_count < battery_count_max, "Fixture within battery capacity");
        const fs::path path = root / "power_supply" / name;
        write(path / "type", "Battery");
        write(path / "status", status);
        write(path / "voltage_now", "15000000");
        stats.battPath[stats.batt_count++] = path.string();
        return path;
    }
};

static void healthy_current() {
    Fixture f; auto p = f.add();
    write(p / "current_now", "2000000");
    write(p / "charge_now", "5000000");
    write(p / "charge_full", "10000000");
    f.stats.update();
    near(f.stats.current_watt, 30, "Normal V*I");
    near(f.stats.remaining_time, 2.5, "Charge/current hours");
    near(f.stats.current_percent, 50, "Charge percentage");
    check(f.stats.current_status == "Discharging", "Discharge sign state");
    write(p / "current_now", "-2000000");
    near(f.stats.getPower(), 30, "Signed firmware current magnitude");
}
static void charging_and_idle() {
    Fixture f; auto p = f.add("BAT0", "Charging");
    write(p / "power_now", "-20000000");
    near(f.stats.getPower(), 20, "Charging power retained");
    check(f.stats.current_status == "Charging", "Charging sign state");
    for (const auto& status : {"Full", "Unknown", "Not charging", ""}) {
        write(p / "status", status);
        near(f.stats.getPower(), 0, "Idle status has no charge/discharge power");
        near(f.stats.getTimeRemaining(), 0, "Idle has no stale runtime");
    }
}
static void current_limits() {
    Fixture f; auto p = f.add();
    write(p / "charge_now", "5000000");
    write(p / "current_now", "30000000");
    near(f.stats.getPower(), 450, "30A threshold remains inclusive");
    write(p / "current_now", "31000000");
    near(f.stats.getPower(), 0, "Over 30A starts capacity baseline");
    advance(2); write(p / "charge_now", "4999000");
    near(f.stats.getPower(), 27, "Over 30A uses capacity");
    write(p / "current_now", "0");
    near(f.stats.getPower(), 27, "Zero current keeps capacity fallback");
    fs::remove(p / "current_now");
    near(f.stats.getPower(), 27, "Missing current keeps capacity fallback");
}
static void claw_steps_and_smoothing() {
    Fixture f; auto p = f.add();
    write(p / "current_now", "65000000");
    write(p / "charge_now", "5000000");
    near(f.stats.getPower(), 0, "65A initial estimate unavailable");
    advance(0.5); write(p / "charge_now", "4999000");
    near(f.stats.getPower(), 0, "Subsecond step waits");
    advance(0.5);
    near(f.stats.getPower(), 54, "Exact charge delta watts");
    near(f.stats.getPower(), 54, "Repeated sample preserves estimate");
    advance(2);
    near(f.stats.getPower(), 54, "Unchanged capacity preserves estimate");
    advance(2); write(p / "charge_now", "4997000");
    near(f.stats.getPower(), 45.9, "0.7/0.3 EMA includes idle interval");
}
static void charging_delta() {
    Fixture f; auto p = f.add("BAT0", "Charging");
    write(p / "current_now", "65000000"); write(p / "charge_now", "4000000");
    near(f.stats.getPower(), 0, "Charging fallback baseline");
    advance(2); write(p / "charge_now", "4001000");
    near(f.stats.getPower(), 27, "Charging increasing capacity");
    check(f.stats.current_status == "Charging", "Fallback keeps plus sign state");
}
static void anomalous_power() {
    Fixture f; auto p = f.add();
    write(p / "power_now", "999000000"); write(p / "energy_now", "50000000");
    near(f.stats.getPower(), 0, "Bad power initial baseline");
    advance(2); write(p / "energy_now", "49990000");
    near(f.stats.getPower(), 18, "Bad power uses energy delta");
    write(p / "power_now", "200000000");
    near(f.stats.getPower(), 200, "200W threshold remains inclusive");
    write(p / "power_now", "201000000");
    near(f.stats.getPower(), 0, "Return to broken sensor starts fresh baseline");
    write(p / "power_now", "0");
    near(f.stats.getPower(), 0, "Valid zero power is retained");
}
static void energy_without_voltage() {
    Fixture f; auto p = f.add();
    fs::remove(p / "voltage_now");
    write(p / "energy_now", "20000000"); write(p / "energy_full", "40000000");
    write(p / "power_now", "10000000");
    f.stats.update();
    near(f.stats.current_watt, 10, "Energy-only power");
    near(f.stats.remaining_time, 2, "Energy-only runtime without voltage");
    near(f.stats.current_percent, 50, "Energy-only percentage");
    fs::remove(p / "power_now");
    near(f.stats.getPower(), 0, "Energy-only estimator baseline");
    advance(3.6); write(p / "energy_now", "19990000");
    near(f.stats.getPower(), 10, "Energy-only delta without voltage");
    near(f.stats.getTimeRemaining(), 1.999, "Energy-only fallback runtime");
}
static void runtime_unit_conversions() {
    Fixture charge; auto c = charge.add();
    write(c / "charge_now", "5000000"); write(c / "power_now", "30000000");
    near(charge.stats.getTimeRemaining(), 2.5, "Power to current conversion has 10^6");
    Fixture energy; auto e = energy.add();
    write(e / "energy_now", "60000000"); write(e / "current_now", "2000000");
    near(energy.stats.getTimeRemaining(), 2, "Energy with current conversion has 10^6");
    fs::remove(c / "voltage_now"); write(c / "current_now", "2000000");
    near(charge.stats.getTimeRemaining(), 2.5, "Charge/current needs no voltage");
}
static void runtime_fallback() {
    Fixture f; auto p = f.add();
    write(p / "current_now", "65000000"); write(p / "charge_now", "5000000");
    f.stats.update(); near(f.stats.remaining_time, 0, "Runtime waits for first estimate");
    advance(3.6); write(p / "charge_now", "4998000");
    f.stats.update();
    near(f.stats.current_watt, 30, "Fallback 30W");
    near(f.stats.remaining_time, 2.499, "Fallback runtime rejects 65A");
}
static void multiple_battery_caches() {
    Fixture f; auto a = f.add("BAT0"); auto b = f.add("BAT1");
    write(a / "current_now", "65000000"); write(b / "current_now", "65000000");
    write(a / "charge_now", "5000000"); write(b / "charge_now", "3000000");
    near(f.stats.getPower(), 0, "Two distinct baselines");
    advance(2); write(a / "charge_now", "4999000"); write(b / "charge_now", "2998000");
    near(f.stats.getPower(), 81, "Separate delta caches sum 27W + 54W");
    near(f.stats.getPower(), 81, "Repeated dual sample unchanged");
    near(f.stats.getTimeRemaining(), 7997000.0f / 5400000.0f, "Dual fallback runtime totals rates");
    check(f.stats.current_now_vec.size() == 1, "One aggregate history entry per update");
}
static void multiple_runtime_units() {
    Fixture f; auto a = f.add("BAT0"); auto b = f.add("BAT1");
    write(a / "charge_now", "4000000"); write(a / "current_now", "2000000");
    write(b / "energy_now", "30000000"); write(b / "power_now", "15000000");
    fs::remove(b / "voltage_now");
    near(f.stats.getTimeRemaining(), 2, "Mixed charge/energy use consistent Wh and W");
    write(a / "voltage_now", "10000000");
    near(f.stats.getTimeRemaining(), 70.0f / 40.0f, "Different pack voltages and total history");
}
static void idle_pack_does_not_mask_active() {
    Fixture f; auto a = f.add("BAT0", "Full"); auto b = f.add("BAT1");
    write(a / "power_now", "20000000"); write(b / "power_now", "30000000");
    write(b / "charge_now", "5000000");
    near(f.stats.getPower(), 30, "Full pack does not hide active pack");
    check(f.stats.current_status == "Discharging", "Active pack determines status");
    near(f.stats.getTimeRemaining(), 2.5, "Full pack does not poison active runtime");
}
static void sequential_discharge_runtime() {
    Fixture f; auto a = f.add("BAT0"); auto b = f.add("BAT1", "Full");
    write(a / "energy_now", "20000000"); write(a / "power_now", "10000000");
    write(b / "energy_now", "20000000"); write(b / "power_now", "0");
    near(f.stats.getTimeRemaining(), 4, "Full reserve pack energy remains available");
    write(b / "status", "Not charging");
    near(f.stats.getTimeRemaining(), 4, "Idle reserve pack energy remains available");
    write(a / "status", "Full");
    near(f.stats.getTimeRemaining(), 0, "Only full packs have no discharge runtime");
}
static void state_transitions() {
    Fixture f; auto p = f.add();
    write(p / "current_now", "65000000"); write(p / "charge_now", "5000000");
    f.stats.update(); advance(1); write(p / "charge_now", "4999000"); f.stats.update();
    near(f.stats.current_watt, 54, "Discharge estimate before switch");
    advance(100); write(p / "status", "Charging"); write(p / "charge_now", "4000000");
    f.stats.update(); near(f.stats.current_watt, 0, "Charging switch resets estimate");
    near(f.stats.remaining_time, 0, "Charging switch resets rate history");
    advance(2); write(p / "charge_now", "4001000");
    near(f.stats.getPower(), 27, "Charging starts independent estimate");
    write(p / "status", "Full"); near(f.stats.getPower(), 0, "Full clears previous estimate");
    write(p / "status", "Discharging"); near(f.stats.getPower(), 0, "Resume after full starts baseline");
}
static void removed_and_replaced_pack() {
    Fixture f; auto p = f.add();
    write(p / "current_now", "65000000"); write(p / "charge_now", "5000000");
    f.stats.getPower(); advance(1); write(p / "charge_now", "4999000");
    near(f.stats.getPower(), 54, "Estimate before removal");
    fs::remove(p / "status"); near(f.stats.getPower(), 0, "Missing status clears stale power");
    write(p / "status", "Discharging"); near(f.stats.getPower(), 0, "Reappearing pack resets baseline");
    advance(1); write(p / "charge_now", "4998000"); near(f.stats.getPower(), 54, "Estimate after reappearance");
    const auto replacement = f.root / "replacement";
    write(replacement / "status", "Discharging"); write(replacement / "voltage_now", "15000000");
    write(replacement / "charge_now", "1000000");
    f.stats.battPath[0] = replacement.string();
    near(f.stats.getPower(), 0, "Path replacement cannot inherit estimate");
}
static void capacity_source_changes() {
    Fixture f; auto p = f.add();
    write(p / "charge_now", "5000000"); write(p / "energy_now", "50000000");
    f.stats.getPower(); advance(1); write(p / "charge_now", "4999000");
    near(f.stats.getPower(), 54, "Charge source estimate");
    fs::remove(p / "charge_now");
    near(f.stats.getPower(), 0, "Charge-to-energy change resets baseline");
    advance(2); write(p / "energy_now", "49990000");
    near(f.stats.getPower(), 18, "New energy source estimates independently");
}
static void capacity_resets_and_no_change() {
    Fixture f; auto p = f.add(); write(p / "charge_now", "5000000");
    near(f.stats.getPower(), 0, "Initial constant capacity");
    advance(100); near(f.stats.getPower(), 0, "No capacity step stays finite zero");
    write(p / "charge_now", "6000000");
    near(f.stats.getPower(), 0, "Opposite capacity direction does not cause spike");
    advance(1); write(p / "charge_now", "1000000");
    near(f.stats.getPower(), 0, "Implausible delta spike is rejected");
    advance(2); write(p / "charge_now", "999000");
    near(f.stats.getPower(), 27, "Recovery after capacity reset");
}
static void invalid_sensor_reads() {
    Fixture f; auto p = f.add(); write(p / "energy_now", "50000000");
    for (const auto& bad : {"garbage", "", "nan", "inf", "1e1000"}) {
        write(p / "power_now", bad); write(p / "current_now", bad); write(p / "voltage_now", bad);
        near(f.stats.getPower(), 0, "Invalid sensor read does not throw or emit NaN");
    }
    advance(2); write(p / "energy_now", "49990000");
    near(f.stats.getPower(), 18, "Invalid direct sensor uses energy delta");
    fs::remove(p / "energy_now"); near(f.stats.getPower(), 0, "Missing capacity does not retain stale estimate");
}
static void no_battery() {
    Fixture f; f.stats.current_watt = 42; f.stats.current_percent = 55; f.stats.remaining_time = 2;
    f.stats.batt_check = false; f.stats.update();
    near(f.stats.current_watt, 0, "No-battery power clears");
    near(f.stats.current_percent, 0, "No-battery percent finite");
    near(f.stats.remaining_time, 0, "No-battery runtime clears");
}
static void discovery_and_limit() {
    Fixture f; const auto root = f.root / "power_supply";
    write(root / "nonstandard-name/type", "Battery");
    write(root / "nonstandard-name/scope", "System");
    write(root / "BAT-mouse/type", "Battery"); write(root / "BAT-mouse/scope", "Device");
    write(root / "BAT-ups/type", "UPS"); write(root / "BAT-mains/type", "Mains");
    fs::create_directory_symlink(root / "nonstandard-name", root / "BAT-alias");
    f.stats.numBattery(); check(f.stats.batt_count == 1, "Type discovery, peripheral filter and deduplication");
    write(root / "BAT1/type", "Battery"); write(root / "BAT2/type", "Battery");
    f.stats.numBattery(); check(f.stats.batt_count == 2, "At most two batteries");
    fs::remove_all(root); f.stats.numBattery();
    check(f.stats.batt_count == 0 && f.stats.battPath[0].empty(), "Rescan clears removed battery slots");
}
static void deckard_semantics() {
    Fixture f; const auto charger = f.root / "deckardcharger";
    const auto p = charger / "BAT0"; const auto power = f.root / "power-monitor/power";
    write(p / "status", "Discharging"); write(charger / "battery_percent", "73");
    write(power / "vph", "12"); write(power / "bl_power", "3");
    f.stats.numBattery(); check(f.stats.batt_count == 1, "Deckard BAT discovery without type");
    f.stats.update(); near(f.stats.current_watt, 15, "Deckard rail plus backlight");
    near(f.stats.current_percent, 73, "Deckard percentage override");
    write(power / "bl_power", "invalid");
    near(f.stats.getPower(), 15, "Deckard invalid monitor read preserves previous power");
    write(p / "status", "Charging");
    near(f.stats.getPower(), 0, "Deckard rail is not presented as charging power");
    write(p / "status", "Full"); near(f.stats.getPower(), 0, "Deckard full remains zero");
    write(p / "status", "Not charging"); write(power / "bl_power", "3");
    near(f.stats.getPower(), 15, "Deckard retains upstream not-charging rail power");
    write(charger / "battery_percent", "120"); near(f.stats.getPercent(), 100, "Deckard percentage bounded");
}
static void percent_bounds_and_mixed_units() {
    Fixture f; auto a = f.add("BAT0"); auto b = f.add("BAT1");
    write(a / "charge_now", "5000000"); write(a / "charge_full", "10000000");
    write(b / "energy_now", "10000000"); write(b / "energy_full", "10000000");
    near(f.stats.getPercent(), 75, "Different capacity units use mean percentage");
    write(a / "charge_full", "0"); write(a / "capacity", "150");
    near(f.stats.getPercent(), 100, "Zero full capacity falls back and clamps");
    write(a / "capacity", "-5"); near(f.stats.getPercent(), 50, "Negative percent clamped");
    fs::remove_all(a); fs::remove_all(b);
    near(f.stats.getPercent(), 0, "Unreadable capacity finite zero");
}
static void runtime_history_window() {
    Fixture f; auto p = f.add(); write(p / "charge_now", "5000000"); write(p / "current_now", "2000000");
    for (int i = 0; i < 30; ++i)
        near(f.stats.getTimeRemaining(), 2.5, "Constant rate window");
    check(f.stats.current_now_vec.size() == 25, "Rate history bounded at 25 aggregate samples");
    write(p / "current_now", "1000000");
    near(f.stats.getTimeRemaining(), 5.0f / 1.96f, "Smoothed runtime uses recent aggregate rates");
}

int main() {
    const std::pair<const char*, std::function<void()>> tests[] = {
        {"healthy current", healthy_current}, {"charging and idle", charging_and_idle},
        {"current limits", current_limits}, {"Claw steps and smoothing", claw_steps_and_smoothing},
        {"charging delta", charging_delta}, {"anomalous power", anomalous_power},
        {"energy without voltage", energy_without_voltage}, {"runtime unit conversions", runtime_unit_conversions},
        {"runtime fallback", runtime_fallback}, {"multiple battery caches", multiple_battery_caches},
        {"multiple runtime units", multiple_runtime_units}, {"idle pack beside active pack", idle_pack_does_not_mask_active},
        {"sequential discharge runtime", sequential_discharge_runtime}, {"state transitions", state_transitions}, {"removed and replaced pack", removed_and_replaced_pack},
        {"capacity source changes", capacity_source_changes}, {"capacity resets and no change", capacity_resets_and_no_change},
        {"invalid sensor reads", invalid_sensor_reads}, {"no battery", no_battery},
        {"discovery and limit", discovery_and_limit}, {"Deckard semantics", deckard_semantics},
        {"percent bounds and mixed units", percent_bounds_and_mixed_units}, {"runtime history window", runtime_history_window},
    };
    for (const auto& test : tests) {
        try { test.second(); std::cout << "PASS " << test.first << '\n'; }
        catch (const std::exception& error) {
            std::cerr << "FAIL " << test.first << ": " << error.what() << '\n';
            return 1;
        }
    }
    std::cout << sizeof(tests) / sizeof(tests[0]) << " cases, " << assertions << " assertions passed\n";
}
