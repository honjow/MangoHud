#pragma once

#include <string>
#include <stdint.h>

class i915_drm_base {
private:
    int card_fd = -1;
    uint64_t total_memory = 0;
    uint64_t free_memory = 0;
    uint64_t used_memory = 0;

public:
    i915_drm_base();
    ~i915_drm_base();
    i915_drm_base(const i915_drm_base&) = delete;
    i915_drm_base& operator=(const i915_drm_base&) = delete;
    bool setup(const std::string& card);
    void poll();
    uint64_t get_total_memory() const;
    uint64_t get_used_memory() const;
};

struct i915_drm {
    i915_drm_base drm;
    i915_drm();
};
