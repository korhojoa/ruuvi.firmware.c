# Interval settings

The advertisement and storage intervals change through Bluetooth.
The tag saves both settings in flash across restarts.
The defaults are 1285 ms for advertisements and 600 s for storage.

The standard Bluetooth update can overwrite existing history during staging.
Refer to [Update compatibility](UPDATE.md) before a firmware update.

## Settings

| Setting | Unit | Range |
| --- | --- | --- |
| Advertisement interval | ms | 100 to 10000 |
| Storage interval | s | 1 to 65535 |

The storage interval must be at least twice the advertisement interval.
The sensor refresh interval is twice the advertisement interval.
This limit makes sure that each storage slot has a new measurement.
The initial fast advertisements stay at 100 ms for five seconds.
The saved interval applies after this initial period.

A storage interval change closes the current block and saves its pending samples.
The next measurement starts a new block with the new interval.
Older blocks keep their original interval and timestamps.
An advertisement-only change does not restart the storage grid.
A failed write returns an error and keeps the effective settings.
The firmware checks the saved record before it applies the settings.
An unchanged setting does not cause a flash write.
A missing or invalid record uses the defaults.
A factory reset removes the saved settings and the history.

A shorter storage interval reduces retention.
The RAM buffer holds at most 16 samples before a flash write.
A sudden power loss can thus lose 16 times the configured storage interval.
The default limit is 160 minutes.

## Bluetooth protocol

The protocol uses the existing NUS service and standard 11-byte messages.
The destination and source are both the system configuration endpoint, `0x22`.
The operation is `0x07` for a read and `0x06` for a write.
The payload contains two unsigned 32-bit integers, with the most significant byte first.
The first integer is the advertisement interval in milliseconds.
The second integer is the storage interval in seconds.
A read request has a zero payload.

A successful reply uses operation `0x06` and contains both effective settings.
A failed request uses `0xEE`.
An unauthorized write uses the existing `0xEA` reply.
A write requires the existing configuration mode.
A short button B press enables this mode for the next connection.
A read does not require configuration mode.
The standard history protocol stays the same.
Stock Ruuvi Station apps do not provide controls for these settings.

## Android app

The VLM Android app adds Measurement intervals to the sensor settings.
The entry appears only after a valid settings response from the tag.
The fields stay empty and disabled until the app reads their values.
Save checks the acknowledgement and a second read before it reports success.
The page explains the short button B press required for a write.

## Host test helper

The helper requires Linux, BlueZ, dbus-python and GLib, the same as `nus_logread.py`.
The examples use a placeholder device address.
The settings require firmware with this feature. Older firmware cannot answer the request.

Read the settings:

```sh
/usr/bin/python3 -B test/tools/nus_settings.py <MAC>
```

Set one or both intervals:

```sh
/usr/bin/python3 -B test/tools/nus_settings.py <MAC> --announce-ms 5000 --storage-s 1800
```

The helper reads the current settings before a change.
An omitted option keeps its current value.
The helper checks the reply and reads the settings again after a write.

## Checks

`scripts/sim-vlongmem.sh --settings` checks the real settings and log modules against simulated flash.
The scenario checks mixed intervals, restarts, failed writes, stale readback, invalid records and bounded flash waits.
`scripts/sim-vlongmem.sh 800` checks long-term operation, ring wraps and power cuts.
`/usr/bin/python3 -B test/tools/test_nus_settings.py` checks the host helper without Bluetooth.

Host tests do not measure radio timing or battery savings.
The firmware needs a device test after installation.

The settings firmware passed 171 Ceedling tests, the settings simulation and an 800-day simulation.
The default, fake-data and real logger variants compiled with the Nordic toolchain.
The real application has 106168 bytes and a 16712-byte margin before the log ring.
The DFU payload matches the application HEX.
No device update was part of these checks.
