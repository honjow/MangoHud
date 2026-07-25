#include <spdlog/spdlog.h>
#include <cmath>
#include <fstream>
#include <filesystem.h>
#include "battery.h"

namespace fs = ghc::filesystem;
using namespace std;

bool BatteryStats::current_now_usable(float i_ua)
{
    // Consumer packs do not sustain >30A. MSI Claw 8 EX reports a nearly
    // fixed ~65A value that does not track load, so V*I is meaningless.
    float abs_i = std::fabs(i_ua);
    return abs_i > 0.0f && abs_i <= 30e6f;
}

float BatteryStats::estimate_power_from_capacity(
    const std::string& syspath, float v_uv
)
{
    const string charge_now_path = syspath + "/charge_now";
    const string energy_now_path = syspath + "/energy_now";

    float charge_uah = 0.0f;
    float energy_uwh = 0.0f;
    bool have_charge = false;
    bool have_energy = false;

    if (fs::exists(charge_now_path)) {
        std::ifstream input(charge_now_path);
        std::string line;
        if (std::getline(input, line)) {
            charge_uah = stof(line);
            have_charge = true;
        }
    } else if (fs::exists(energy_now_path)) {
        std::ifstream input(energy_now_path);
        std::string line;
        if (std::getline(input, line)) {
            energy_uwh = stof(line);
            have_energy = true;
        }
    } else {
        return last_estimated_power_w;
    }

    auto now = std::chrono::steady_clock::now();
    if (!capacity_sample_valid) {
        last_charge_uah = charge_uah;
        last_energy_uwh = energy_uwh;
        last_capacity_sample_time = now;
        capacity_sample_valid = true;
        SPDLOG_INFO(
            "Battery current_now unusable; estimating power from charge/energy delta"
        );
        return last_estimated_power_w;
    }

    double dt = std::chrono::duration<double>(now - last_capacity_sample_time).count();
    if (dt < 1.0)
        return last_estimated_power_w;

    float power_w = 0.0f;
    bool updated = false;

    if (have_charge) {
        float delta_uah = last_charge_uah - charge_uah;
        // charge_now is coarse; wait until it actually moves.
        if (std::fabs(delta_uah) >= 1.0f && std::fabs(v_uv) > 0.0f) {
            // µAh over seconds → W at current voltage:
            // (|ΔµAh| / 1e6 / (dt/3600)) * (µV/1e6)
            power_w = std::fabs(delta_uah) * std::fabs(v_uv) * 3600.0f
                      / (1e12f * static_cast<float>(dt));
            updated = true;
            last_charge_uah = charge_uah;
            last_capacity_sample_time = now;
        }
    } else if (have_energy) {
        float delta_uwh = last_energy_uwh - energy_uwh;
        if (std::fabs(delta_uwh) >= 1.0f) {
            // µWh over seconds → W
            power_w = std::fabs(delta_uwh) * 3600.0f
                      / (1e6f * static_cast<float>(dt));
            updated = true;
            last_energy_uwh = energy_uwh;
            last_capacity_sample_time = now;
        }
    }

    if (updated) {
        if (last_estimated_power_w <= 0.0f)
            last_estimated_power_w = power_w;
        else
            last_estimated_power_w = last_estimated_power_w * 0.7f + power_w * 0.3f;
    }

    return last_estimated_power_w;
}

void BatteryStats::numBattery() {
    int batteryCount = 0;
    if (!fs::exists("/sys/class/power_supply/")) {
         batteryCount = 0;
    }
    fs::path path("/sys/class/power_supply/");
    for (auto& p : fs::directory_iterator(path)) {
        string fileName = p.path().filename();
        if (fileName.find("BAT") != std::string::npos) {
            battPath[batteryCount] = p.path();
            batteryCount += 1;
        }
    }
    batt_count = batteryCount;
    batt_check = true;
}

void BatteryStats::update() {
    if (!batt_check) {
        numBattery();
        if (batt_count == 0) {
            SPDLOG_ERROR("No battery found");
        }
    }

     if (batt_count > 0) {
        current_watt = getPower();
        current_percent = getPercent();
        remaining_time = getTimeRemaining();
    }
}

float BatteryStats::getPercent()
{
    float charge_n = 0;
    float charge_f = 0;
    for(int i = 0; i < batt_count; i++) {
        string syspath = battPath[i];
        string charge_now = syspath + "/charge_now";
        string charge_full = syspath + "/charge_full";
        string energy_now = syspath + "/energy_now";
        string energy_full = syspath + "/energy_full";
        string capacity = syspath + "/capacity";

        if (fs::exists(charge_now)) {
            std::ifstream input(charge_now);
            std::string line;
            if(std::getline(input, line)) {
                charge_n += (stof(line) / 1000000);
            }
            std::ifstream input2(charge_full);
            if(std::getline(input2, line)) {
                charge_f += (stof(line) / 1000000);
            }
        }

        else if (fs::exists(energy_now)) {
            std::ifstream input(energy_now);
            std::string line;
            if(std::getline(input, line)) {
                charge_n += (stof(line) / 1000000);
            }
            std::ifstream input2(energy_full);
            if(std::getline(input2, line)) {
                charge_f += (stof(line) / 1000000);
            }
        }

        else {
            // using /sys/class/power_supply/BAT*/capacity
            // No way to get an accurate reading just average the percents if mutiple batteries
            std::ifstream input(capacity);
            std::string line;
            if(std::getline(input, line)) {
                charge_n += stof(line) / 100;
                charge_f = batt_count;
            }
        }
    }
    return (charge_n / charge_f) * 100;
}

float BatteryStats::getPower() {
    float power_w = 0.0f;

    for (int i = 0; i < batt_count; i++) {
        string syspath = battPath[i];
        string current_now = syspath + "/current_now";
        string voltage_now = syspath + "/voltage_now";
        string power_now = syspath + "/power_now";
        string status = syspath + "/status";

        {
            std::ifstream input(status);
            std::string line;
            if (std::getline(input, line)) {
                current_status = line;
                state[i] = current_status;
            }
        }

        if (state[i] != "Charging" && state[i] != "Discharging") {
            // TODO if we have multiple batteries, we will return 0 if just one of them is charging
            return 0.0f;
        }

        float v_uv = 0.0f;
        if (fs::exists(voltage_now)) {
            std::ifstream input(voltage_now);
            std::string line;
            if (std::getline(input, line))
                v_uv = stof(line);
        }

        // Prefer power_now (µW) when available.
        if (fs::exists(power_now)) {
            std::ifstream input(power_now);
            std::string line;
            if (std::getline(input, line)) {
                float p = std::fabs(stof(line)) / 1000000.0f;
                // Same class of firmware bug can poison power_now.
                if (p <= 200.0f)
                    power_w += p;
                else
                    power_w += estimate_power_from_capacity(syspath, v_uv);
            }
            continue;
        }

        float i_ua = 0.0f;

        if (fs::exists(current_now)) {
            std::ifstream input(current_now);
            std::string line;
            if (std::getline(input, line))
                i_ua = stof(line);
        }

        if (current_now_usable(i_ua) && std::fabs(v_uv) > 0.0f) {
            power_w += (std::fabs(i_ua) * std::fabs(v_uv)) * 1e-12f;
        } else {
            power_w += estimate_power_from_capacity(syspath, v_uv);
        }
    }

    return power_w;
}

float BatteryStats::getTimeRemaining() {
    float current = 0.0f;
    float charge = 0.0f;

    for (int i = 0; i < batt_count; i++) {
        string syspath = battPath[i];
        string current_now = syspath + "/current_now";
        string charge_now = syspath + "/charge_now";
        string energy_now = syspath + "/energy_now";
        string voltage_now = syspath + "/voltage_now";
        string power_now = syspath + "/power_now";

        float v_uv = 0.0f;
        if (fs::exists(voltage_now)) {
            std::ifstream input_voltage(voltage_now);
            std::string line;
            if (std::getline(input_voltage, line))
                v_uv = stof(line);
        }

        bool used_current = false;
        if (fs::exists(current_now)) {
            std::ifstream input(current_now);
            std::string line;
            float i_ua = 0.0f;
            if (std::getline(input, line))
                i_ua = stof(line);
            if (current_now_usable(i_ua)) {
                current_now_vec.push_back(std::fabs(i_ua));
                used_current = true;
            }
        }

        if (!used_current) {
            if (fs::exists(power_now) && std::fabs(v_uv) > 0.0f) {
                std::ifstream input_power(power_now);
                std::string line;
                float power = 0.0f;
                if (std::getline(input_power, line))
                    power = std::fabs(stof(line));
                if (power > 0.0f && power / 1e6f <= 200.0f) {
                    // (µW / µV) = µA
                    current_now_vec.push_back(power / v_uv);
                    used_current = true;
                }
            }
        }

        if (!used_current) {
            // Derive µA from capacity-delta power estimate.
            float power_w = estimate_power_from_capacity(syspath, v_uv);
            if (power_w > 0.0f && std::fabs(v_uv) > 0.0f)
                current_now_vec.push_back(power_w * 1e12f / std::fabs(v_uv));
        }

        if (fs::exists(charge_now)) {
            std::ifstream input(charge_now);
            std::string line;
            if (std::getline(input, line)) {
                charge += stof(line);
            }
        } else if (fs::exists(energy_now) && std::fabs(v_uv) > 0.0f) {
            float energy = 0.0f;

            {
                std::ifstream input_energy(energy_now);
                std::string line;
                if (std::getline(input_energy, line)) {
                    energy = stof(line);
                }
            }

            // (µWh / µV) = µAh
            charge += energy / v_uv;
        }

        if (current_now_vec.size() > 25) {
            current_now_vec.erase(current_now_vec.begin());
        }
    }

    if (current_now_vec.empty()) {
        return 0.0f;
    }

    for (const auto& current_now_sample : current_now_vec) {
        current += current_now_sample;
    }
    current /= static_cast<float>(current_now_vec.size());

    if (current <= 0.0f) {
        return 0.0f;
    }

    return charge / current;
}

BatteryStats Battery_Stats;
