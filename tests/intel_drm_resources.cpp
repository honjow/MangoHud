// Exercise the real DRM readers with wrapped ioctls/allocations. GPU hardware
// is not needed. Link with --wrap=ioctl --wrap=calloc --wrap=free.
#include <cstdarg>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <iostream>
#include <stdexcept>
#include <type_traits>
#include <libdrm/i915_drm.h>
#include "mangohud-next/server/metrics/gpu/intel/i915/i915_drm.hpp"
#include "mangohud-next/server/metrics/gpu/intel/xe/xe_drm.hpp"
#include "mangohud-next/server/metrics/gpu/intel/xe/include/xe_drm.h"

static size_t ioctls = 0, allocations = 0, frees = 0;
static void* last_allocation = nullptr;
static bool fail_query = false;
static void check(bool value, const char* message) {
    if (!value)
        throw std::runtime_error(message);
}

extern "C" void* __real_calloc(size_t count, size_t size);
extern "C" void __real_free(void* pointer);
extern "C" void* __wrap_calloc(size_t count, size_t size) {
    auto* pointer = __real_calloc(count, size);
    last_allocation = pointer;
    ++allocations;
    return pointer;
}
extern "C" void __wrap_free(void* pointer) {
    if (pointer && pointer == last_allocation) {
        ++frees;
        last_allocation = nullptr;
    }
    __real_free(pointer);
}
extern "C" int __wrap_ioctl(int, unsigned long request, ...) {
    ++ioctls;
    va_list args;
    va_start(args, request);
    auto* argument = va_arg(args, void*);
    va_end(args);
    if (request == DRM_IOCTL_XE_DEVICE_QUERY) {
        auto* query = static_cast<drm_xe_device_query*>(argument);
        if (!query->data) {
            query->size = sizeof(drm_xe_query_mem_regions) + sizeof(drm_xe_mem_region);
            return 0;
        }
        if (fail_query)
            return -1;
        auto* regions = reinterpret_cast<drm_xe_query_mem_regions*>(query->data);
        regions->num_mem_regions = 1;
        regions->mem_regions[0].mem_class = DRM_XE_MEM_REGION_CLASS_VRAM;
        regions->mem_regions[0].total_size = 3ULL << 30;
        regions->mem_regions[0].used = 1ULL << 30;
        return 0;
    }
    if (request == DRM_IOCTL_I915_QUERY) {
        auto* query = static_cast<drm_i915_query*>(argument);
        auto* item = reinterpret_cast<drm_i915_query_item*>(query->items_ptr);
        if (!item->data_ptr) {
            item->length = sizeof(drm_i915_query_memory_regions) + sizeof(drm_i915_memory_region_info);
            return 0;
        }
        auto* regions = reinterpret_cast<drm_i915_query_memory_regions*>(item->data_ptr);
        regions->num_regions = 1;
        regions->regions[0].region.memory_class = I915_MEMORY_CLASS_DEVICE;
        regions->regions[0].probed_size = 3ULL << 30;
        regions->regions[0].unallocated_size = 2ULL << 30;
        return 0;
    }
    return -1;
}

static size_t open_descriptors() {
    size_t result = 0;
    for ([[maybe_unused]] const auto& entry : std::filesystem::directory_iterator("/proc/self/fd"))
        ++result;
    return result;
}

int main() {
    static_assert(!std::is_copy_constructible_v<xe_drm_base>);
    static_assert(!std::is_copy_assignable_v<xe_drm_base>);
    static_assert(!std::is_copy_constructible_v<i915_drm_base>);
    static_assert(!std::is_copy_assignable_v<i915_drm_base>);
    try {
        const auto before = open_descriptors();
        {
            xe_drm_base xe;
            i915_drm_base i915;
            xe.poll();
            i915.poll();
            check(ioctls == 0, "unopened readers must not ioctl stdin");
            check(xe.setup("/dev/null") && i915.setup("/dev/null"), "open harmless test device");
            check(open_descriptors() == before + 2, "one descriptor per reader");
            check(xe.setup("/dev/null") && i915.setup("/dev/null"), "reader re-setup");
            check(open_descriptors() == before + 2, "re-setup closes previous descriptors");
            for (int n = 0; n < 1000; ++n)
                xe.poll();
            check(allocations == 1000 && frees == 1000, "every successful xe query frees its buffer");
            check(xe.get_total_memory() == 3ULL << 30, "xe query total preserved");
            check(xe.get_used_memory() == 1ULL << 30, "xe query usage preserved");
            fail_query = true;
            xe.poll();
            check(allocations == 1001 && frees == 1001, "failed xe fetch frees its buffer");
            i915.poll();
            check(i915.get_total_memory() == 3ULL << 30, "i915 query total preserved");
            check(i915.get_used_memory() == 1ULL << 30, "i915 query usage preserved");
        }
        check(open_descriptors() == before, "both DRM destructors close device descriptors");
        std::cout << "Intel DRM resources: 1001 allocation/free pairs and descriptor lifecycle passed\n";
        return 0;
    } catch (const std::exception& error) {
        std::cerr << "Intel DRM resource fixture failed: " << error.what() << '\n';
        return 1;
    }
}
