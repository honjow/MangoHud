#pragma once

#include <cstdint>
#include <vector>
#include <string>
#include <map>
#include <set>
#include <chrono>
#include <fstream>
#include <mutex>
#include <initializer_list>
#include <sys/types.h>

typedef std::map<std::string, std::string> fdinfo_data;
typedef std::chrono::time_point<std::chrono::steady_clock> chrono_timer;

// Shared by per-process sampling and Intel's whole-device resident estimate.
fdinfo_data read_fdinfo(std::istream& stream);
uint64_t fdinfo_memory_bytes(const std::string& value);
uint64_t fdinfo_memory_bytes(const fdinfo_data& data,
                             std::initializer_list<const char*> keys);

// Readable-process estimate for devices without a native whole-card counter.
// Deduplicates DRM clients across processes and has its own two-second timer.
class FDInfoDeviceMemory {
public:
    FDInfoDeviceMemory(const std::string& driver, const std::string& pci_dev,
                       std::initializer_list<const char*> resident_keys,
                       const std::string& proc_root = "/proc",
                       const std::string& drm_node = "");
    float get_memory_used(chrono_timer now = std::chrono::steady_clock::now());

private:
    const std::string driver, pci_dev, proc_root, drm_node;
    std::string card_node;
    const std::vector<std::string> resident_keys;
    chrono_timer last_scan{};
    bool have_sample = false;
    float cached_memory_used = 0.f;
};

class FDInfoBase {
private:
    std::map<std::string, std::ifstream> fds_streams;
    chrono_timer last_init;
    std::string card_node;
    const std::string proc_root;
    const std::string drm_root;

    std::vector<std::string> find_fds();
    void open_fds(const std::vector<std::string>& fds);
    std::string get_card_node();

public:
    const std::string drm_node;
    const pid_t pid;

    FDInfoBase(const std::string& drm_node, pid_t pid,
               const std::string& proc_root = "/proc",
               const std::string& drm_root = "/sys/class/drm");
    std::vector<fdinfo_data> fds_data;

    void init(chrono_timer now = std::chrono::steady_clock::now());
    void poll(chrono_timer now = std::chrono::steady_clock::now());
};

struct FDInfoWrapper {
    std::mutex pids_mutex;
    std::map<pid_t, FDInfoBase> pids;
    const std::string drm_node;

    explicit FDInfoWrapper(const std::string& drm_node) : drm_node(drm_node) {}

    void add_pid(pid_t pid);
    void poll_all();
    float get_memory_used(pid_t pid, const std::string& key);
    float get_memory_used(pid_t pid, std::initializer_list<const char*> keys);
    uint64_t get_gpu_time(pid_t pid, const std::string& key);
};

struct FDInfo {
    FDInfoWrapper fdinfo;
    explicit FDInfo(const std::string& drm_node);
};
