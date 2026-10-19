# BUG: UBX frame checksum is wrong — every UBX command is silently dropped

## Location
- `firmware/src/gps.c` → `ubx_send()`
- Buggy line (at time of writing, ~line 452): `for (int i = 0; i < k; i++) { a += buf[i]; b += a; }`
- The fixed version already exists in `PlatformIO/Projects/NEO-M9N/src/gps.c` and can be used as a reference diff.

## Symptoms
- **All UBX binary commands to the NEO-M9N had no effect.**
  - `UBX-MON-VER` poll: no reply (`gps_dump` shows `VER -` / `VER=(none)`).
  - `UBX-NAV-SAT` / `UBX-MON-HW`: no reply (`have_navsat=0`, `ANT=0`, `JAM` stays 0).
  - `UBX-CFG-MSG`, `UBX-CFG-RATE`, `UBX-CFG-PRT`, `UBX-CFG-GNSS`, `UBX-CFG-RST`: all silently ignored.
- **NMEA / PUBX commands (`$PUBX,...`) worked normally**, e.g. `$PUBX,40,GLL,0,0,0,0,0,0` stopped GLL output.
- This previously led to the wrong conclusion "UBX writes don't reach the module / the module does not support UBX". That is **incorrect**.

## Root cause
The UBX 8-bit Fletcher checksum is computed over **class, id, lenL, lenH, payload** and **must NOT include the `0xB5 0x62` sync bytes**. The current code starts at `i = 0`, so it folded the sync header into the checksum. Every UBX frame therefore had a bad checksum and the module discarded it without any error (UBX is silent on bad checksum).

## Buggy code (gps.c)
```c
uint8_t a = 0, b = 0;
for (int i = 0; i < k; i++) { a += buf[i]; b += a; }          /* WRONG: includes 0xB5 0x62 */
for (int i = 0; i < n; i++) { buf[k++] = payload[i]; a += payload[i]; b += a; }
buf[k++] = a; buf[k++] = b;
```

## Fix (start at `i = 2`)
```c
uint8_t a = 0, b = 0;
/* UBX checksum covers class..payload, NOT the 0xB5 0x62 sync bytes */
for (int i = 2; i < k; i++) { a += buf[i]; b += a; }          /* OK: skip sync header */
for (int i = 0; i < n; i++) { buf[k++] = payload[i]; a += payload[i]; b += a; }
buf[k++] = a; buf[k++] = b;
```
At that point `k == 6`, so `i = 2..5` covers `cls, id, lenL, lenH`; the payload loop and the checksum tail are unchanged.

## Verification (done on the separate NEO-M9N board/project)
After the fix, on the same module:
- `UBX-MON-VER` → `VER = ROM CORE 4.04 (d964f4)` (genuine u-blox).
- `have_navsat = 1`, `ANT = 1`, `pwr = 2`, `JAM` populated (NAV-SAT / MON-HW answered).
- `UBX-CFG-RATE` works: 10 Hz and 25 Hz output confirmed
  (NMEA rate went from ~10 sentences/s to ~110/s at 10 Hz and ~225/s at 25 Hz).
- `UBX-CFG-PRT` (baud change) and `UBX-CFG-MSG` (e.g. disable GLL) take effect.
- Practical note for 10/25 Hz: raise the UART baud (e.g. 230400), and trim/decimate NMEA
  (disable GLL/VTG, emit GSV every 2nd epoch) so the stream fits. With 4 concurrent
  constellations, 25 Hz is harder to get a fix on — limiting to GPS+BeiDou via
  `UBX-CFG-GNSS` usually helps.

## Impact
Every feature that calls `ubx_send()` was a silent no-op before this fix, while still
logging "sent" (that only reflects a successful `uart_write_bytes`):
`gps_begin` / `gps_begin_i2c` MON-VER poll, `gps_poll` NAV-SAT/MON-HW polls,
`gps_cfg_gps_bds`, `gps_disable_gll`, `gps_restart`, and the I2C branch of `gps_next_baud`.

## Suggested commit message
```
gps: fix UBX checksum (exclude 0xB5 0x62 sync bytes in ubx_send)

The Fletcher checksum must start at the class byte; including the sync
header made every UBX frame invalid, so the module silently dropped all
UBX polls/config (MON-VER/NAV-SAT/MON-HW/CFG-*). PUBX/NMEA was unaffected,
which masked the issue. After the fix UBX-MON-VER answers and CFG-RATE/
CFG-PRT/CFG-MSG take effect.
```

## Notes
- Any other copy of `gps.c` derived from this one has the same bug and must be fixed too.
- NMEA/PUBX was always fine (different checksum), which is why PUBX could be used as a control.
