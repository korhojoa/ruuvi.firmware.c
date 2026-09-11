#!/usr/bin/env bash
# Build and operate the host simulation of the vlongmem log module with the
# sanitizers. Refer to test/sim/sim_vlongmem.c.
#
#   scripts/sim-vlongmem.sh            # 800 simulated days
#   scripts/sim-vlongmem.sh 400        # 400 days
#   scripts/sim-vlongmem.sh 400 v      # with the module log output
set -euo pipefail
ROOT="$(cd "$(dirname "$0")/.." && pwd)"
OUT="${TMPDIR:-/tmp}/vlongmem-sim"
mkdir -p "${OUT}"
SRC="${ROOT}/src"
DRV="${SRC}/ruuvi.drivers.c/src"
INC=(-I "${ROOT}/test/sim" -I "${SRC}" -I "${SRC}/application_config"
     -I "${SRC}/ruuvi.boards.c" -I "${DRV}" -I "${DRV}/tasks"
     -I "${DRV}/interfaces/flash" -I "${DRV}/interfaces/log" -I "${DRV}/interfaces/rtc"
     -I "${DRV}/interfaces/yield" -I "${DRV}/interfaces/communication"
     -I "${DRV}/interfaces/environmental" -I "${DRV}/interfaces/acceleration"
     -I "${DRV}/interfaces/adc" -I "${DRV}/interfaces/gpio" -I "${DRV}/interfaces/timer"
     -I "${DRV}/interfaces/scheduler" -I "${DRV}/interfaces/watchdog"
     -I "${DRV}/interfaces/power" -I "${DRV}/interfaces/atomic"
     -I "${SRC}/ruuvi.endpoints.c/src" -I "${SRC}/ruuvi.libraries.c/src")
DEFS=(-DBOARD_RUUVITAG_B -DAPPLICATION_MODE_VLONGMEM -DCEEDLING)
gcc -std=gnu11 -O1 -g -fsanitize=address,undefined -fno-omit-frame-pointer \
    -Wall -Wextra -Wno-unused-parameter \
    "${INC[@]}" "${DEFS[@]}" \
    "${SRC}/app_log_vlongmem.c" \
    "${SRC}/app_log_vlongmem_codec.c" \
    "${SRC}/app_log_vlongmem_time.c" \
    "${SRC}/app_log_vlongmem_fake.c" \
    "${DRV}/ruuvi_driver_sensor.c" \
    "${ROOT}/test/sim/sim_vlongmem.c" \
    -lm -o "${OUT}/sim"
"${OUT}/sim" "$@"
