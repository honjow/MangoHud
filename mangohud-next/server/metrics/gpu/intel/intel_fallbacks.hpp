#pragma once

#include <algorithm>
#include <charconv>
#include <cmath>
#include <filesystem>
#include <fstream>
#include <set>
#include <string>
#include <vector>

#include "../../fdinfo.hpp"

// Shared by the next server and the legacy wrapper's Intel backends. Paths and
// time arguments are injectable so the actual proc/sysfs readers can be tested
// without a particular Intel GPU or privileged access.
class IntelFallbacks {
public:
    IntelFallbacks(const std::string& driver, const std::string& pci_dev,
                   const std::filesystem::path& proc_root = "/proc",
                   const std::filesystem::path& powercap_root = "/sys/class/powercap",
                   const std::filesystem::path& hwmon_root = "/sys/class/hwmon")
        : device_memory(driver, pci_dev, driver == "i915"
              ? std::initializer_list<const char*>{"drm-resident-local0", "drm-resident-system0"}
              : std::initializer_list<const char*>{"drm-resident-vram0", "drm-resident-gtt"},
              proc_root.string()), powercap_root(powercap_root), hwmon_root(hwmon_root) {}

    void setup(bool need_power, bool need_power_limit, bool need_temperature) {
        if (need_power || need_power_limit)
            find_rapl(need_power, need_power_limit);
        if (need_temperature)
            find_package_temperature();
    }

    float get_power_usage(chrono_timer now = std::chrono::steady_clock::now()) {
        uint64_t energy = 0;
        if (!read_number(rapl_energy, energy)) {
            have_energy_sample = false;
            return 0.f;
        }
        if (!have_energy_sample) {
            last_energy = energy;
            last_energy_time = now;
            have_energy_sample = true;
            return 0.f;
        }
        uint64_t delta = 0;
        if (energy >= last_energy)
            delta = energy - last_energy;
        else if (max_energy_range && last_energy <= max_energy_range && energy <= max_energy_range)
            delta = max_energy_range - last_energy + energy;
        const auto elapsed = std::chrono::duration_cast<std::chrono::nanoseconds>(now - last_energy_time).count();
        last_energy = energy;
        last_energy_time = now;
        // RAPL reports microjoules. Never substitute CPU package energy.
        return elapsed > 0 ? static_cast<double>(delta) * 1000.0 / elapsed : 0.f;
    }

    float get_power_limit() {
        uint64_t value = 0;
        return read_number(rapl_power_limit, value) ? value / 1'000'000.f : 0.f;
    }

    int get_temperature() {
        uint64_t value = 0;
        return read_number(package_temperature, value) ? std::lround(value / 1000.0) : 0;
    }

    float get_device_memory_used(chrono_timer now = std::chrono::steady_clock::now()) {
        return device_memory.get_memory_used(now);
    }

private:
    FDInfoDeviceMemory device_memory;
    const std::filesystem::path powercap_root, hwmon_root;
    std::ifstream rapl_energy, rapl_power_limit, package_temperature;
    uint64_t max_energy_range = 0, last_energy = 0;
    chrono_timer last_energy_time{};
    bool have_energy_sample = false;

    static std::vector<std::filesystem::path> entries(const std::filesystem::path& root) {
        std::vector<std::filesystem::path> paths;
        std::error_code ec;
        std::filesystem::directory_iterator it(root,
            std::filesystem::directory_options::skip_permission_denied, ec), end;
        for (; !ec && it != end; it.increment(ec))
            paths.push_back(it->path());
        return paths;
    }

    static std::string read_line(const std::filesystem::path& path) {
        std::ifstream stream(path);
        std::string line;
        std::getline(stream, line);
        return line;
    }

    static bool read_number(std::ifstream& stream, uint64_t& value) {
        if (!stream.is_open())
            return false;
        stream.clear();
        stream.seekg(0);
        std::string text;
        if (!std::getline(stream, text))
            return false;
        const auto parsed = std::from_chars(text.data(), text.data() + text.size(), value);
        return parsed.ec == std::errc() && parsed.ptr == text.data() + text.size();
    }

    void find_rapl(bool need_power, bool need_power_limit) {
        for (const auto& path : entries(powercap_root)) {
            const auto name = path.filename().string();
            // Only an explicitly named GPU/uncore subdomain is eligible.
            if (name.rfind("intel-rapl:", 0) != 0 ||
                std::count(name.begin(), name.end(), ':') < 2 ||
                read_line(path / "name") != "uncore")
                continue;
            if (need_power && !rapl_energy.is_open()) {
                rapl_energy.clear();
                rapl_energy.open(path / "energy_uj");
                std::ifstream range(path / "max_energy_range_uj");
                read_number(range, max_energy_range);
            }
            if (need_power_limit && !rapl_power_limit.is_open()) {
                rapl_power_limit.clear();
                rapl_power_limit.open(path / "constraint_0_power_limit_uw");
            }
            if ((!need_power || rapl_energy.is_open()) &&
                (!need_power_limit || rapl_power_limit.is_open()))
                return;
        }
    }

    void find_package_temperature() {
        for (const auto& path : entries(hwmon_root)) {
            if (read_line(path / "name") != "coretemp")
                continue;
            for (const auto& label : entries(path)) {
                const auto name = label.filename().string();
                if (name.rfind("temp", 0) != 0 || name.size() <= 10 ||
                    name.compare(name.size() - 6, 6, "_label") != 0 ||
                    read_line(label) != "Package id 0")
                    continue;
                const auto input = path / (name.substr(0, name.size() - 6) + "_input");
                package_temperature.clear();
                package_temperature.open(input);
                if (package_temperature.is_open())
                    return;
            }
        }
    }
};
