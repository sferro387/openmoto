# Phase 7: read-only KOMBI service-parameter inspection (2021 BMW R18)

Date: 2026-10-24. Scope: a dedicated, explicitly gated read-only mode to read the
four KOMBI service-related values with ReadDataByIdentifier, establishing the
baseline before any future service-reminder reset. **This is read-only.** No
write (`2E`), security access (`27`), service reset, routine (`31`), session
(`10`) or coding is enabled; those remain firmware-denied. Offline-verified; the
real-motorcycle reads are supervised and operator-approved per request.

Evidence levels: **[APK]** MotoScan 1.110 bytecode; **[FW]** proven from our
firmware/tests; **[BIKE]** measured on the real R18; **[INFERRED]**/**[UNKNOWN]**.

## 1. What this mode adds (and what it does not)

A compile-time flag `OPENMOTO_DIAG_SERVICE_READ` (envs `diag_service_read_dryrun`
/ `diag_service_read`, host `--service-read`) extends the read allowlist with
EXACTLY four manufacturer-specific DIDs, all service `0x22` pure reads:

| Request | DID | MotoScan meaning [APK] |
|---|---|---|
| `22 E1 19` | 0xE119 | odometer-related value |
| `22 E1 2B` | 0xE12B | clock-related value |
| `22 E1 2C` | 0xE12C | next-service-date-related value |
| `22 E1 2D` | 0xE12D | service-distance-related value |

`policyCheck()` tests `serviceForbidden()` first, so this flag cannot open any
write/reset/security/routine path — proven by `tests/test_diag_service_read.py`
(writes and neighbouring DIDs stay denied even with the flag on). The Phase 5
F150 probe build (`OPENMOTO_DIAG_KOMBI_PROBE`) is a separate flag and is
unchanged; default builds still deny all five extra DIDs.

## 2. Transport (reused, verified against the bike in Phase 5) [BIKE][FW]

Same addressing proven by the Phase 5 `22 F1 50` exchange:
`ADDR 6F1 660 60 F1` + `PAD 00` → request frame `60 03 22 E1 xx 00 00 00` on
`0x6F1`; response on `0x660` with extension byte `0xF1`, stripped by the firmware.

## 3. Response framing (expected) [APK][FW]

With BMW extended addressing one CAN frame carries 6 UDS payload bytes (byte 0 is
the `0x60`/`0xF1` address extension). The documented response lengths therefore
split as:

| DID | expected total UDS bytes [APK] | framing |
|---|---|---|
| E119 | 7 (`62 E1 19` + 4) | multi-frame: first frame + 1 flow-control + consecutive |
| E12B | 10 (`62 E1 2B` + 7) | multi-frame |
| E12C | 7 (`62 E1 2C` + 4) | multi-frame |
| E12D | 5 (`62 E1 2D` + 2) | single frame (no flow control) |

The firmware sends at most one bounded flow-control frame (`30 00 02`, BS=0,
STmin) and reassembles consecutive frames with sequence checking. Offline tests
confirm all four (three multi-frame, one single-frame) reassemble correctly.

## 4. Field encodings from MotoScan (investigation; raw bytes are authoritative)

These are how MotoScan's `sl0.r()` **interprets** the bytes [APK]; we record them
to guide reading, NOT to overwrite the raw capture. Until confirmed against the
bike they are [APK]/[INFERRED], not [BIKE].

- **E119 odometer** [APK]: a 4-byte value read as **unsigned 32-bit big-endian**
  (`& 0xFFFFFFFF`), at an offset after the 3-byte `62 E1 19` header. A constant
  `1000` (`0x3E8`) appears in the method — [INFERRED] a km/metre scaling or
  display divisor; the exact application is **[UNKNOWN]** and must be checked
  against the odometer the dash shows.
- **E12C next service date** [APK]: rendered `"%02d.%02d.%04d"` (**dd.mm.yyyy**)
  with placeholder `"--.--.----"`. Byte extraction in `sl0.smali` near line 7956:
  `day = buf[6]`, `month = buf[7]`, `year = (buf[8] << 8) + buf[9]` (**16-bit
  big-endian year**). [INFERRED] the offsets are into MotoScan's own receive
  buffer (which includes framing bytes), so the mapping onto our stripped UDS
  payload must be confirmed from the real response, not assumed.
- **E12B clock** [APK]: 10-byte response (7 data bytes). MotoScan treats it as the
  cluster clock (date/time correction). Exact field split **[UNKNOWN]**.
- **E12D service distance** [APK]: 5-byte response, a **2-byte big-endian short**
  at offset 3 (per docs/diag-kombi-findings.md). Units (km) **[INFERRED]**.

**Rule (task 7):** the tool records and saves the raw `62 E1 xx …` bytes verbatim.
Any decoded value is labelled [INFERRED-from-APK] until a [BIKE] reading confirms
it (e.g. E12C decoding to a plausible future date, E119 matching the dash odo).

## 5. Offline test results [FW]

```
./scripts/pio run -d firmware/mega-can -e diag_service_read            # builds
./scripts/pio run -d firmware/mega-can -e diag_service_read_dryrun     # builds
python3 tests/test_diag_service_read.py                                # PASS
```

`test_diag_service_read.py` (real core vs mock ECU): the four DIDs are allowed
only with the flag and denied by default (host + firmware agree); each uses the
verified frame `6F1 / 60 03 22 E1 xx 00 00 00`; E119/E12B/E12C reassemble via one
flow-control frame, E12D is single-frame; positive responses validate with the
echoed DID; writes/resets/security and neighbouring DIDs stay denied. Phase 5
`diag_kombi_probe` and all prior HEX images are unchanged.

These are offline/software results against a mock ECU. They verify the request
framing, allowlist gating and multi-frame reassembly logic — NOT the real
cluster's values or electrical behaviour.

## 6. Supervised real-motorcycle read procedure

Preconditions: Phase 5 link/probe already succeeded on this bike; ignition ON,
engine OFF; short session. Flash with OBD **disconnected** (B4), then reconnect.

```sh
# Flash (OBD DISCONNECTED):
./scripts/pio run -d firmware/mega-can -e diag_service_read -t upload --upload-port /dev/cu.usbmodem21101
# Boot SILENT, verify, then (OBD reconnected, ignition ON / engine OFF):
ADDR 6F1 660 60 F1
PAD 00
STATUS                 # expect IDLE, addr=SET pad=SET, nothing armed
# Then ONE read at a time, each explicitly confirmed:
ARM 22E12C             # firmware prints ARMED code=XXXX + exact frame
CONFIRM XXXX           # single transmission; prints TRACE + RESULT (raw resp bytes)
# repeat for 22E12D, 22E119, 22E12B — one ARM/CONFIRM each, operator-approved.
```

No automatic transmission: every read requires a fresh `ARM`+`CONFIRM`. `STOP` /
`!` abort to LISTEN_ONLY. Raw `RESULT resp=…` bytes are saved under
`tests/diag_ack/` (or a Phase 7 run dir) per read.

## 7. Safety state

- Read-only: `2E`/`27`/`31`/`10`/`14`/`34-38` all denied (verified). No reset,
  no coding, no adaptation, no security.
- Bounded, one-shot, no retries; auto-return to LISTEN_ONLY; `!`/ESTOP latched.
- `r18-kombi-APK.json` remains `verified=false` (addressing proven functionally
  in Phase 5 but not via electrical B1–B4; the reads are driven through the
  firmware ARM/CONFIRM interface, the real safety authority).
- Still outstanding electrically: B1–B4 unverified (documented, not bypassed).

## 8. Objective / next step

This establishes the **baseline** service values (current next-service date,
distance interval, clock, odometer) needed before any future service-reminder
reset. The reset itself (`2E E1 2B/2C/2D` writes) stays disabled and is a
separate, higher-stakes decision requiring a verified field map, write-path
safety work, and explicit authorization — out of scope here.
