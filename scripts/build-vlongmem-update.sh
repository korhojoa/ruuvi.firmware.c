#!/usr/bin/env bash
set -euo pipefail
ulimit -c 0
ROOT="$(cd "$(dirname "$0")/.." && pwd)"
if [[ "${1:-}" != --in-container ]]; then
    exec "$ROOT/scripts/podman/build.sh" -- bash /work/scripts/build-vlongmem-update.sh --in-container
fi
SDK=/opt/nRF5_SDK_15.3.0_59ac345/components/libraries/bootloader
ARM=/usr/local/gcc-arm-none-eabi-7-2018-q2-update/bin/arm-none-eabi
OUT="$ROOT/src/targets/ruuvitag_b/armgcc"
PREFIX=ruuvitag_b_armgcc_ruuvifw_vlongmem_v3.34.1-vlongmem
BRIDGE="$OUT/${PREFIX}.2_update_bridge"
TEMP=$(mktemp -d "$OUT/.update-check.XXXXXX")
trap 'rm -rf "$TEMP"' EXIT
"$ARM-gcc" -mcpu=cortex-m4 -mthumb -Os -nostdlib -ffreestanding \
    -fno-unwind-tables -fno-asynchronous-unwind-tables -Wall -Wextra -Werror \
    -Wl,-T,"$ROOT/src/update_bridge/bridge.ld" \
    "$ROOT/src/update_bridge/main.c" -o "$TEMP/bridge.elf"
"$ARM-objcopy" -O ihex --gap-fill 0xff --pad-to 0x27000 "$TEMP/bridge.elf" "$TEMP/bridge.hex"
"$ARM-objcopy" -I ihex -O binary "$TEMP/bridge.hex" "$TEMP/bridge.bin"
"$ARM-objdump" -d "$TEMP/bridge.elf"
"$ARM-objcopy" -I ihex -O binary "$OUT/${PREFIX}.1_app.hex" "$TEMP/previous.bin"
"$ARM-objcopy" -I ihex -O binary "$OUT/${PREFIX}.2_app.hex" "$TEMP/final-unpadded.bin"
FINAL_SIZE=$(stat -c %s "$TEMP/final-unpadded.bin")
FINAL_END=$((0x26000 + ((FINAL_SIZE + 4095) / 4096) * 4096))
"$ARM-objcopy" -I ihex -O ihex --gap-fill 0xff --pad-to "$FINAL_END" \
    "$OUT/${PREFIX}.2_app.hex" "$TEMP/final.hex"
"$ARM-objcopy" -I ihex -O binary "$TEMP/final.hex" "$TEMP/final.bin"
OLD_SIZE=$(stat -c %s "$TEMP/previous.bin")
for header in nrf_bootloader_fw_activation nrf_dfu_settings nrf_dfu_mbr \
    nrf_bootloader_info crc32 nrf_log nrf_log_ctrl nrf_dfu_utils \
    nrf_bootloader_wdt nrf_dfu_validation; do
    touch "$TEMP/$header.h"
done
cp "$SDK/nrf_bootloader_fw_activation.c" "$SDK/dfu/nrf_dfu_utils.c" "$TEMP/"
gcc -D_GNU_SOURCE -std=c11 -Wall -Wextra -Werror -Wno-int-to-pointer-cast \
    -fsanitize=undefined -fno-sanitize-recover=all \
    -include "$ROOT/test/dfu/sdk_stubs.h" -I "$TEMP" -I "$ROOT/test/dfu" \
    "$TEMP/nrf_bootloader_fw_activation.c" "$TEMP/nrf_dfu_utils.c" \
    "$ROOT/test/dfu/test_update.c" -o "$TEMP/test-update"
"$TEMP/test-update" "$OLD_SIZE" "$TEMP/bridge.bin" "$TEMP/final.bin"
nrfutil pkg generate --application "$TEMP/bridge.hex" --application-version 1 \
    --hw-version 0xB0 --sd-req 0xB7 --key-file "$OUT/ruuvi_open_private.pem" \
    "$TEMP/bridge.zip"
cp "$TEMP/bridge.hex" "$BRIDGE.hex"
cp "$TEMP/bridge.zip" "$BRIDGE.zip"
nrfutil pkg generate --application "$TEMP/final.hex" --application-version 1 \
    --hw-version 0xB0 --sd-req 0xB7 --key-file "$OUT/ruuvi_open_private.pem" \
    "$TEMP/after-bridge.zip"
cp "$TEMP/after-bridge.zip" "$OUT/${PREFIX}.2_after_bridge.zip"
unzip -p "$TEMP/bridge.zip" bridge.bin | cmp - "$TEMP/bridge.bin"
unzip -p "$TEMP/after-bridge.zip" final.bin | cmp - "$TEMP/final.bin"
echo "Built temporary DFU application: $(stat -c %s "$TEMP/bridge.bin") bytes"
echo "No hardware update performed. Refer to docs/vlongmem/UPDATE.md."
