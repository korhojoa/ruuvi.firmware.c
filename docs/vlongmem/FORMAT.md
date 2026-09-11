# vlongmem flash log format, version 1

The code is in `src/app_log_vlongmem.c` (ring, headers, time) and
`src/app_log_vlongmem_codec.c` (sample encoding). All multi-byte integers are
little-endian. Erased flash reads as 0xFF.

## Region

| Item | Value |
|---|---|
| Start | 0x44000 (`APP_VLONGMEM_REGION_START`) |
| End | 0x72000 (`APP_VLONGMEM_REGION_END`) |
| Page size | 4096 B |
| Pages | 46 |

Each page holds one block. The block with the sequence number `seq` is on
page `seq % 46`. Sequence numbers start at 1 and always increase. Thus the
newest block is the valid header with the highest `seq`. When the ring is
full, page `seq % 46` is erased before the next block is written.

## Block header, bytes 0 to 31 of the page

| Offset | Size | Field | Notes |
|---|---|---|---|
| 0 | 4 | magic | 0x4D4C5652, the bytes `R V L M` |
| 4 | 4 | seq | block sequence number, not 0 and not 0xFFFFFFFF |
| 8 | 4 | boot_id | boot counter of the session that wrote the block |
| 12 | 4 | start_uptime_s | tag uptime in seconds at the first sample slot of the block |
| 16 | 4 | epoch_offset_s | epoch seconds minus uptime seconds for this boot session, 0xFFFFFFFF when not known |
| 20 | 2 | interval_s | period of the sample grid, seconds |
| 22 | 2 | version | 1 |
| 24 | 8 | reserved | 0xFF |

A header is valid when the magic and the version are correct and `seq` is
not 0 and not 0xFFFFFFFF.

The field `epoch_offset_s` is the only field that is written after the
header. It is 0xFFFFFFFF until a phone does a log read. Then the offset of
the current session is written into each header of that session that holds
0xFFFFFFFF. The nRF52832 flash lets you write a 32-bit word two times
between erases. This is the second write.

The absolute time of sample `i` in a block is:

```
epoch_s = epoch_offset_s + start_uptime_s + i * interval_s
```

A block with an unknown `epoch_offset_s` from a session that is not the
current session has no date. The tag does not send it.

## Sample data, bytes 32 to 4095 of the page

The data is a sequence of samples and then erased flash. Each sample has
three fields in a fixed order. The samples are written in groups of full
samples. Each group has 0xFF padding to a 4-byte boundary. The decode rule at
the start of a sample is:

- Byte 0xFF at an offset that is not a multiple of 4: this is padding, the
  decoder continues at the next multiple of 4.
- Byte 0xFF at a multiple of 4: this is the end of the data in this block.

A 0xFF byte at the start of a field in a sample shows that a power loss cut
the sample. The decoder stops there.

### Fields

| Index | Field | Unit in flash | Absolute encoding | Missing sentinel |
|---|---|---|---|---|
| 0 | temperature | 0.01 deg C | int16 | 0x8000 |
| 1 | humidity | 0.01 %RH | uint16, 0 to 65534 | 0xFFFF |
| 2 | pressure | 1 Pa, minus 50000 | uint16, 0 to 65534 | 0xFFFF |

### Field encoding

Each field has one of two forms:

- **One byte**: a signed 8-bit delta `d` from -127 to +127, but not -1.
  The field value is `reference + d`.
- **Three bytes**: the escape 0x80 and then the absolute value as 16-bit
  little-endian. If the absolute value is the missing sentinel of the field,
  the reading is missing and the reference does not change. If not, the
  field value is the absolute value and the reference becomes that value.

The reference of each field is 0 at the start of each block. After a
one-byte delta, the reference becomes the decoded value. The encoder writes
a delta when the change is in the range and is not -1 (0xFF) or -128 (the
escape). If not, the encoder writes the absolute value.

Result: a sample uses 3 to 9 bytes. The first sample of a block usually uses
9 bytes. No data is lost at any rate of change. A fast change only uses more
bytes. Values out of the absolute range are limited to the range. For
pressure, this applies to readings below 50000 Pa.

## Sample grid

The samples are on a fixed grid of `interval_s` from `start_uptime_s`. A
slot without a sample (for example, during a GATT log read that blocks the
scheduler) is a sample with three missing fields. Thus the later slots keep
their time. When more than `APP_VLONGMEM_MAX_MISSING` slots (144, one day)
are missing in sequence, the block is closed. The next sample opens a new
block with a new `start_uptime_s`.

A block is closed and a new block is opened when the next sample does not
fit in the page, and at each boot. The gap across a reboot is not known.

## Write policy

The samples stay in RAM until `APP_VLONGMEM_FLUSH_SAMPLES` (16) samples are
in the buffer, or until the block is closed. Then the buffer is written to
flash. A maximum of 16 samples is lost at a sudden power loss. Each write is
one word-aligned operation. No word is written two times, with the exception
of `epoch_offset_s` as given above.

## GATT transfer

The log-read handler in `src/app_sensor.c` gives the current time of the
phone to `app_log_time_set()`. Then it reads the blocks from the oldest to
the newest, and the samples in each block. It does not send samples that are
older than the start time of the request. It sends one message for each
valid field of each sample, with the epoch timestamp calculated above. It
does not send the missing fields of a sample. The standard 5 min stop is
removed. The loop puts a signal to the watchdog at each sample.
