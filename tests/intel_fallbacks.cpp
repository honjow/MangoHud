// Hardware-independent integration fixtures for the production Intel fallback
// readers, HwmonBase and FDInfoBase/FDInfoWrapper. No root or GPU is required.
#include <cmath>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <sstream>
#include <stdexcept>
#include <unistd.h>

#include "mangohud-next/server/metrics/gpu/intel/intel_fallbacks.hpp"
#include "mangohud-next/server/metrics/hwmon.hpp"

namespace fs = std::filesystem;
using namespace std::chrono_literals;

static size_t checks = 0;
static void check(bool value, const char* message) {
    ++checks;
    if (!value)
        throw std::runtime_error(message);
}
static bool near(double actual, double expected) {
    return std::abs(actual - expected) < 0.00001;
}
static void write(const fs::path& path, const std::string& value) {
    fs::create_directories(path.parent_path());
    std::ofstream stream(path);
    stream << value;
    if (!stream)
        throw std::runtime_error("failed to write fixture " + path.string());
}
struct Fixture {
    fs::path root;
    Fixture() {
        std::string name = (fs::temp_directory_path() / "mangohud-intel-XXXXXX").string();
        auto* path = mkdtemp(name.data());
        if (!path)
            throw std::runtime_error("mkdtemp failed");
        root = path;
    }
    ~Fixture() { std::error_code ec; fs::remove_all(root, ec); }
    fs::path proc() const { return root / "proc"; }
    fs::path powercap() const { return root / "powercap"; }
    fs::path hwmon() const { return root / "hwmon"; }
    IntelFallbacks intel(const std::string& driver = "xe") {
        return IntelFallbacks(driver, "0000:00:02.0", proc(), powercap(), hwmon());
    }
};
static std::string fd(const std::string& driver, const std::string& client,
                      const std::string& memory, const std::string& pci = "0000:00:02.0") {
    return "drm-driver:\t" + driver + "\ndrm-pdev:\t" + pci +
           "\ndrm-client-id:\t" + client + "\n" + memory;
}
static void add_fd(Fixture& f, int pid, int number, const std::string& data,
                   const std::string& node = "renderD128") {
    const auto base = f.proc() / std::to_string(pid);
    fs::create_directories(base / "fd");
    fs::create_symlink("/dev/dri/" + node, base / "fd" / std::to_string(number));
    write(base / "fdinfo" / std::to_string(number), data);
}

static void test_parser() {
    std::istringstream input("\nno colon\n ignored: 42\ndrm-client-id:\t17\n"
                             "drm-resident-gtt:   256 MiB\r\nempty:\t\n");
    const auto parsed = read_fdinfo(input);
    check(parsed.at("drm-client-id") == "17", "parse tab-separated client ID");
    check(parsed.at("drm-resident-gtt") == "256 MiB", "trim whitespace and CR");
    check(parsed.size() == 2, "skip malformed and indented records");
    check(fdinfo_memory_bytes("1024 KiB") == 1048576, "KiB conversion");
    check(fdinfo_memory_bytes("2 MiB") == 2097152, "MiB conversion");
    check(fdinfo_memory_bytes("3 GiB") == 3221225472ULL, "GiB conversion");
    check(fdinfo_memory_bytes("1024") == 1024, "unitless bytes conversion");
    check(fdinfo_memory_bytes("1024 B") == 1024, "bytes conversion");
    check(fdinfo_memory_bytes("") == 0, "empty memory safe");
    check(fdinfo_memory_bytes("-12 KiB") == 0, "negative memory safe");
    check(fdinfo_memory_bytes("oops KiB") == 0, "invalid memory safe");
    check(fdinfo_memory_bytes("12 banana") == 0, "invalid unit safe");
    check(fdinfo_memory_bytes("18446744073709551615 GiB") == 0, "overflow safe");
    check(fdinfo_memory_bytes({{"vram", "0 KiB"}, {"gtt", "1 GiB"}}, {"vram", "gtt"}) == 0,
          "present zero-valued dedicated memory takes precedence");
}

static void test_process_discovery() {
    Fixture f;
    const auto sys = f.root / "drm";
    fs::create_directories(sys / "renderD128/device/drm/card0");
    FDInfoWrapper wrapper("renderD128");
    wrapper.pids.try_emplace(42, "renderD128", 42, f.proc().string(), sys.string());
    auto& reader = wrapper.pids.at(42);
    const auto start = std::chrono::steady_clock::now();
    reader.poll(start);
    check(reader.fds_data.empty(), "missing proc PID harmless");
    add_fd(f, 42, 3, fd("xe", "1", "drm-resident-gtt: 256 MiB\n"));
    reader.poll(start + 1ms);
    check(reader.fds_data.size() == 1, "empty fd list discovers immediately");
    check(near(wrapper.get_memory_used(42, {"drm-resident-vram0", "drm-resident-gtt"}), .25),
          "late xe shared-memory key reaches proc_vram");

    add_fd(f, 42, 4, fd("xe", "1", "drm-resident-gtt: 256 MiB\n"));
    add_fd(f, 42, 5, fd("xe", "2", "drm-resident-vram0: 512 MiB\n"), "card0");
    add_fd(f, 42, 6, fd("xe", "3", "drm-resident-gtt: 8 GiB\n"), "renderD129");
    reader.poll(start + 100ms);
    check(reader.fds_data.size() == 1, "nonempty fd list obeys two-second throttle");
    reader.poll(start + 2100ms);
    check(reader.fds_data.size() == 2, "two-second discovery includes card and deduplicates clients");
    check(near(wrapper.get_memory_used(42, {"drm-resident-vram0", "drm-resident-gtt"}), .75),
          "sum unique clients on target DRM node");

    write(f.proc() / "42/fdinfo/3", fd("xe", "1", "drm-resident-vram0: 128 MiB\n"));
    reader.poll(start + 2200ms);
    check(near(wrapper.get_memory_used(42, {"drm-resident-vram0", "drm-resident-gtt"}), .625),
          "runtime memory selection updates on the retained fd stream");
    check(reader.fds_data.front().count("drm-resident-gtt") == 0,
          "removed fdinfo fields are not retained in old snapshots");

    write(f.proc() / "42/fdinfo/3", fd("i915", "1", "drm-resident-system0: 384 MiB\n"));
    reader.poll(start + 2300ms);
    check(near(wrapper.get_memory_used(42, {"drm-resident-local0", "drm-resident-system0", "drm-total-local0"}), .375),
          "i915 resident system memory reaches proc_vram");
    write(f.proc() / "42/fdinfo/3", fd("i915", "1", "drm-resident-local0: 0 KiB\ndrm-resident-system0: 2 GiB\n"));
    reader.poll(start + 2400ms);
    check(near(wrapper.get_memory_used(42, {"drm-resident-local0", "drm-resident-system0"}), 0),
          "i915 dedicated zero is preserved");

    fs::remove(f.proc() / "42/fd/3");
    fs::remove(f.proc() / "42/fd/4");
    fs::remove(f.proc() / "42/fd/5");
    reader.poll(start + 4200ms);
    check(reader.fds_data.empty(), "closed descriptors are removed at rediscovery");
    add_fd(f, 42, 7, fd("xe", "1", "drm-resident-gtt: 1 GiB\n"));
    reader.poll(start + 4201ms);
    check(reader.fds_data.size() == 1, "client IDs can be reused after descriptors close");

    if (geteuid() != 0) {
        fs::permissions(f.proc() / "42/fd", fs::perms::none);
        reader.init(start + 4300ms);
        reader.poll(start + 4301ms);
        check(reader.fds_data.empty(), "permission-denied fd iteration is harmless");
        fs::permissions(f.proc() / "42/fd", fs::perms::owner_all);
        reader.poll(start + 4302ms);
        check(reader.fds_data.size() == 1, "poll recovers when permission returns");
    } else {
        std::cout << "Permission-denial checks skipped: root can bypass mode bits\n";
    }
}

static void test_device_memory() {
    Fixture f;
    write(f.proc() / "10/fdinfo/1", fd("xe", "7", "drm-resident-gtt: 256 MiB\n"));
    write(f.proc() / "11/fdinfo/2", fd("xe", "7", "drm-resident-gtt: 256 MiB\n"));
    write(f.proc() / "12/fdinfo/3", fd("xe", "8", "drm-resident-gtt: 512 MiB\n"));
    write(f.proc() / "13/fdinfo/4", fd("xe", "9", "drm-resident-gtt: 99 GiB\n", "0000:03:00.0"));
    write(f.proc() / "14/fdinfo/5", fd("i915", "9", "drm-resident-system0: 2 GiB\n"));
    write(f.proc() / "15/fdinfo/6", fd("xe", "10", "drm-total-gtt: 10 GiB\n"));
    write(f.proc() / "not-a-pid/fdinfo/7", fd("xe", "11", "drm-resident-gtt: 99 GiB\n"));
    fs::create_directories(f.proc() / "16/fdinfo");
    if (geteuid() != 0) {
        write(f.proc() / "16/fdinfo/8", fd("xe", "12", "drm-resident-gtt: 99 GiB\n"));
        fs::permissions(f.proc() / "16/fdinfo", fs::perms::none);
    }
    auto xe = f.intel();
    auto i915 = f.intel("i915");
    const auto start = std::chrono::steady_clock::now();
    check(near(xe.get_device_memory_used(start), .75),
          "device estimate matches driver and PCI, deduplicates cross-process clients, skips denied data");
    check(near(i915.get_device_memory_used(start), 2), "i915 resident shared memory across proc");
    write(f.proc() / "17/fdinfo/9", fd("xe", "13", "drm-resident-vram0: 1 GiB\n"));
    for (int n = 1; n < 80; ++n)
        check(near(xe.get_device_memory_used(start + n * 25ms), .75),
              "whole-device scan independently throttled at every metrics tick");
    check(near(xe.get_device_memory_used(start + 2s), 1.75), "whole-device refresh at two seconds");
    fs::permissions(f.proc() / "16/fdinfo", fs::perms::owner_all);
    fs::remove_all(f.proc() / "16");
    fs::remove_all(f.proc() / "10");
    fs::remove_all(f.proc() / "11");
    check(near(xe.get_device_memory_used(start + 4s), 1.5), "device estimate drops terminated clients");
    write(f.proc() / "18/fdinfo/1", "drm-driver: panfrost\ndrm-client-id: 50\ndrm-resident-memory: 128 MiB\n");
    write(f.proc() / "19/fdinfo/1", "drm-driver: panthor\ndrm-client-id: 51\ndrm-resident-memory: 256 MiB\n");
    FDInfoDeviceMemory panfrost("panfrost", "", {"drm-resident-memory"}, f.proc().string());
    FDInfoDeviceMemory panthor("panthor", "", {"drm-resident-memory"}, f.proc().string());
    check(near(panfrost.get_memory_used(start), .125), "platform DRM device without PCI metadata");
    check(near(panthor.get_memory_used(start), .25), "platform drivers remain separate");
    add_fd(f, 20, 2, "drm-driver: panfrost\ndrm-client-id: 52\ndrm-resident-memory: 512 MiB\n", "renderD128");
    add_fd(f, 21, 3, "drm-driver: panfrost\ndrm-client-id: 53\ndrm-resident-memory: 2 GiB\n", "renderD129");
    FDInfoDeviceMemory platform_node("panfrost", "", {"drm-resident-memory"}, f.proc().string(), "renderD128");
    check(near(platform_node.get_memory_used(start), .5), "platform DRM node filters other devices and unreadable identity");
}

static void test_power_and_temperature() {
    Fixture f;
    const auto package = f.powercap() / "intel-rapl:0";
    write(package / "name", "package-0\n");
    write(package / "energy_uj", "999999999\n");
    write(package / "constraint_0_power_limit_uw", "200000000\n");
    const auto core = f.powercap() / "intel-rapl:0:0";
    write(core / "name", "core\n");
    write(core / "energy_uj", "888888888\n");
    auto unavailable = f.intel();
    unavailable.setup(true, true, true);
    check(unavailable.get_power_usage() == 0, "CPU package energy never used as GPU power");
    check(unavailable.get_power_limit() == 0, "CPU package power cap never used as GPU limit");
    check(unavailable.get_temperature() == 0, "missing package temperature harmless");

    const auto uncore = f.powercap() / "intel-rapl:0:1";
    write(uncore / "name", "uncore\n");
    write(uncore / "energy_uj", "900000\n");
    write(uncore / "max_energy_range_uj", "1000000\n");
    write(uncore / "constraint_0_power_limit_uw", "25000000\n");
    const auto hw = f.hwmon() / "hwmon0";
    write(hw / "name", "coretemp\n");
    write(hw / "temp1_label", "Core 0\n");
    write(hw / "temp1_input", "99000\n");
    write(hw / "temp2_label", "Package id 1\n");
    write(hw / "temp2_input", "98000\n");
    write(hw / "temp3_label", "Package id 0\n");
    write(hw / "temp3_input", "65000\n");

    // Use real HwmonBase registration and opening semantics. Merely registered
    // sensor names must not suppress Intel fallback discovery.
    HwmonBase gpu_hwmon;
    gpu_hwmon.base_dir = (f.root / "gpu-hwmon").string();
    fs::create_directories(gpu_hwmon.base_dir);
    gpu_hwmon.setup({{"energy", "energy1_input"}, {"temp", "temp1_input"}, {"power_limit", "power1_max"}});
    check(gpu_hwmon.is_exists("energy") && !gpu_hwmon.is_open("energy"),
          "registered missing hwmon sensor is not an open sensor");
    auto fallback = f.intel();
    fallback.setup(!gpu_hwmon.is_open("energy"), !gpu_hwmon.is_open("power_limit"), !gpu_hwmon.is_open("temp"));
    const auto start = std::chrono::steady_clock::now();
    check(fallback.get_power_usage(start) == 0, "RAPL first sample has no startup spike");
    write(uncore / "energy_uj", "100000\n");
    check(near(fallback.get_power_usage(start + 1s), .2), "RAPL counter wrap conversion");
    write(uncore / "energy_uj", "400000\n");
    check(near(fallback.get_power_usage(start + 2s), .3), "RAPL normal delta conversion");
    check(fallback.get_power_usage(start + 2s) == 0, "RAPL zero elapsed time safe");
    write(uncore / "energy_uj", "broken\n");
    check(fallback.get_power_usage(start + 3s) == 0, "RAPL malformed read safe");
    write(uncore / "energy_uj", "0\n");
    check(fallback.get_power_usage(start + 4s) == 0, "RAPL recovery resets baseline");
    write(uncore / "energy_uj", "200000\n");
    check(near(fallback.get_power_usage(start + 5s), .2), "RAPL zero baseline is a valid sample");
    check(near(fallback.get_power_limit(), 25), "uncore cap in watts");
    check(fallback.get_temperature() == 65, "coretemp Package id 0 fallback");
    write(hw / "temp3_input", "72000\n");
    check(fallback.get_temperature() == 72, "retained temperature stream is refreshed");

    write(f.root / "gpu-hwmon/energy1_input", "123\n");
    write(f.root / "gpu-hwmon/temp1_input", "41000\n");
    write(f.root / "gpu-hwmon/power1_max", "50000000\n");
    gpu_hwmon.setup({{"energy", "energy1_input"}, {"temp", "temp1_input"}, {"power_limit", "power1_max"}});
    auto dedicated = f.intel();
    dedicated.setup(!gpu_hwmon.is_open("energy"), !gpu_hwmon.is_open("power_limit"), !gpu_hwmon.is_open("temp"));
    gpu_hwmon.poll_sensors();
    check(gpu_hwmon.is_open("energy") && gpu_hwmon.is_open("temp"), "dedicated GPU hwmon detected");
    check(dedicated.get_power_limit() == 0 && dedicated.get_temperature() == 0,
          "fallback readers remain closed when dedicated GPU sensors open");
    check(gpu_hwmon.get_sensor_value("temp") == 41000, "dedicated GPU temperature preserved");
}

int main() {
    try {
        test_parser();
        test_process_discovery();
        test_device_memory();
        test_power_and_temperature();
        std::cout << "Intel fallback fixtures: " << checks << " checks passed\n";
        return 0;
    } catch (const std::exception& error) {
        std::cerr << "Intel fallback fixture failed: " << error.what() << '\n';
        return 1;
    }
}
