#pragma once
#include <chrono>
#include <string>
#include <vector>

static constexpr int battery_count_max = 2;

class BatteryStats{
    public:
        void numBattery();
        void update();
        float getPower();
        float getPercent();
        float getTimeRemaining();
        std::string battPath[battery_count_max];
        float current_watt = 0;
        float current_percent = 0;
        float remaining_time = 0;
        std::string current_status;
        std::string state[battery_count_max];
        int batt_count=0;
        bool batt_check = false;
        // Aggregate rate history: microamps, or microwatts for energy-based packs.
        std::vector<float> current_now_vec = {};

    private:
        using Clock = std::chrono::steady_clock;
        struct CapacitySample {
            bool valid = false;
            bool uses_charge = false;
            float capacity = 0.0f;
            float power_w = 0.0f;
            Clock::time_point time{};
        };
        // A sample belongs to one battery and one charge/discharge session.
        CapacitySample capacity_samples[battery_count_max];
        std::string sample_paths[battery_count_max];
        bool remaining_uses_energy = false;

        // Private injection points for deterministic sysfs fixtures; no user setting.
        friend struct BatteryTestAccess;
        std::string power_supply_path = "/sys/class/power_supply/";
        std::string deckard_charger_path = "/run/deckardcharger/";
        std::string deckard_power_path = "/run/power-monitor/power/";
        Clock::time_point (*sample_clock)() = Clock::now;

        static bool current_now_usable(float i_ua);
        void refresh_state(int index);
        float battery_power(int index, float v_uv);
        float estimate_power_from_capacity(int index, float v_uv);
};

extern BatteryStats Battery_Stats;
