#!/usr/bin/env bash
set -euo pipefail

# Usage: bash run-host-tests.sh /path/to/patched/hardware/xiaomi
test_dir="$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")" && pwd)"
hardware_dir="${1:?Pass the hardware/xiaomi tree with dada fingerprint patches applied}"
test_out="$(mktemp -d)"
trap 'rm -rf -- "$test_out"' EXIT

c++ -std=c++17 -DIMPL_V2 -pthread -g -O1 -Wall -Wextra -Werror \
  -Wno-unused-parameter -fsanitize=address,undefined -fno-omit-frame-pointer \
  -I"$test_dir/stubs" -I"$hardware_dir/fingerprint/include" \
  "$test_dir/UdfpsHandlerTest.cpp" -o "$test_out/udfps-test"
"$test_out/udfps-test"
