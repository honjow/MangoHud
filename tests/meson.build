# Deterministic fixtures use production code with temporary sysfs/proc trees.
# No GPU, battery hardware, root access, or running graphics session is needed.
fixture_inc = include_directories('..', '../src', '../include')

platform_sensors_test = executable('test-platform-sensors', 'platform_sensors.cpp')
test('platform-sensors', platform_sensors_test)

battery_test = executable('test-battery-fixtures',
  'battery_fixtures.cpp', '../src/battery.cpp', '../src/file_utils.cpp',
  include_directories: fixture_inc,
  dependencies: spdlog_dep,
  cpp_args: pre_args)
test('battery-fixtures', battery_test)

intel_fallbacks_test = executable('test-intel-fallbacks',
  'intel_fallbacks.cpp',
  '../mangohud-next/server/metrics/fdinfo.cpp',
  '../mangohud-next/server/metrics/hwmon.cpp',
  '../mangohud-next/server/common/helpers.cpp',
  include_directories: fixture_inc,
  dependencies: [spdlog_dep, dep_pthread],
  cpp_args: pre_args)
test('intel-fallbacks', intel_fallbacks_test)

intel_resources_test = executable('test-intel-drm-resources',
  'intel_drm_resources.cpp',
  '../mangohud-next/server/metrics/gpu/intel/i915/i915_drm.cpp',
  '../mangohud-next/server/metrics/gpu/intel/xe/xe_drm.cpp',
  include_directories: fixture_inc,
  dependencies: [spdlog_dep, libdrm_dep],
  cpp_args: pre_args,
  link_args: ['-Wl,--wrap=ioctl', '-Wl,--wrap=calloc', '-Wl,--wrap=free'])
test('intel-drm-resources', intel_resources_test)

gpu_lifetime_test = executable('test-gpu-lifetime',
  'gpu_lifetime.cpp',
  '../mangohud-next/server/metrics/gpu/gpu.cpp',
  '../mangohud-next/server/metrics/fdinfo.cpp',
  include_directories: fixture_inc,
  dependencies: [spdlog_dep, dep_pthread],
  cpp_args: pre_args)
test('gpu-lifetime', gpu_lifetime_test, timeout: 15)
