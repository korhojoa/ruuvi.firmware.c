# Update compatibility

## Stored log format

The interval-settings firmware uses the same log region and version-1 block format.
It can decode blocks from the earlier vlongmem firmware.
Each older block keeps its original sample interval.
An interval setting change flushes the current block before it starts a new grid.

Format compatibility does not make a Bluetooth firmware update safe for stored measurements.
A restart can also discard samples that are still in the RAM buffer.
The normal limit is 16 samples, or 160 minutes at the default interval.

## Bluetooth update staging

The supplied bootloader source at tag `3.1.0-beta` reserves only 20480 bytes for application data.
Its configuration sets `NRF_DFU_SINGLE_BANK_APP_UPDATES` to zero.
It uses the Nordic SDK 15.3 dual-bank update path when the image fits.

The SDK function `nrf_dfu_bank1_start_addr()` aligns the end of the installed application to a flash page.
The update then uses that address as its temporary image buffer.
The calculation is:

```text
staging_start = align_up(previous_application_end, 4096)
staging_end = align_up(staging_start + new_application_size, 4096)
```

For the retained application images, these are the calculated regions:

| Region | Start | End, exclusive |
| --- | --- | --- |
| Previous vlongmem application | 0x26000 | 0x3FB0C |
| New application, 106168 bytes | 0x26000 | 0x3FEB8 |
| Temporary update pages | 0x40000 | 0x5A000 |
| Measurement ring | 0x44000 | 0x72000 |
| Overlap | 0x44000 | 0x5A000 |

The temporary update overwrites 22 pages of the measurement ring.
Thus the normal application DFU package must not be used to preserve history on an existing tag.
A successful history export preserves a separate copy, not the original data on the tag.

The source configuration comes from
[the released bootloader](https://github.com/ruuvi/ruuvi.nrf5_sdk15_bootloader.c/blob/3.1.0-beta/ruuvitag_b_s132_6.1.1/config/sdk_config.h).
The address calculation uses the retained old and new application HEX files.
No update was performed on a tag for this check.
The installed bootloader binary and settings were not read from the tag.
A different installed bootloader needs its own verification.

## Two-stage Bluetooth update

A small temporary application changes the location of the next update buffer.
The temporary application occupies one flash page at `0x26000`.
It sets `GPREGRET` to `0xB1` and requests a system reset.
The supplied bootloader then starts Bluetooth DFU again.
The temporary application does not write flash or collect measurements.

The two packages use these regions:

| Operation | Start | End, exclusive |
| --- | --- | --- |
| Receive the temporary application | 0x40000 | 0x41000 |
| Install the temporary application | 0x26000 | 0x27000 |
| Receive the final application | 0x27000 | 0x41000 |
| Install the final application | 0x26000 | 0x40000 |

These regions end before the measurement ring at `0x44000`.
The bootloader copies the second image downwards, with progress records for each page.
The packages include erased padding through the end of the last page.
Thus each saved copy offset stays page-aligned, including after an interrupted final copy.
The final application code is identical to the normal vlongmem.2 image.
Only its trailing erased padding differs.

### Build and verification

Run this command from the repository root:

```sh
bash scripts/build-vlongmem-update.sh
```

The script uses the existing firmware container and the normal output directory.
It does not change the container image or update a device.
It removes its temporary test files when it ends.
The script produces these packages in `src/targets/ruuvitag_b/armgcc/`:

1. `ruuvitag_b_armgcc_ruuvifw_vlongmem_v3.34.1-vlongmem.2_update_bridge.zip`
2. `ruuvitag_b_armgcc_ruuvifw_vlongmem_v3.34.1-vlongmem.2_after_bridge.zip`

The host test compiles the unmodified SDK cache and activation sources with simulated flash functions.
It uses the actual application images and a full measurement region with known bytes.
It verifies the copied images and an identical measurement region after each update.
It also simulates a restart at 83 erase, write, and progress-save points.
The test passes with the undefined-behavior sanitizer.
The script verifies that each ZIP contains the tested binary.
These are host test results, not a hardware qualification.
The installed bootloader and the two-stage procedure still need hardware verification.

### Installation sequence for the supplied bootloader

CAUTION: Do not install the final package first.
The installed application must have the verified vlongmem.1 size.
A different installed application or bootloader requires another address calculation.

1. Export the full measurement history before the update.
2. Disconnect Ruuvi Station from the tag.
3. Push button B briefly to permit the normal Bluetooth update.
4. Install the `update_bridge.zip` package with nRF Connect Secure DFU.
5. Reconnect to the bootloader, which advertises as `RuuviBoot` in the supplied configuration.
6. Install the `after_bridge.zip` package with nRF Connect Secure DFU.
7. Reconnect Ruuvi Station and verify the firmware revision and historical measurements.

The temporary application requests DFU again after a timeout or a restart.
It does not require a timed user action.
During this intermediate state, the tag does not collect measurements.
The procedure preserves the flash history in the host tests.
It cannot preserve samples that the installed firmware keeps only in RAM.
A fresh export includes those samples as a separate copy.

## Wired updates

A wired update also needs verification of its erase range before use.
The existing SES flash script erases all flash and does not preserve history.
A bootloader that protects the complete application data region is another possible long-term change.
Changing only the normal application does not change the existing bootloader's staging rules.
