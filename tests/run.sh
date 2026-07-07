#!/usr/bin/env bash
# Host-side decode test. Generates the washer mapping.h from the upstream
# fixture, then compiles + runs test_decode.cpp against it. No hardware needed.
set -euo pipefail
here="$(cd "$(dirname "$0")" && pwd)"
root="$(cd "$here/.." && pwd)"
build="$here/build"
mkdir -p "$build"

# Regenerate the washer mapping into the build dir (never committed).
python3 "$root/provisioning/generate_mapping.py" \
  "$root/provisioning/tests/fixtures/example_washing_machine_config.json" \
  --out "$build/mapping.h" >/dev/null 2>&1 \
  || "$root/provisioning/.venv/bin/python" "$root/provisioning/generate_mapping.py" \
       "$root/provisioning/tests/fixtures/example_washing_machine_config.json" \
       --out "$build/mapping.h"

CXX="${CXX:-$(command -v g++ || command -v clang++)}"
"$CXX" -std=c++11 -Wall -Wextra \
  -I "$root/esphome/components/homewhiz" \
  -I "$build" \
  "$here/test_decode.cpp" -o "$build/test_decode"

"$build/test_decode"
