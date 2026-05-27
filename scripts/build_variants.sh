#!/bin/bash
set -euo pipefail

ROOT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
IDF_PY="${IDF_PY:-idf.py}"

if ! command -v "$IDF_PY" >/dev/null 2>&1; then
    echo "ERROR: idf.py not found in PATH. Source ESP-IDF export.sh first." >&2
    exit 1
fi

build_variant() {
    local variant_name="$1"
    local hw_variant="$2"
    local behavior_variant="$3"
    local defaults_file="$4"
    local build_dir="$ROOT_DIR/build-${variant_name}"
    local sdkconfig_path="$build_dir/sdkconfig"

    echo ""
    echo "=== Building ${variant_name} ==="
    mkdir -p "$build_dir"

    "$IDF_PY" \
        -C "$ROOT_DIR" \
        -B "$build_dir" \
        -D SDKCONFIG="$sdkconfig_path" \
        -D SDKCONFIG_DEFAULTS="sdkconfig.defaults;${defaults_file}" \
        -D TAG_HW_VARIANT="$hw_variant" \
        -D TAG_BEHAVIOR_VARIANT="$behavior_variant" \
        build
}

build_variant "seeed" "seeed_c3" "standard" "sdkconfig.seeed.defaults"
build_variant "production" "production" "standard" "sdkconfig.production.defaults"
build_variant "demo" "production" "demo" "sdkconfig.production.defaults"
