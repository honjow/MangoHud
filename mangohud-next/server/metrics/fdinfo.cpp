#include <algorithm>
#include <charconv>
#include <filesystem>
#include <limits>
#include <spdlog/spdlog.h>
#include "fdinfo.hpp"

namespace fs = std::filesystem;
using namespace std::chrono_literals;

fdinfo_data read_fdinfo(std::istream& stream) {
    fdinfo_data data;
    for (std::string line; std::getline(stream, line);) {
        if (line.empty() || line.front() == ' ' || line.front() == '\t')
            continue;
        const auto colon = line.find(':');
        if (colon == std::string::npos)
            continue;
        const auto first = line.find_first_not_of(" \t", colon + 1);
        if (first == std::string::npos)
            continue;
        const auto last = line.find_last_not_of(" \t\r");
        data[line.substr(0, colon)] = line.substr(first, last - first + 1);
    }
    return data;
}

uint64_t fdinfo_memory_bytes(const std::string& value) {
    uint64_t bytes = 0;
    const auto parsed = std::from_chars(value.data(), value.data() + value.size(), bytes);
    if (parsed.ec != std::errc())
        return 0;
    const auto first = value.find_first_not_of(" \t", parsed.ptr - value.data());
    const auto unit = first == std::string::npos ? "" : value.substr(first);
    uint64_t scale = 1;
    if (unit == "KiB")
        scale = 1024;
    else if (unit == "MiB")
        scale = 1024 * 1024;
    else if (unit == "GiB")
        scale = 1024 * 1024 * 1024;
    else if (!unit.empty() && unit != "B")
        return 0;
    return bytes <= std::numeric_limits<uint64_t>::max() / scale ? bytes * scale : 0;
}

uint64_t fdinfo_memory_bytes(const fdinfo_data& data,
                             std::initializer_list<const char*> keys) {
    for (const auto* key : keys) {
        const auto found = data.find(key);
        // A present zero-valued dedicated-memory key must not select GTT.
        if (found != data.end())
            return fdinfo_memory_bytes(found->second);
    }
    return 0;
}

FDInfoDeviceMemory::FDInfoDeviceMemory(const std::string& driver, const std::string& pci_dev,
                                       std::initializer_list<const char*> resident_keys,
                                       const std::string& proc_root, const std::string& drm_node)
    : driver(driver), pci_dev(pci_dev), proc_root(proc_root), drm_node(drm_node),
      resident_keys(resident_keys.begin(), resident_keys.end()) {
    if (drm_node.empty() || !pci_dev.empty())
        return;
    std::error_code ec;
    const auto device = fs::path("/sys/class/drm") / drm_node / "device/drm";
    fs::directory_iterator it(device, fs::directory_options::skip_permission_denied, ec), end;
    for (; !ec && it != end; it.increment(ec)) {
        const auto name = it->path().filename().string();
        if (name.rfind("card", 0) == 0) {
            card_node = name;
            break;
        }
    }
}

float FDInfoDeviceMemory::get_memory_used(chrono_timer now) {
    // An empty per-process fd list must never force this expensive scan.
    if (have_sample && now - last_scan < 2s)
        return cached_memory_used;
    have_sample = true;
    last_scan = now;

    std::error_code ec;
    fs::directory_iterator it(proc_root, fs::directory_options::skip_permission_denied, ec), end;
    if (ec)
        return cached_memory_used;
    std::set<std::string> clients;
    double total = 0;
    for (; !ec && it != end; it.increment(ec)) {
        const auto name = it->path().filename().string();
        if (name.empty() || name.find_first_not_of("0123456789") != std::string::npos)
            continue;
        std::error_code fd_ec;
        fs::directory_iterator fd(it->path() / "fdinfo",
            fs::directory_options::skip_permission_denied, fd_ec), fd_end;
        for (; !fd_ec && fd != fd_end; fd.increment(fd_ec)) {
            std::ifstream stream(fd->path());
            if (!stream.is_open())
                continue;
            const auto data = read_fdinfo(stream);
            const auto fd_driver = data.find("drm-driver");
            const auto pdev = data.find("drm-pdev");
            const auto client = data.find("drm-client-id");
            if (fd_driver == data.end() || fd_driver->second != driver ||
                (pdev == data.end() ? std::string() : pdev->second) != pci_dev ||
                client == data.end() || client->second.empty())
                continue;
            // Platform DRM devices have no PCI identity. When the caller
            // supplies a node, do not mix multiple GPUs using the same driver.
            if (pci_dev.empty() && !drm_node.empty()) {
                std::error_code link_ec;
                const auto link = fs::read_symlink(it->path() / "fd" / fd->path().filename(), link_ec);
                if (link_ec || (link.filename() != drm_node &&
                    (card_node.empty() || link.filename() != card_node)))
                    continue;
            }
            if (!clients.insert(client->second).second)
                continue;
            // Whole-device estimate uses resident memory, never allocated totals.
            for (const auto& key : resident_keys) {
                const auto value = data.find(key);
                if (value != data.end()) {
                    total += fdinfo_memory_bytes(value->second);
                    break;
                }
            }
        }
    }
    cached_memory_used = total / (1024.0 * 1024.0 * 1024.0);
    return cached_memory_used;
}

FDInfoBase::FDInfoBase(const std::string& drm_node, pid_t pid,
                       const std::string& proc_root, const std::string& drm_root)
    : proc_root(proc_root), drm_root(drm_root), drm_node(drm_node), pid(pid) {
    card_node = get_card_node();
    init();
}

void FDInfoBase::init(chrono_timer now) {
    open_fds(find_fds());
    last_init = now;
}

void FDInfoBase::poll(chrono_timer now) {
    // Late-created buffer clients are common on Intel. Fast discovery while
    // empty is limited to this PID; whole-device scans have their own timer.
    if (fds_streams.empty() || now - last_init >= 2s)
        init(now);

    fds_data.clear();
    std::set<std::string> client_ids;
    for (auto it = fds_streams.begin(); it != fds_streams.end();) {
        auto& stream = it->second;
        stream.clear();
        stream.seekg(0);
        auto data = read_fdinfo(stream);
        const auto id = data.find("drm-client-id");
        if (id == data.end() || !client_ids.insert(id->second).second) {
            it = fds_streams.erase(it);
            continue;
        }
        fds_data.push_back(std::move(data));
        ++it;
    }
}

std::vector<std::string> FDInfoBase::find_fds() {
    const fs::path path = fs::path(proc_root) / std::to_string(pid) / "fd";
    std::vector<std::string> fds;
    std::error_code ec;
    // Permission failures and process exits must not abort metrics polling.
    fs::directory_iterator it(path, fs::directory_options::skip_permission_denied, ec), end;
    for (; !ec && it != end; it.increment(ec)) {
        std::error_code link_ec;
        const auto link = fs::read_symlink(it->path(), link_ec);
        if (link_ec)
            continue;
        if (link.filename() == drm_node ||
            (!card_node.empty() && link.filename() == card_node))
            fds.push_back(it->path().filename().string());
    }
    if (ec)
        SPDLOG_TRACE("failed to iterate {}: {}", path.string(), ec.message());
    return fds;
}

void FDInfoBase::open_fds(const std::vector<std::string>& fds) {
    std::set<std::string> paths;
    for (const auto& fd : fds)
        paths.insert((fs::path(proc_root) / std::to_string(pid) / "fdinfo" / fd).string());

    for (auto it = fds_streams.begin(); it != fds_streams.end();) {
        if (paths.find(it->first) == paths.end())
            it = fds_streams.erase(it);
        else
            ++it;
    }
    for (const auto& path : paths) {
        // Retain readable streams across rescans rather than reopening all
        // handles every two seconds. Expired clients are removed by poll().
        if (fds_streams.find(path) != fds_streams.end())
            continue;
        std::ifstream stream(path);
        if (stream.is_open())
            fds_streams.emplace(path, std::move(stream));
    }
}

std::string FDInfoBase::get_card_node() {
    const auto device = fs::path(drm_root) / drm_node / "device/drm";
    std::error_code ec;
    fs::directory_iterator it(device, fs::directory_options::skip_permission_denied, ec), end;
    for (; !ec && it != end; it.increment(ec)) {
        const auto filename = it->path().filename().string();
        if (filename.rfind("card", 0) == 0)
            return filename;
    }
    return "";
}

void FDInfoWrapper::add_pid(pid_t pid) {
    std::unique_lock lock(pids_mutex);
    pids.try_emplace(pid, drm_node, pid);
}

void FDInfoWrapper::poll_all() {
    std::unique_lock lock(pids_mutex);
    for (auto it = pids.begin(); it != pids.end();) {
        std::error_code ec;
        if (!fs::exists("/proc/" + std::to_string(it->first), ec) && !ec)
            it = pids.erase(it);
        else {
            it->second.poll();
            ++it;
        }
    }
}

float FDInfoWrapper::get_memory_used(pid_t pid, const std::string& key) {
    return get_memory_used(pid, {key.c_str()});
}

float FDInfoWrapper::get_memory_used(pid_t pid, std::initializer_list<const char*> keys) {
    std::unique_lock lock(pids_mutex);
    const auto p = pids.find(pid);
    if (p == pids.end())
        return 0.f;
    double total = 0;
    for (const auto& fd : p->second.fds_data)
        total += fdinfo_memory_bytes(fd, keys);
    return total / (1024.0 * 1024.0 * 1024.0);
}

uint64_t FDInfoWrapper::get_gpu_time(pid_t pid, const std::string& key) {
    std::unique_lock lock(pids_mutex);
    const auto p = pids.find(pid);
    if (p == pids.end())
        return 0;
    uint64_t total = 0;
    for (const auto& fd : p->second.fds_data) {
        const auto found = fd.find(key);
        if (found == fd.end())
            continue;
        uint64_t time = 0;
        const auto& value = found->second;
        if (std::from_chars(value.data(), value.data() + value.size(), time).ec == std::errc())
            total += time;
    }
    return total;
}

FDInfo::FDInfo(const std::string& drm_node) : fdinfo(drm_node) {}
