# vlongmem variant: environmental logger for one year

The `vlongmem` variant of `ruuvi.firmware.c` is for RuuviTag B (nRF52832,
S132 6.1.1). It keeps approximately one year of temperature, humidity and
pressure history in the internal flash. It sends the history through the
standard Ruuvi GATT log-read protocol. Thus the unmodified Ruuvi Station
apps can read the history, in the limits given in "Client findings".

## Decisions

| Item | Decision |
|---|---|
| Fields | temperature, humidity, pressure |
| Interval | 10 min |
| Resolution in flash | 0.01 deg C, 0.01 %RH, 1 Pa (the same as the wire format, thus no loss of data) |
| Ring behavior | ring-wrap, the oldest block is erased when the ring is full |
| Write policy | samples are kept in RAM and written after 16 samples (a maximum of 160 min of data is lost at a sudden power loss) |
| Host access | standard Nordic UART Service log-read protocol only, no custom reader |
| Apps | official Ruuvi Station, app changes are a later decision |
| Gateway | not applicable, the gateway cannot read tag logs |

## Measured numbers

These values come from a build of the `default` variant at commit 7e6ca34.

| Item | Value |
|---|---|
| Application code region (`ruuvitag_b.ld`) | 0x26000 to 0x75000, 316 KB, shared with storage |
| Standard `.storage_flash` section | 0x60000 to 0x75000, 84 KB |
| FDS pages in that section (settings) | the top 17 pages, 0x64000 to 0x75000 |
| Application size (text + data) | 102,452 B |
| SoftDevice | S132 6.1.1, 0x0 to 0x26000 |
| Bootloader | Ruuvi 3.1.0, 0x75000 to 0x7E000, settings at 0x7F000 |
| Standard log format | 16 B for each sample, 14 records of 250 samples, approximately 12 days at 5 min |

### vlongmem flash layout

| Region | Address | Size |
|---|---|---|
| Application code | 0x26000 to 0x44000 | 120 KB |
| Log ring (raw pages) | 0x44000 to 0x72000 | 184 KB, 46 pages |
| FDS (settings, boot counter) | 0x72000 to 0x75000 | 12 KB, 3 pages |

Each page of the log ring holds one block. A block has a 32 B header and
4064 B of sample data. The usual sample is 3 B. Thus one page holds
approximately 1354 samples, or 9.4 days at 10 min. The 46 pages hold
approximately 430 days before escapes are counted. Refer to `FORMAT.md` for
the exact layout.

The `vlongmem` build at the same commit has these values:

| Item | Value |
|---|---|
| Application size (text + data) | 103,724 B |
| Last used application address (end of the `.data` load image) | 0x3F530 |
| Margin to the log ring | 19 KB |
| Static RAM (data + bss) | 8,464 B, the standard variant uses 16,584 B |

The static RAM is smaller because the two 4 KB log blocks of the standard
logger are replaced with a 148 B buffer.

## Client findings

This is what the official clients do with the log-read protocol. The source
of the data is the GitHub code of Ruuvi Station for Android
(`com.ruuvi.station`, GATT code in `com.ruuvi.bluetooth.default`), Ruuvi
Station for iOS (`com.ruuvi.station.ios` with `BTKit`), and the gateway
(`ruuvi.gateway_esp.c` and `ruuvi.gateway_nrf.c`).

The protocol comes from `ruuvi.endpoints.c`. A message has 11 bytes:
destination, source, operation and an 8 B payload. A log read is operation
0x11 to destination 0x3A. The payload has the current time of the phone and
the start time, as big-endian u32 seconds. The tag replies with one
operation 0x10 message for each field of each sample. The reply has the
timestamp as u32 seconds and the value as a big-endian i32. Temperature and
humidity are multiplied by 100, pressure is in Pa. A payload of all 0xFF
stops the stream, operation 0xE0 cancels it. The protocol has no limit on the
number of samples.

Limits that were found:

- **Android**: a constant in the code sets the start time to a maximum of 10
  days before now. Each stored value older than 10 days is erased when an
  advertisement comes in. Samples are matched by a linear scan of a list in
  memory, thus a very large read is slow. A disconnection in the transfer
  discards all received data. A humidity or pressure value of 0xFFFFFFFF is
  null. The log read is only possible with firmware 3.28.12 or newer.
- **iOS**: a stored setting sets the start time to a maximum of 10 days
  before now, and older rows are erased. The full transfer must complete in
  the service timeout, 60 s by default, measured from the request. The app
  makes a record only when temperature, humidity and pressure all come in for
  a sample. Thus a tag that records less than three fields gives no records
  on iOS.
- **Gateway**: the gateway does not connect to a tag. GATT is not compiled
  into its nRF52811 firmware and Bluetooth is off on its ESP32. The gateway
  only sends raw advertisements to the cloud. Its "history" endpoint is the
  table of recent advertisements, not tag logs.
- **Standard tag firmware**: a log read that continues for more than 5 min
  stops (`APP_HEARTBEAT_OVERDUE_INTERVAL_MS`). The watchdog resets the tag
  after 6 min, because the read loop blocks the sample task and the watchdog
  task. The standard firmware also erases its log at each boot.
- The apps do not send GATT history to Ruuvi Cloud. The Android app can
  send it to a URL that the user sets.

Result: an unmodified app reads only the last 10 days in each transfer, and
the phone erases older data. A read of a full year is possible only with
changes to the apps. Android: the 10-day constant, the erase call and the
sample match. iOS: the service timeout, the erase setting and the
three-field rule. The tag sends each range that the client asks for, without
the 5 min stop.

## Behavior with fast changes

The encoding has no loss of data. Each field is a signed one-byte delta from
the previous sample when the change is in the range of +/-1.27 deg C,
+/-1.27 %RH or +/-127 Pa for each interval. When the change is larger, the
field is an escape byte and the absolute value in two bytes. The decoder then
sets its reference to that absolute value. Thus a fast change uses more
flash, not less precision. A sample with three escaped fields uses 9 bytes,
not 3.

Example: a bicycle in storage makes almost only 3-byte samples. On a
transport through a storm and a tunnel, the pressure can change by some
hundred Pa in 10 min and the humidity by tens of percent. Those samples use
9 bytes for the time of the event. An event of 24 hours uses a maximum of
144 * 6 = 864 more bytes, approximately one fifth of one page. The retention
decreases only by the bytes that are used. The values stay accurate at the
stored resolution. The rate of change never makes the data incorrect.

Two limits stay. A missing or failed sensor reading is written as an escape
with a sentinel value, and the field is invalid for that sample. The time of
the sample does not move. The pressure is kept as Pa minus 50000 in 16 bits,
thus absolute values below 50000 Pa (above approximately 5500 m) are limited
to 50000 Pa.

## Time

The tag has no clock. The time of each sample is implicit. The block header
holds the uptime of the first sample of the block. The samples are on a fixed
grid of 10 min from that point. A slot without a sample (for example, during
a GATT transfer that blocks the scheduler) is written as a missing marker.
Thus the grid does not move.

The phone gives the absolute time. Each log-read request has the current
time of the phone. The variant keeps that time and the tag uptime as a time
anchor for the current boot session, in the settings area of the flash. It
also writes the epoch offset into the header of each block of that session
that has no offset. A block from a session that never connected to a phone
has no date, and the tag does not send it. Thus the installation procedure
has one step after the battery is installed: connect one time with the app,
and the time is recorded.

### Clock drift

The uptime counter runs from a 32.768 kHz crystal. The SoftDevice
configuration declares it as 20 ppm. A tuning-fork crystal is slower when it
is cold, by approximately 0.034 ppm for each squared degree from 25 deg C.
Typical values for one year:

| Condition | Rate | Error in one year |
|---|---|---|
| Tolerance at 25 deg C | 20 ppm | 10.5 min |
| 5 deg C | 14 ppm slow | 7 min slow |
| -10 deg C | 42 ppm slow | 22 min slow |
| -25 deg C | 85 ppm slow | 45 min slow |

The anchors correct this. When a session has two or more anchors, the time
of a sample is a linear interpolation between the two nearest anchors, or an
extrapolation with the rate of the nearest pair. The times are calculated
when the log is read, thus a later connection also corrects the samples
before it. With one connection at the start and one at the end, the error in
the period between them is some seconds. With one anchor, the time is the
anchor offset and the drift is not corrected.

The tag keeps a maximum of 16 anchors, a maximum of 4 for each session. A
connection less than 1 hour after the last anchor replaces that anchor. A
phone time that shows a rate error of more than 1 % against the first anchor
of the session is not accepted, and a phone time before 2020 is not
accepted. A session without anchors uses the header offset. Refer to
`FORMAT.md` for the record layout.

A reboot always starts a new block and a new session. The gap between the
last sample before the reboot and the first sample after the reboot is not
known.

## Connection

The variant keeps the standard radio settings of the `default` variant. The
tag sends a connectable advertisement each 1285 ms (`APP_BLE_INTERVAL_MS`)
and reads the sensors each 2570 ms (`APP_NUM_REPEATS` 2). The advertisement
has the usual sensor data. Thus the tag shows in the scan list of Ruuvi
Station and nRF Connect the same as a standard tag.

To read the history, open the sensor in Ruuvi Station and start the history
synchronization. The app connects through GATT and does the log read. A
button press on the tag is not necessary for the log read, the same as with
the standard firmware. The first connection after a battery change also sets
the absolute time of the log, refer to "Time".

## Build

The toolchain is in a rootless podman image, refer to `scripts/podman/`.

```
scripts/podman/build.sh VARIANTS=vlongmem        # ruuvitag_b, vlongmem only
scripts/podman/build.sh                         # all variants
scripts/podman/build.sh -- make -C targets/ruuvitag_b/armgcc clean
```

The output goes to `src/targets/ruuvitag_b/armgcc/` as
`ruuvitag_b_armgcc_ruuvifw_vlongmem_<version>_{app.hex,full.hex,dfu_app.zip}`.
The DFU zip goes to the tag through the air with nRF Connect and the
standard Ruuvi bootloader. The full hex is for a wired programmer. The
bootloader and the SoftDevice do not change, thus a DFU back to the standard
firmware is possible.

Note: the CI workflow gets the SDK and the nRF command line tools from
`storage.ruuvi.com`. That name did not resolve in DNS when the image was
made. The image gets the same versions from the Nordic URLs.

## Differences from the handoff specification

- The interval is 10 min, not 5 min, and there are three fields, not one.
  Thus iOS makes records, and one year of data is possible.
- There is no custom host decoder and there are no custom NUS commands. The
  standard log-read protocol is the interface. Thus the official apps
  operate without changes, in their own limits.
- The block header has no count field. The number of samples in a block
  comes from a decode of the data to the first 0xFF byte at a sample
  boundary. A delta of -1 is never written as 0xFF, it goes through the
  escape. Thus the 0xFF byte is not ambiguous.
- The absolute time procedure above is new. The handoff did not include it.

## Open items

- App changes for a read of a full year, a later decision.
- Samples in a long GATT transfer are recorded as missing. A sample from
  the transfer loop would close that gap.
- Power: the variant keeps the standard advertisement rate. A slower
  advertisement rate and no accelerometer would increase the battery life.
