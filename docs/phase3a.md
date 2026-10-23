# Phase 3A — CAN sniffer (CAN1 LISTEN_ONLY)

Status: **PASS ON REAL HARDWARE** (run `20261023T075313Z`). Passive capture,
serial commands, the requested 2 BENCH runs and a further 5 repeatability runs
all passed with zero controller overruns and zero serial drops. Three earlier
firmware revisions failed or only passed under the original, weaker bench
criteria. Their logs are preserved and analyzed below.
Phase 1 (`loopback_8mhz`) and Phase 2 (`physical_dual_8mhz`) sources are
unchanged, and their HEX files still match the verified hashes.

Hardware is the unchanged, verified Phase 2 bench (see [phase2.md](phase2.md)):
Mega 2560, two MCP2515/TJA1050 modules with 8 MHz crystals, CAN1 CS D10 / INT D2,
CAN2 CS D9 / INT D3, shared SPI D50/D51/D52, H–H and L–L, ~55 Ω combined
termination, USB power only. No motorcycle, OBD or external CAN node.

## Environments

| Environment | Source | CAN1 | CAN2 |
|---|---|---|---|
| `sniffer_8mhz` | `src/phase3a/main.cpp`, `OPENMOTO_SNIFFER_BENCH=0` | LISTEN_ONLY | Reset, held in CONFIG (silent); no CAN2 transmit code compiled |
| `sniffer_bench_8mhz` | same file, `OPENMOTO_SNIFFER_BENCH=1` | LISTEN_ONLY | CONFIG; NORMAL only during `BENCH` / `FLOOD` |

Both add `-DSERIAL_TX_BUFFER_SIZE=256` (Arduino core default is 64). The flag also
reaches the framework build; the firmware prints `# serial_tx_free=255` at
startup to confirm it. Serial runs at 115200 baud (`OPENMOTO_SNIFFER_BAUD`), and
SPI at 1 MHz, the same as Phase 2.

```sh
./scripts/pio run -d firmware/mega-can -e sniffer_8mhz
./scripts/pio run -d firmware/mega-can -e sniffer_bench_8mhz
```

## CAN1 never transmits

- The firmware has **no transmit path for CAN1**. The only TX routine
  (`can2Queue`, bench build only) hard-codes CS D9, and the library `sendMessage`
  is never called. The linked `sniffer_8mhz` ELF contains no `sendMessage`,
  `setNormalMode` or loopback symbols.
- Before LISTEN_ONLY is entered, all three CAN1 TXREQ bits are verified clear.
  LISTEN_ONLY is then verified by CANSTAT and CANCTRL readback.
- CAN1 CANSTAT is re-read every 100 ms and before every bench frame. If it is
  ever not LISTEN_ONLY, both controllers are forced to CONFIG, capture stops, an
  `# ALERT` is printed and a reset is required. This never triggered (`mode_faults=0`).
- In LISTEN_ONLY the MCP2515 sends no ACK and no error flags (datasheet DS21801
  §9.3). The TJA1050 is not in a hardware silent mode: silence is enforced only
  by the MCP2515 configuration.

## Reception design

- `can1.reset()` (unmodified library) sets RXM=00 (std+ext), RXB0→RXB1 rollover,
  and all filters and masks to 0, so all frames are accepted. This is verified
  by register readback.
- **INT-driven:** INT on D2 is an active-low level. The loop reads the pin (GPIO,
  no SPI) and uses SPI only while INT is asserted. READ STATUS (2 bytes) decides
  which RX buffer to read; CANINTF is read only for error flags. A FALLING-edge
  ISR only counts `int_edges`. A 100 ms health poll counts flags found while INT
  is high as `int_missed` (measured 0 in every run, so INT D2 delivery is validated).
- **Fast receive path:** one READ RX BUFFER transaction reads the header and only
  DLC payload bytes, and raising CS clears RXnIF. Chip select uses direct port
  writes (D10=PB4, D9=PH6), with the mapping verified at startup.
- **Output decoupled from reception:** the receive path only copies the frame
  into a 64-entry RAM queue. `flushOutput()` formats at most one line per loop
  iteration, and only when a full line (max 43 bytes) fits in the serial TX
  buffer, so it never blocks. If the queue is full, the frame is counted in
  `serial_drop`. Status text waits for TX space while still draining CAN1.
  Queued frames print after the message, so lines never interleave.
- RTR is decoded per the datasheet: SIDL.SRR (standard) and DLC.RTR (extended).
- **Errors:** ERRIF → EFLG. RX0OVR/RX1OVR are counted as `hw_overrun` (each event
  means ≥1 frame was lost inside the controller) and cleared. MERRF is counted
  as `merr`. Acknowledging flags does not change controller error handling.
  Counts are lower bounds, because events between two services coalesce.
  Observed: with BUKT rollover enabled, overflows set **RX0OVR** (EFLG 0x40).
- Timestamps are `millis()` at MCU read, not bus arrival time.

## Serial protocol

```text
F <ms> <id> <S|E> <dlc>[ <payload hex>][ R]
F 10422 123 S 8 4F4D00000000A55A
F 10425 18DAF110 E 8 4F4D00010100A55A
F 10431 321 S 4 R
```

Standard IDs are printed as 3 hex digits and extended IDs as 8. The payload is
`min(dlc,8)` bytes of packed hex. RTR frames carry no payload and end in ` R`.
Status lines begin with `#`; statistics lines begin with `S `.

Commands (case-insensitive, newline-terminated): `START`, `STOP`, `STATS`, `RESET`,
`HELP`, plus `BENCH` and `FLOOD` in the bench build. Capture starts OFF. While
stopped, CAN1 stays LISTEN_ONLY and frames are drained but not counted.

Statistics, printed every 5 s while capturing and on `STATS`:
`rx std ext rtr fps window_ms printed serial_drop hw_overrun dropped_min merr errif
eflg_or tec_peak rec_peak dlc_gt8 queue_peak int_edges int_missed mode_faults capture`.
`fps` covers the window since the previous `S` line.
`dropped_min = serial_drop + hw_overrun`. CAN1 TEC/REC always read 0, because
LISTEN_ONLY deactivates the error counters; they are not a bus-health indicator.

## Serial bandwidth and lost frames (measured)

At 115200 baud the link carries ~11.5 kB/s, i.e. ~330 standard 8-byte lines/s
(~35 B each). A loaded 500 kbps bus carries far more. The FLOOD measurement
(below) received **3,848 frames/s** of an 8-byte frame, but only 446 lines were
printed, including the 64-frame queue drained afterwards. 3,402 frames (88%)
were counted but not printed (`serial_drop`), and 24 overrun events occurred. Count-only, the same flood had
**0 overruns at 3,857 frames/s**. Printing therefore costs both:

1. **Lines:** about 90% of frames cannot be printed on a busy bus. This is
   deliberate and counted; blocking on serial would stall reception.
2. **Some controller loss:** formatting and the serial TX interrupt consume CPU,
   so CAN1's 2 hardware buffers occasionally overflow (24 events/s here).

For the shortest frames (ID 0x7FF, DLC 0), the count-only flood received **7,827
frames/s with 513 overrun events/s**: back-to-back minimal frames exceed CAN1's
receive capacity at 1 MHz SPI. Faster SPI, binary output, a higher baud rate
(e.g. `OPENMOTO_SNIFFER_BAUD=1000000`) or filtering would raise these limits.
None of them has been validated.

## Two-node listen-only bench: prediction vs measured behavior

With only CAN2 (NORMAL) and CAN1 (LISTEN_ONLY), **no node ACKs CAN2's frames**.
This is a property of the bench, not a sniffer fault. Before testing, I
predicted: (1) while error-active, CAN2's active error flag would corrupt each
attempt and CAN1 would discard it; (2) only after 16 attempts (TEC 128,
error-passive) would CAN1 receive copies; (3) CAN2 would stay at TEC 128 and keep
retransmitting without going bus-off.

| Prediction | Measured | Verdict |
|---|---|---|
| (1)/(2) error-active attempts are discarded by CAN1 | First frame: `CAN2_TEC_after_first=16` with **2 copies** (final firmware); 24 with **3 copies** (rev 2/3). Copies = attempts (TEC/8) | **Falsified:** CAN1 stored every attempt, error-active ones included |
| — | `merr=8` on CAN1 in every final run (≈ the 8 frames sent while CAN2 was error-active, 2 attempts each) | Consistent with CAN1 storing the frame *and* flagging a message error when the active error flag follows. Mechanism **unverified** (no oscilloscope) |
| (3) TEC stops at 128, no bus-off | CAN2 TEC=128, EFLG_OR=0x15 (TXEP, TXWAR, EWARN), never TXBO, across 7 bench runs and 3 s of flood (~15,000 unACKed attempts) | **Confirmed** |
| (3) continuous automatic retransmission | FLOOD 8-byte frame: 3,857 copies/s with 0 overruns = 259 µs ≈ 130 bit times per attempt, matching frame + passive error flag + delimiter + intermission + suspend | **Confirmed** |
| No successful transmission | CAN2 CANINTF_OR=0xA0 (MERRF, ERRIF); TX0IF never set | **Confirmed** |

The first frame's first copy arrives ~630 µs after RTS. Later frames take ~336 µs.
The ~300 µs difference is not explained by the data (possibly CAN2 start-up after
entering NORMAL). It does not affect results.

Retransmission is deliberately **not disabled** (no one-shot mode), and
controller error handling is unchanged. The bench requests ABAT on CAN2 as soon
as CAN1 holds a frame, so the in-flight attempt completes and each frame is seen
twice (198–200 of 200 frames). CAN2 is reset at the end, which clears its TEC.

## BENCH command (`sniffer_bench_8mhz`)

1. Reset counters, enable capture, verify CAN1 LISTEN_ONLY and CAN2 CONFIG, then
   set CAN2 NORMAL.
2. Send 25 rounds × 8 frames = **200 frames**, one at a time, through CAN2 TXB0:

   | # | ID | Type | DLC | Notes |
   |---|---|---|---|---|
   | 0 | 0x123 | std | 8 | |
   | 1 | 0x18DAF110 | ext | 8 | |
   | 2 | 0x7FF | std | 0 | max std ID, empty |
   | 3 | 0x1FFFFFFF | ext | 3 | max ext ID |
   | 4 | 0x321 | std RTR | 4 | |
   | 5 | 0x0ABCDEF | ext RTR | 0 | |
   | 6 | 0x000 | std | 1 | min ID |
   | 7 | 0x456 | std | 8 | |

   Payload (first DLC bytes): `4F 4D round index seq_lo seq_hi A5 5A`.
3. For each frame: check CAN1 is LISTEN_ONLY and queue the frame. When CAN1
   holds a frame, request ABAT on CAN2, then verify ID, type, RTR, DLC and payload.
   Wait 5 ms for trailing copies, then drain output. Fail fast on a missing or
   unexpected frame, a safety stop or CAN2 bus-off.
4. Stop CAN2 (ABAT, CONFIG, reset, restore bitrate) and print the report,
   `BENCH: PASS|FAIL` and an `S` line.

**PASS requires:** 200/200 matched, 0 missing/unexpected/timeouts, CAN1 LISTEN_ONLY
throughout, CAN1 TEC/REC 0, **`hw_overrun=0`, `serial_drop=0`, `int_missed=0`**, and
CAN2 stopped in CONFIG. The host validator independently requires the printed
unique frames to equal the deterministic 200-frame sequence exactly, the printed
count to equal `rx` and `copies_total`, and identical sequences across runs.
Duplicates, CAN2 ACK errors / error-passive state and CAN1 MERRF are reported,
not treated as failures.

## FLOOD command (measurement only)

CAN2 sends one unACKed frame and retransmits it back-to-back for 1 s. This runs
three times: 0x123 DLC 8 with printing, 0x123 DLC 8 count-only, and 0x7FF DLC 0
count-only. The command reports rx/s, printed, serial_drop, hw_overrun and CAN2
TEC/REC/EFLG/CANINTF. It is not a pass/fail test; the validator checks only
integrity (frames, phases, LISTEN_ONLY, CAN2 stopped).

## Results (run `20261023T075313Z`, final firmware)

Port `/dev/cu.usbmodem21101`, VID:PID 2341:0042, serial 34530303238351110202,
verified before each upload. Both uploads were flash-verified (11,774 and
17,994 bytes). Summary: [result JSON](../tests/phase3a-result-20261023T075313Z.json)
(includes HEX, source and log SHA-256).

| Test | Result | Key measurements |
|---|---|---|
| T1 passive 30 s (`sniffer_8mhz`) | **PASS** | rx=0, merr=0, hw_overrun=0, serial_drop=0, int_missed=0, mode_faults=0 (silent bus, no spurious frames) |
| T2 commands (`sniffer_8mhz`) | **PASS** | 9/9: HELP, STATS (off/on), START, RESET (zeroed), case-insensitive stop, unknown, overlong |
| T3–T5 BENCH ×2 (requested) | **PASS, PASS** | 200/200 each; 400 copies (2/frame); hw_overrun=0; serial_drop=0; int_missed=0; queue_peak=2; identical sequences |
| BENCH ×5 (repeatability) | **5 × PASS** | 400/400/402/400/400 copies; max 3 copies; hw_overrun=0 in all |
| FLOOD 0x123 DLC 8, print ON | measured | 3,848 rx/s; printed 446; serial_drop 3,402; hw_overrun 24 |
| FLOOD 0x123 DLC 8, count-only | measured | 3,857 rx/s; hw_overrun 0 |
| FLOOD 0x7FF DLC 0, count-only | measured | 7,827 rx/s; hw_overrun 513 (above capacity) |

Logs: `tests/phase3a-{sniffer,bench}-upload-20261023T075313Z.log`,
`tests/phase3a-{passive,commands,bench,repeat,flood}-{serial,capture}-20261023T075313Z.log`.

## Investigation history (all logs preserved)

| Run | Firmware (HEX SHA-256 prefix) | Outcome | Root cause / change |
|---|---|---|---|
| `073111Z`, `073153Z` | sniffer `37841e96` | T1, T2 PASS | Initial implementation (superseded) |
| `073909Z` | bench `50479639` | Firmware `BENCH: PASS` ×2 and validator accepted, **but** hw_overrun 540/549, serial_drop 25, ~10 copies/frame | (a) The bench aborted CAN2 only after `serviceCan1()` returned, after ~8 copies. (b) Per-frame CPU cost ~675 µs (32-bit division in decimal formatting; formatting even when serial was full). The validator skipped the sequence check when serial_drop>0, which was **too weak**. Fixed: ABAT from the receive path, division-free formatting, room check before formatting, `hw_overrun`/`serial_drop` added to the PASS criteria, and unconditional sequence checks |
| `074449Z` | sniffer `bec8a43a`, bench `6c29c4a9` | T1/T2 PASS; BENCH **FAIL ×2** (hw_overrun 1 each) | ~3 copies/frame. Formatting/queuing still in the receive path; short frames (~112–128 µs retransmit) overran when a 4th copy arrived. Fixed: RAM output queue, so the receive path does SPI only |
| `074927Z` | sniffer `98952db9`, bench `850277e9` | T1/T2 PASS; BENCH PASS, **FAIL** (hw_overrun 1) | Marginal: recopy interval 168 µs vs receive cost. Fixed: direct-port CS, READ STATUS dispatch, ABAT requested as soon as CAN1 holds a frame (before reading it) |
| `075313Z` | sniffer `8fbca2d8`, bench `185b02aa` | **All PASS** (see Results) | Final |

The first time this failed (`073909Z`), the failure was hidden by the
firmware's original PASS criteria. The criteria were then **strengthened**, and
the strengthened validator rejects that log. No assertion was relaxed. Build
logs: `tests/phase3a-build-*.log` (two superseded warning-only builds that were
never uploaded were not kept).

## Limitations of the two-node listen-only bench

- No ACKing node: every CAN2 frame ends in an ACK error and is never
  successfully transmitted. A real bus has ≥2 NORMAL nodes that ACK each other.
- Frame counts on CAN1 ≠ frames "sent": duplicates come from retransmission. The
  bench validates the set and order of unique frames, not copy counts.
- CAN1's acceptance of error-active attempts (and the MERRF that accompanies it)
  is observed but not explained at the bit level. On a healthy bus with ACKing
  nodes, this situation does not arise.
- CAN1 error counters are deactivated in LISTEN_ONLY, so TEC/REC = 0 says nothing
  about bus health.
- BENCH traffic is paced. FLOOD shows that sustained back-to-back short frames
  exceed capacity at 1 MHz SPI, and printing loses ~90% of lines on a busy bus.
- Both controllers share one MCU, SPI bus and crystal type, so bit-timing
  tolerance between independent oscillators is not exercised.
- No vehicle, OBD, ISO-TP, UDS or connector validation. A sniffer that passes
  on this bench is not a validated vehicle logger.

## Reproduce

```sh
./scripts/pio device list    # confirm the Mega (2341:0042) and its port
./scripts/pio run -d firmware/mega-can -e sniffer_8mhz -t upload --upload-port USB_PORT
host/python/.venv/bin/python host/python/capture_phase3a.py --port USB_PORT --seconds 30 --output tests/phase3a-passive-serial-<UTC>.log
host/python/.venv/bin/python host/python/capture_phase3a.py --port USB_PORT --commands --output tests/phase3a-commands-serial-<UTC>.log
./scripts/pio run -d firmware/mega-can -e sniffer_bench_8mhz -t upload --upload-port USB_PORT
host/python/.venv/bin/python host/python/capture_phase3a.py --port USB_PORT --bench --bench-runs 2 --output tests/phase3a-bench-serial-<UTC>.log
host/python/.venv/bin/python host/python/capture_phase3a.py --port USB_PORT --flood --output tests/phase3a-flood-serial-<UTC>.log
```

Phase 3A uploads require explicit user approval
(`.kiro/steering/hardware-safety.md`). Run long sessions under `caffeinate -i`:
the bench Mac idle-slept on battery during one run, which delayed (but did not
corrupt) the `073909Z` session.
