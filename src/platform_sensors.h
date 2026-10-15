#pragma once

#include <cstdint>
#include <string_view>

// Convert platform-specific fan units to RPM. Keep the handheld names shared
// with the former overlay fan reader when using CPUStats' cached reader.
inline int platform_fan_divisor(std::string_view name)
{
    if (name.find("slg4ax46073v") != std::string_view::npos)
        return 2;

    constexpr std::string_view handhelds[] = {
        "steamdeck_hwmon", "asus", "aynec", "ayaneo_ec", "gpdfan",
        "msi_ec", "msi-ec", "msi_wmi_platform", "oxp_ec", "oxpec",
    };
    for (auto driver : handhelds) {
        if (name.find(driver) != std::string_view::npos)
            return 1;
    }
    return 0;
}

// gpu_metrics v3 temperatures are centidegrees; hwmon has already been
// converted to Celsius. SoC is an approximation used only as a last resort.
inline int apu_cpu_temperature_c(uint16_t core, int hwmon, uint16_t soc)
{
    if (core != UINT16_MAX && core > 0)
        return core / 100;
    if (hwmon > 0)
        return hwmon;
    if (soc != UINT16_MAX && soc > 0)
        return soc / 100;
    return 0;
}
