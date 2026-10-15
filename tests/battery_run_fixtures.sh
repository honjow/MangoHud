#!/usr/bin/env bash
# Production filesystem.h, battery.cpp, file_utils.cpp, and string_utils.h.
# Only logging is stubbed when spdlog is not available from pkg-config.
set -euo pipefail
root=$(cd -- "$(dirname -- "$0")/.." && pwd)
build=$(mktemp -d)
trap 'rm -rf "$build"' EXIT
flags=()
libs=()
if pkg-config --exists spdlog; then
    read -r -a flags <<< "$(pkg-config --cflags spdlog)"
    read -r -a libs <<< "$(pkg-config --libs spdlog)"
else
    mkdir -p "$build/shim/spdlog"
    cat > "$build/shim/spdlog/spdlog.h" <<'SHIM'
#pragma once
#define SPDLOG_INFO(...) ((void)0)
#define SPDLOG_ERROR(...) ((void)0)
#define SPDLOG_WARN(...) ((void)0)
#define SPDLOG_DEBUG(...) ((void)0)
SHIM
    flags+=("-I$build/shim")
    echo "spdlog unavailable: logging-only shim enabled" >&2
fi
read -r -a extra_flags <<< "${CXXFLAGS:-}"
"${CXX:-c++}" -std=c++20 -Wall -Wextra -Werror "${extra_flags[@]}" \
    "${flags[@]}" -I"$root/src" -I"$root/include" \
    "$root/tests/battery_fixtures.cpp" "$root/src/battery.cpp" "$root/src/file_utils.cpp" \
    "${libs[@]}" -o "$build/battery-fixtures"
"$build/battery-fixtures"
