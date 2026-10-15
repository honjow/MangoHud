# Downstream migration onto upstream e1430ab

The merge retains old downstream `dev` e5ff6e0179c5a14b722dbaee9a86cadfec06ac08
as its first parent and official e1430abaa024c57624661b99314ba8f2d4c06d03
as its second parent. It preserves the upstream unified GPU backend rather than
restoring the removed legacy implementation.

## Original customizations

| Original commit | Behavior retained | Current implementation / evidence |
| --- | --- | --- |
| f684ce8 | Charging/discharging `+W` / `-W` | `src/hud_elements.cpp`; original presentation change retained |
| 0d6b7a0 | Handheld fan drivers | `src/platform_sensors.h`, `CPUStats::GetPlatformFanFile`; `platform-sensors` covers all nine added names and upstream divisors |
| ac0ffd1 | AMD v3 CPU temperature fallback | Maximum valid core reading, then CPU hwmon, then SoC; `platform-sensors` covers zero, invalid and priority cases |
| 502850c | Intel uncore RAPL power/limit and resident process-memory fallback | Shared i915/xe `IntelFallbacks`, native hwmon precedence; periodic two-second FD discovery and empty fast path; `intel-fallbacks` |
| 5ab4554 | Whole-device readable-process resident memory | `FDInfoDeviceMemory` deduplicates DRM client IDs and matches GPU identity, independent two-second limit; Intel and platform-DRM paths retain scope |
| 9d0d60e | Intel package temperature fallback | `coretemp` Package id 0 only when a dedicated temperature sensor is unavailable; `intel-fallbacks` |
| e5ff6e0 | MSI Claw 8 EX bogus fixed-current power estimate | Per-battery capacity delta with normal power/current preferred; `battery-fixtures` |

## Integration fixes

- Battery estimation state is independent per pack and invalidated on source,
  direction or device changes. Upstream battery discovery and Deckard behavior
  remain supported. Remaining time sums pack energy, including standby packs.
- i915/xe DRM descriptors are initialized, closed on re-setup and destruction;
  xe query allocations are freed. `intel-drm-resources` instruments real readers
  for 1001 balanced allocation/free pairs and descriptor lifetime.
- The new shared GPU poller must stop before derived sensor members are destroyed.
  Ownership deleters join before deletion for both legacy and next owners.
  `gpu-lifetime` exercises the actual base poller and both ownership forms.
  Poll intervals and the existing stop/join behavior are otherwise unchanged.
- Platform clock reads share the FD-info lock, as does xe process load access.
- The Debian dependency list includes libxi-dev, required by mangoapp. Existing
  workflow files remain byte-for-byte unchanged from downstream dev.
- The next client library installs beside its generated Vulkan manifest, using
  the existing `libdir_mangohud` layout.

## Running tests

Configure normally with `-Dtests=enabled`, build, then run:

    meson test -C build --print-errorlogs

Five suites run on Linux: `platform-sensors`, `battery-fixtures`,
`intel-fallbacks`, `intel-drm-resources`, `gpu-lifetime`. They use temporary
sysfs/proc-shaped trees and production parsing/resource code. Test executables
are not installed. The unchanged Ubuntu validation workflow compiles them with
GCC and Clang; the isolated Arch package PR executes all five suites.

Standalone commands and coverage are in `battery_README.md` and
`intel_README.md`. Permission-denial checks require a non-root runner; root runs
explicitly report their omission. No fixtures establish actual sensor accuracy,
GPU rendering, overlay appearance, or hotplug behavior on physical hardware.
