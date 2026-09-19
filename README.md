# TEF6686 ESP-IDF Test Firmware

A minimal ESP-IDF (ESP32-S3) driver and test harness for the NXP TEF6686
FM/AM world-band tuner IC, built around a custom hand-soldered PCB (chip
transplanted from a donor module via hotplate reflow).

This started as a bring-up exercise for a 12 MHz-crystal TEF6686 board and
turned into a fairly thorough reverse-engineering/verification pass against
the official NXP TEF668X User Manual — a handful of real bugs were found
and fixed along the way, documented below in case they save someone else
the same debugging session.

## Hardware

- **Chip:** TEF6686 (F8605 die marking), transplanted from a donor module
- **Crystal:** 12.000 MHz (stock module used 9.216 MHz — see "Reference
  clock" below if you're doing the same swap)
- **MCU:** ESP32-S3 (reused ESP32-S3+TFT PCB from a companion DAB radio
  project)
- **I2C:** SDA = GPIO18, SCL = GPIO17, 100 kHz, address `0x64`
- **FM antenna:** 1:1 balun feed
- **AM antenna:** 8" ferrite rod (works well on MW; see notes on SW below)
- **Patch:** Lithio V102 (`patch_v102[]`), sourced from the
  PE5PVB/TEF6686-remastered project's patch tables, uploaded on every boot
  since the chip has no non-volatile patch storage

## Building

Standard ESP-IDF v5.x project layout.

```
idf.py set-target esp32s3
idf.py build flash monitor
```

Serial console runs at 115200 baud.

## Usage

Type a frequency and press Enter to tune. Type `FM` or `AM` on their own
line to switch bands — each has its own native units:

- **FM:** 10 kHz units (e.g. `10660` = 106.60 MHz), range 6400–10800
- **AM:** plain kHz (e.g. `999` = 999 kHz MW, `15330` = 15330 kHz SW),
  range 144–27000 (combined LW/MW/SW)

The quality line prints once a second:

```
[FM  90.00 MHz] RSSI:+36.6 dBuV | USN: 30 | WAM: 48 | Offset:-4.2 kHz | BW:236 kHz | MOD: 42 kHz | STEREO
[AM   693 kHz]  RSSI:+58.7 dBuV | Noise: 63 | CoChan:  1 | Offset:+0.0 kHz | BW:  4 kHz | MOD: 34%
```

## Bugs found & fixed vs. the reference sources

None of these were exotic — all were confirmed against the official NXP
TEF668X User Manual (Rev 1.6) I²C command reference. Listed in case they're
useful to anyone porting from the same community reference code.

### 1. Reference clock calculation for a non-stock crystal
`APPL_Set_ReferenceClock` (module `0x40`, cmd `4`) takes the crystal
frequency in Hz split into two 16-bit big-endian words plus a type field.
For 12 MHz:

```
12,000,000 = (0x00B7 << 16) | 0x1B00
[ w 40 04 01 00B7 1B00 0000 ]
```

This matches the manual's own worked example exactly — useful confirmation
if you're changing crystal frequency on your own board.

### 2. Stereo pilot bit was wrong
`Get_Signal_Status` (FM cmd `133`) returns a single 16-bit status word.
The pilot-detected flag is **bit 15** (`0x8000`), not bit 8 — an earlier
version of this code checked the wrong bit and reported "mono" almost
regardless of actual pilot lock. Confirmed against the manual's own I²C
example (`r 8000` = "stereo signal found").

### 3. `Set_OperationMode` polarity
`Set_OperationMode` (APPL cmd `1`): **0 = normal operation, 1 = standby**
(no RF). An earlier version passed `1` intending "normal operation" — the
opposite of what the manual defines. It didn't visibly break anything
because a subsequent `Tune_To` call automatically promotes the chip out of
standby, but the parameter now matches its actual documented meaning.

### 4. `Get_Quality_Status` field offset
`Get_Quality_Status` (FM/AM cmd `128`) returns **7** 16-bit words —
`status, level, usn/noise, wam/co_channel, offset, bandwidth, modulation`
— not 6. An earlier read only requested 12 bytes and mis-indexed into the
buffer as a result. Now reads all 14 bytes with the full field set
(including the modulation-depth field), matching the manual's layout.

### 5. De-emphasis constant
`Set_Deemphasis` (FM cmd `31`) takes the time constant in units of 0.1 µs,
not µs. `500` = 50 µs (the correct European default) — a value of `50`
(intended as "50 µs") is actually an undocumented 5 µs setting.

### 6. Tune mode
`Tune_To` mode parameter: `1` = **Preset** (short mute, for a deliberate
user retune — what this firmware does), `4` = **Jump** (inaudible mute,
intended for background AF-list checking, e.g. RDS alternative-frequency
switching). Mode `1` is correct for a manually-entered frequency.

## AM/SW audio VU meter via I2S

The TEF6686 can output its demodulated audio as digital I2S instead of (or
alongside) the analogue DAC output:

```
AUDIO cmd 22 Set_Dig_IO(signal=33 [IIS_SD_1], mode=2 [output],
                         format=16 [I2S 16-bit], operation=256 [master],
                         samplerate=4410 [44.1kHz])
```

With `operation=256` the TEF6686 generates its own BCK/WS, so the host MCU
is a plain I2S receiver — no clock generation needed on the ESP32 side.
The output source defaults to the audio processor (post volume/mute), so
this feed matches what actually reaches the speaker. Used here to drive a
reused LVGL bargraph VU meter (originally built for a companion DAB radio
project) — verified working into an ES9023 DAC before wiring to the
ESP32-S3's I2S RX peripheral.

## AM/SW sensitivity: a note for anyone considering this chip for DX/ham use

Head-to-head against a Si4735-based receiver (Tecsun PL-300) on a 13.635 MHz
shortwave broadcast, the Tecsun's own short whip received the station
clearly while this TEF6686 board struggled with both a long outdoor wire
and an 8" ferrite rod.

The likely reason is architectural rather than a fault on this board: the
manual's *only* AM antenna command is `Set_Antenna` (AM cmd 12), which is
purely an **attenuator** for taming an over-strong active antenna — there
is no equivalent to the Si4735's tracked/tuned antenna input (its ATC),
which provides real passive RF gain and selectivity right at the tuned
frequency. The TEF6686's AM/SW path is a wideband, untracked front end
that depends on the antenna to already deliver a workable signal; it adds
no preselection gain of its own. The TEF668x family also has no SSB/BFO
capability at all (confirmed: zero mentions in the manual), so it's not a
candidate for ham-band or utility-station listening regardless of
antenna.

**Conclusion:** this chip is a good, well-behaved **FM/AM broadcast-band**
receiver — stereo FM performance is solid and comparable to other chips in
its class once the bugs above are fixed. For shortwave DX or SSB/ham work,
a tracked-front-end part with SSB support (e.g. Si4735, tested separately
and preferred for that purpose) is the better investment of time.

## Credits / references

- NXP **TEF668X User Manual**, Rev 1.6 (17 Feb 2015) — the authoritative
  source for every command/register decoded above
- **PE5PVB/TEF6686-remastered** and the wider PE5PVB TEF6686 project
  ecosystem — patch table source and general community reference
- Crystal option table (4 / 9.216 / 12 / 55.46667 MHz) per the same
  project's hardware notes

## License

Add a license of your choice here before relying on this for anything
beyond personal/hobby use.
