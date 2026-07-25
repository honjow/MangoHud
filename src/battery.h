#pragma once
#include <chrono>
#include <string>
#include <vector>

class BatteryStats{
    public:
        void numBattery();
        void update();
        float getPower();
        float getPercent();
        float getTimeRemaining();
        std::string battPath[2];
        float current_watt = 0;
        float current_percent = 0;
        float remaining_time = 0;
        std::string current_status;
        std::string state [2];
        int batt_count=0;
        bool batt_check = false;
        std::vector<float> current_now_vec = {};

    private:
        // MSI Claw (and similar) firmware can report a stuck/bogus current_now
        // (~65A). Fall back to charge/energy delta estimation in that case.
        bool capacity_sample_valid = false;
        float last_charge_uah = 0.0f;
        float last_energy_uwh = 0.0f;
        float last_estimated_power_w = 0.0f;
        std::chrono::steady_clock::time_point last_capacity_sample_time{};

        static bool current_now_usable(float i_ua);
        float estimate_power_from_capacity(const std::string& syspath, float v_uv);
};

extern BatteryStats Battery_Stats;
