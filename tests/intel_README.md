# Intel / DRM fallback regression fixtures

These tests use the production readers against temporary proc/sysfs-shaped
files. They need no Intel GPU, root privileges, or mounted fake filesystem.
Run the Meson `intel-fallbacks` and `intel-drm-resources` tests, or compile from
the repository root with a C++20 compiler and the normal spdlog/libdrm headers.

## Source and dependency list

`intel_fallbacks.cpp`:
- `mangohud-next/server/metrics/fdinfo.cpp`
- `mangohud-next/server/metrics/hwmon.cpp`
- `mangohud-next/server/common/helpers.cpp`
- Project root include directory, spdlog dependency, threads

`intel_drm_resources.cpp`:
- `mangohud-next/server/metrics/gpu/intel/i915/i915_drm.cpp`
- `mangohud-next/server/metrics/gpu/intel/xe/xe_drm.cpp`
- Project root include directory, spdlog and libdrm dependencies
- Link arguments: `-Wl,--wrap=ioctl`, `-Wl,--wrap=calloc`, `-Wl,--wrap=free`

Example standalone commands:

```sh
c++ -std=c++20 -O1 -g -pthread -I. \
  tests/intel_fallbacks.cpp \
  mangohud-next/server/metrics/fdinfo.cpp \
  mangohud-next/server/metrics/hwmon.cpp \
  mangohud-next/server/common/helpers.cpp \
  $(pkg-config --cflags --libs spdlog) -o /tmp/intel-fallbacks
/tmp/intel-fallbacks

c++ -std=c++20 -O1 -g -I. \
  tests/intel_drm_resources.cpp \
  mangohud-next/server/metrics/gpu/intel/i915/i915_drm.cpp \
  mangohud-next/server/metrics/gpu/intel/xe/xe_drm.cpp \
  $(pkg-config --cflags --libs spdlog libdrm) \
  -Wl,--wrap=ioctl -Wl,--wrap=calloc -Wl,--wrap=free \
  -o /tmp/intel-drm-resources
/tmp/intel-drm-resources
```

## Coverage and limits

- Intel i915/xe runtime resident-memory fallback, present-zero key priority,
  late process clients, empty-list fast discovery, two-second rediscovery,
  retained streams, removal of stale snapshots, and duplicate DRM clients
- Whole-device readable-process memory estimate, PCI/driver matching,
  cross-process deduplication, independent two-second throttle, permission
  failures, and platform devices without PCI metadata
- Optional DRM-node matching prevents mixing platform GPUs of one driver
- RAPL uncore discovery, first sample, wrap, zero timestamp delta, zero energy,
  malformed input and recovery, power limits; CPU package energy is excluded
- Coretemp Package id 0 fallback and dedicated GPU hwmon precedence, including
  the distinction between registered sensor names and genuinely open streams
- Real i915/xe resource management using mocked query responses: 1001 successful
  or failed xe allocation/free pairs and descriptor setup/destruction

The chmod permission-denial cases run as an ordinary user; they explicitly skip
when run as root, which can bypass the mode bits. These tests do not establish
sensor availability or measurement accuracy on a particular physical GPU.
ASan/UBSan can be enabled using the usual compiler flags. LeakSanitizer needs an
execution environment that permits its process inspection; resource-count tests
remain useful where LeakSanitizer cannot run.
