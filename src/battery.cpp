#include <algorithm>
#include <vector>
#include <spdlog/spdlog.h>
#include <cmath>
#include <filesystem.h>
#include "battery.h"
#include "file_utils.h"
#include "string_utils.h"

namespace fs = ghc::filesystem;
using namespace std;

static bool read_value(const string& path, float& value) {
    const string line = read_line(path);
    float parsed = 0.0f;
    value = 0.0f;
    if (line.empty() || !try_stof(parsed, line) || !std::isfinite(parsed))
        return false;
    value = parsed;
    return true;
}

bool BatteryStats::current_now_usable(float i_ua)
{
    // MSI Claw 8 EX can report a stuck ~65A, unrelated to the actual load.
    return std::isfinite(i_ua) && std::fabs(i_ua) > 0.0f && std::fabs(i_ua) <= 30e6f;
}

void BatteryStats::refresh_state(int index)
{
    const string status = read_line(battPath[index] + "/status");
    if (sample_paths[index] != battPath[index] || state[index] != status) {
        capacity_samples[index] = {};
        current_now_vec.clear();
        sample_paths[index] = battPath[index];
    }
    state[index] = status;
}

float BatteryStats::estimate_power_from_capacity(int index, float v_uv)
{
    auto& sample = capacity_samples[index];
    const string& syspath = battPath[index];
    float capacity = 0.0f;
    const bool have_charge = v_uv > 0.0f &&
        read_value(syspath + "/charge_now", capacity) && capacity >= 0.0f;
    if (!have_charge &&
        (!read_value(syspath + "/energy_now", capacity) || capacity < 0.0f)) {
        sample = {};
        return 0.0f;
    }

    const auto now = sample_clock();
    if (!sample.valid || sample.uses_charge != have_charge) {
        sample = {true, have_charge, capacity, 0.0f, now};
        SPDLOG_INFO("Battery current/power unusable; estimating power from charge/energy delta");
        return 0.0f;
    }

    const double dt = std::chrono::duration<double>(now - sample.time).count();
    if (dt < 1.0)
        return sample.power_w;

    const float delta = sample.capacity - capacity;
    // Coarse capacity readings need a full step, including the preceding idle time.
    if (std::fabs(delta) < 1.0f)
        return sample.power_w;

    // Do not turn a capacity reset or a charge/discharge reversal into a spike.
    const bool wrong_direction = (state[index] == "Discharging" && delta < 0.0f) ||
                                 (state[index] == "Charging" && delta > 0.0f);
    const float power_w = std::fabs(delta) * 3600.0f / (1e6f * dt) *
                          (have_charge ? v_uv / 1e6f : 1.0f);
    sample.capacity = capacity;
    sample.time = now;
    if (wrong_direction || !std::isfinite(power_w) || power_w > 200.0f) {
        sample.power_w = 0.0f;
        return 0.0f;
    }
    sample.power_w = sample.power_w <= 0.0f ? power_w :
                     sample.power_w * 0.7f + power_w * 0.3f;
    return sample.power_w;
}

// A peripheral's battery (a gamepad, a mouse) has the scope "Device"
static bool is_system_battery(const fs::path& path) {
    return read_line((path / "type").string()) == "Battery" &&
           read_line((path / "scope").string()) != "Device";
}

void BatteryStats::numBattery() {
    int batteryCount = 0;
    std::vector<fs::path> found;
    for (int i = 0; i < battery_count_max; ++i) {
        battPath[i].clear();
        state[i].clear();
        sample_paths[i].clear();
        capacity_samples[i] = {};
    }
    current_now_vec.clear();

    for (const auto& path : { fs::path(power_supply_path), fs::path(deckard_charger_path) }) {
        if (!fs::exists(path))
            continue;

        const bool is_deckard = path == fs::path(deckard_charger_path);
        for (auto& p : fs::directory_iterator(path)) {
            string fileName = p.path().filename();
            if (batteryCount < battery_count_max &&
                (is_deckard ? fileName.find("BAT") != std::string::npos : is_system_battery(p.path()))) {
                // Skip duplicate battery symlinks.
                std::error_code ec;
                fs::path real = fs::canonical(p.path(), ec);
                if (!ec && std::find(found.begin(), found.end(), real) != found.end())
                    continue;
                if (!ec)
                    found.push_back(real);
                battPath[batteryCount++] = p.path();
            }
        }
    }

    batt_count = batteryCount;
    batt_check = true;
}

void BatteryStats::update() {
    if (!batt_check) {
        numBattery();
        if (batt_count == 0)
            SPDLOG_ERROR("No battery found");
    }

    current_watt = getPower();
    current_percent = getPercent();
    remaining_time = getTimeRemaining();
}

float BatteryStats::getPercent()
{
    float percent = 0.0f;
    if (read_value((fs::path(deckard_charger_path) / "battery_percent").string(), percent))
        return std::clamp(percent, 0.0f, 100.0f);

    float capacity_sum = 0.0f, full_sum = 0.0f, percent_sum = 0.0f;
    int count = 0, first_kind = -1;
    bool mixed = false;
    for (int i = 0; i < batt_count; ++i) {
        const string& syspath = battPath[i];
        float capacity = 0.0f, full = 0.0f;
        int kind = 0;
        if (read_value(syspath + "/charge_now", capacity) &&
            read_value(syspath + "/charge_full", full) && capacity >= 0.0f && full > 0.0f) {
            kind = 0;
        } else if (read_value(syspath + "/energy_now", capacity) &&
                   read_value(syspath + "/energy_full", full) && capacity >= 0.0f && full > 0.0f) {
            kind = 1;
        } else if (read_value(syspath + "/capacity", capacity)) {
            kind = 2;
            full = 100.0f;
        } else {
            continue;
        }
        capacity = std::clamp(capacity, 0.0f, full);
        percent_sum += capacity / full * 100.0f;
        capacity_sum += capacity;
        full_sum += full;
        if (first_kind < 0)
            first_kind = kind;
        mixed |= first_kind != kind;
        ++count;
    }
    // Unlike charge/energy units cannot be added. Fall back to mean percentages.
    return count == 0 ? 0.0f : mixed ? percent_sum / count : capacity_sum / full_sum * 100.0f;
}

// vph is the measured system rail, bl_power the backlight.
static bool get_power_deckard(const string& path, float& power) {
    float vph_w = 0.0f, bl_power_w = 0.0f;
    if (!read_value((fs::path(path) / "vph").string(), vph_w) ||
        !read_value((fs::path(path) / "bl_power").string(), bl_power_w))
        return false;
    power = vph_w + bl_power_w;
    return true;
}

float BatteryStats::battery_power(int index, float v_uv)
{
    const string& syspath = battPath[index];
    float value = 0.0f;
    if (fs::exists(syspath + "/power_now")) {
        if (read_value(syspath + "/power_now", value) && std::fabs(value) <= 200e6f) {
            capacity_samples[index] = {};
            return std::fabs(value) / 1e6f;
        }
    } else if (read_value(syspath + "/current_now", value) && current_now_usable(value) && v_uv > 0.0f) {
        capacity_samples[index] = {};
        return std::fabs(value) * v_uv * 1e-12f;
    }
    return estimate_power_from_capacity(index, v_uv);
}

float BatteryStats::getPower() {
    float power_w = 0.0f;
    const bool deckard = fs::exists(fs::path(deckard_power_path) / "vph");
    current_status.clear();
    bool deckard_active = batt_count > 0;
    for (int i = 0; i < batt_count; ++i) {
        refresh_state(i);
        // Steam Frame's rail power is system consumption, not charging power.
        if (deckard) {
            deckard_active &= !state[i].empty() && state[i] != "Charging" &&
                              state[i] != "Unknown" && state[i] != "Full";
            current_status = state[i];
            continue;
        }
        if (state[i] != "Charging" && state[i] != "Discharging")
            continue;
        if (current_status.empty())
            current_status = state[i];
        else if (current_status != state[i])
            current_status = "Unknown";

        float v_uv = 0.0f;
        read_value(battPath[i] + "/voltage_now", v_uv);
        power_w += battery_power(i, v_uv);
    }
    // Keep previous rail power on empty or invalid reads, as upstream does.
    if (deckard)
        return !deckard_active ? 0.0f : get_power_deckard(deckard_power_path, power_w) ? power_w : current_watt;
    return power_w;
}

float BatteryStats::getTimeRemaining() {
    float capacity[battery_count_max] = {}, rate[battery_count_max] = {};
    float voltage[battery_count_max] = {};
    bool energy_basis[battery_count_max] = {};
    bool any_energy = false, all_convertible = true;
    int count = 0;
    for (int i = 0; i < batt_count; ++i) {
        refresh_state(i);
        const bool active = state[i] == "Charging" || state[i] == "Discharging";
        if (!active && state[i] != "Full" && state[i] != "Not charging")
            continue;
        const string& syspath = battPath[i];
        float charge = 0.0f, energy = 0.0f, current = 0.0f, v_uv = 0.0f;
        const bool have_charge = read_value(syspath + "/charge_now", charge) && charge >= 0.0f;
        const bool have_energy = read_value(syspath + "/energy_now", energy) && energy >= 0.0f;
        read_value(syspath + "/voltage_now", v_uv);
        if (!active) {
            // Sequential-discharge systems still have the idle pack's energy available.
            if (!have_charge && !have_energy)
                continue;
            energy_basis[count] = !have_charge;
            capacity[count] = have_charge ? charge : energy;
        } else if (have_charge && read_value(syspath + "/current_now", current) && current_now_usable(current)) {
            capacity[count] = charge;
            rate[count] = std::fabs(current);
        } else {
            const float power_w = battery_power(i, v_uv);
            if (have_energy && power_w > 0.0f) {
                capacity[count] = energy;
                rate[count] = power_w * 1e6f;
                energy_basis[count] = true;
            } else if (have_charge && v_uv > 0.0f && power_w > 0.0f) {
                capacity[count] = charge;
                // W / µV -> µA. µW / µV alone would produce amps, not µA.
                rate[count] = power_w * 1e12f / v_uv;
            } else {
                current_now_vec.clear();
                return 0.0f;
            }
        }
        voltage[count] = v_uv;
        any_energy |= energy_basis[count];
        all_convertible &= energy_basis[count] || v_uv > 0.0f;
        ++count;
    }

    if (count == 0 || (any_energy && !all_convertible)) {
        current_now_vec.clear();
        return 0.0f;
    }
    const bool use_energy = all_convertible;
    float total_capacity = 0.0f, total_rate = 0.0f;
    for (int i = 0; i < count; ++i) {
        // µAh * µV / 10^6 = µWh. Normalize both numerator and denominator.
        const float scale = use_energy && !energy_basis[i] ? voltage[i] / 1e6f : 1.0f;
        total_capacity += capacity[i] * scale;
        total_rate += rate[i] * scale;
    }
    if (remaining_uses_energy != use_energy) {
        current_now_vec.clear();
        remaining_uses_energy = use_energy;
    }
    if (total_rate <= 0.0f) {
        current_now_vec.clear();
        return 0.0f;
    }
    // One total sample per update, not one sample per battery.
    current_now_vec.push_back(total_rate);
    if (current_now_vec.size() > 25)
        current_now_vec.erase(current_now_vec.begin());
    float average_rate = 0.0f;
    for (const float sample : current_now_vec)
        average_rate += sample;
    average_rate /= current_now_vec.size();
    return total_capacity / average_rate;
}

BatteryStats Battery_Stats;
