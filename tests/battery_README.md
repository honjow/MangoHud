# Battery migration fixtures

Run the focused suite from the repository root:

```sh
tests/battery_run_fixtures.sh
```

The runner compiles the real `src/battery.cpp`, `src/file_utils.cpp`,
`src/string_utils.h`, and bundled `include/filesystem.h` with C++20,
`-Wall -Wextra -Werror`. It uses real spdlog when pkg-config provides it;
otherwise it explicitly reports a temporary **logging-only** shim. It does not
substitute the filesystem, file-reading implementation, or battery code.
The Meson target should use these same two production source files, the `src`
and `include` include directories, and the spdlog dependency.

`BatteryTestAccess` only changes private filesystem roots and the steady-clock
function. No environment variable, command-line flag, or public runtime setting
is added. Fixtures are created in unique temporary directories and removed.
The suite never reads or writes real battery sysfs files and does not sleep.

## Coverage

- Positive and signed current, charging watts and status, full/unknown/idle states
- Inclusive 30 A and 200 W limits; MSI Claw-style 65 A, missing/zero current,
  and implausible/invalid `power_now`
- Charge and energy differencing, minimum one-second interval, stepped updates,
  unchanged capacity, 0.7/0.3 exponential smoothing, and capacity resets
- Energy-only hardware, including no `voltage_now`, and direct/estimated runtime
- Isolated two-battery sample caches, summed rate history, different units and
  voltages, and 25-sample history bound
- Status transitions, removed/replaced paths, capacity source changes, and rescan
- Sequential discharge: a full or idle reserve pack contributes stored energy
  while only the active pack contributes discharge power
- Type-based discovery, Device-scope exclusion, canonical-symlink deduplication,
  two-battery bound, and missing batteries
- Percentage bounds, missing/zero capacity totals, mixed capacity units
- Steam Frame/Deckard discovery, percentage override, system rail + backlight,
  invalid-read retention, and its upstream charging/full/not-charging semantics

## Necessary corrections made during migration

1. Capacity-estimation state now belongs to each battery and resets when its path,
   charging state, or capacity source changes. One pack cannot contaminate another.
2. Runtime uses dimensionally consistent rates: `W / µV * 10^12 = µA`,
   `µAh * µV / 10^6 = µWh`, and `µWh / µW = hours`. The former
   `µW / µV = µA` and `µWh / µV = µAh` comments/conversions missed a factor
   of one million. Energy-only runtime does not need an invented voltage.
3. Runtime history contains one aggregate rate per update, instead of averaging
   individual pack rates and then dividing the sum of both capacities by that mean.
4. Unreadable/nonfinite values and zero capacity totals do not crash or yield NaN;
   idle/removed packs cannot hide another active pack's power or leak stale state.

These are source-level fixtures, not a full MangoHud build or hardware validation.
The complete build and on-device checks still need to exercise MSI Claw firmware,
charging/discharging HUD signs, and Steam Frame telemetry.

Optional sanitizer invocation:

```sh
ASAN_OPTIONS=detect_leaks=0 \
CXXFLAGS='-fsanitize=address,undefined -fno-omit-frame-pointer -g' \
  tests/battery_run_fixtures.sh
```

Leak checking is disabled in that example because the task executor runs under
ptrace, which LeakSanitizer does not support. AddressSanitizer and UBSan remain
active. On an ordinary development machine, omit `ASAN_OPTIONS` to enable the
normal leak check as well.
