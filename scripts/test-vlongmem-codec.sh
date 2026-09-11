#!/usr/bin/env bash
# Build and operate the unit test of the vlongmem codec on the host with gcc
# and the vendored Unity. The test has no mocks, thus ceedling is not
# necessary.
set -euo pipefail
ROOT="$(cd "$(dirname "$0")/.." && pwd)"
OUT="${TMPDIR:-/tmp}/vlongmem-codec-test"
mkdir -p "${OUT}"
cat > "${OUT}/runner.c" <<'EOF'
#include "unity.h"
void setUp(void); void tearDown(void);
#define T(name) void name(void); RUN_TEST(name);
int main(void) {
    UNITY_BEGIN();
    T(test_first_sample_near_zero_is_delta)
    T(test_first_sample_far_from_zero_escapes)
    T(test_small_deltas_are_one_byte)
    T(test_delta_minus_one_escapes)
    T(test_delta_minus_128_escapes)
    T(test_large_step_escapes_and_resets_reference)
    T(test_missing_field_is_reported_invalid_and_keeps_reference)
    T(test_negative_temperature_absolute)
    T(test_pressure_clamps_below_offset)
    T(test_decode_stops_at_end_marker)
    T(test_decode_stops_on_truncated_sample)
    T(test_decode_stops_on_truncated_escape)
    T(test_decode_end_marker_mid_sample_is_truncation)
    T(test_units_roundtrip)
    T(test_units_invalid_inputs)
    T(test_long_random_walk_roundtrips)
    return UNITY_END();
}
EOF
gcc -std=c99 -Wall -Wextra -Werror -O1 \
    -DUNITY_INCLUDE_DOUBLE \
    -I "${ROOT}/CMock/vendor/unity/src" -I "${ROOT}/src" \
    "${ROOT}/CMock/vendor/unity/src/unity.c" \
    "${ROOT}/src/app_log_vlongmem_codec.c" \
    "${ROOT}/test/test_app_log_vlongmem_codec.c" \
    "${OUT}/runner.c" -lm -o "${OUT}/test"
"${OUT}/test"
