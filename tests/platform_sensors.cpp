#include "../src/platform_sensors.h"
#include <iostream>
#include <stdexcept>

static unsigned checks = 0;
static void expect(int actual, int wanted)
{
    ++checks;
    if (actual != wanted)
        throw std::runtime_error("platform sensor regression at check " + std::to_string(checks));
}

int main()
{
    for (auto driver : {"steamdeck_hwmon", "asus", "asus_custom", "aynec",
                        "ayaneo_ec", "gpdfan", "msi_ec", "msi-ec",
                        "msi_wmi_platform", "oxp_ec", "oxpec"})
        expect(platform_fan_divisor(driver), 1);
    expect(platform_fan_divisor("slg4ax46073v"), 2);
    expect(platform_fan_divisor("amdgpu"), 0);
    expect(platform_fan_divisor(""), 0);
    expect(apu_cpu_temperature_c(6500, 70, 7500), 65);
    expect(apu_cpu_temperature_c(0, 70, 7500), 70);
    expect(apu_cpu_temperature_c(UINT16_MAX, 70, 7500), 70);
    expect(apu_cpu_temperature_c(0, 0, 7500), 75);
    expect(apu_cpu_temperature_c(0, -1, 7500), 75);
    expect(apu_cpu_temperature_c(0, 0, 0), 0);
    expect(apu_cpu_temperature_c(0, 0, UINT16_MAX), 0);
    expect(apu_cpu_temperature_c(6550, 70, 7500), 65);
    std::cout << checks << " platform fan/AMD temperature checks passed\n";
}
