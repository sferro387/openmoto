# Phase 3B — High-performance CAN sniffer

Status: **implemented and benchmarked on the physical bench.** The recommended
production configuration (`p3b_sniffer`: 4 MHz SPI, 921600 nominal baud, binary
output paced to 20 kB/s) passed every reliability test in its final run. Some
other configurations failed, and some limits remain unresolved; both are
reported below, not hidden. Phase 1, Phase 2 and Phase 3A sources are unchanged
and their HEX files rebuild to the verified hashes (`1f4bb0f5…`, `efd7a160…`,
`8fbca2d8…`, `185b02aa…`).

Summary JSON: [`tests/phase3b-result-20261023T095738Z.json`](../tests/phase3b-result-20261023T095738Z.json)
(all environment HEX/RAM/flash, tested-vs-rebuilt HEX equality for every final
run, baseline, transport totals, every final capture and evidence hashes).

Hardware is the unchanged Phase 2/3A bench: Mega 2560 (16 MHz), two
MCP2515/TJA1050 modules (8 MHz), CAN1 CS D10/INT D2 (LISTEN_ONLY), CAN2 CS D9
(bench traffic only), 500 kbps, ~55 Ω, USB power. The USB bridge is the stock
ATmega16U2 Arduino USB-serial firmware (VID:PID 2341:0042, bcdDevice 0x0001,
"Generic CDC", full speed), connected through a VIA Labs USB 3.0 hub. There is
no motorcycle, OBD, ECU or external CAN node.

## 1. Architecture

`firmware/mega-can/src/phase3b/main.cpp` (derived from Phase 3A, which is
untouched) plus `protocol.h` (binary encoder, pure C++, shared with the host
tests).

```text
MCP2515 #1 (LISTEN_ONLY) --INT D2--> serviceCan1(): READ STATUS, READ RX BUFFER
      (SPI, CS by direct port write)      | copy only: Frame -> 64-entry RAM queue
                                          v
                      flushOutput(): <=1 record/line per call, never while INT
                      is asserted; bytes written one at a time with CAN1 serviced
                      between bytes; CRC computed while writing; optional pacing
                                          v
                      HardwareSerial TX (256-byte buffer) -> ATmega16U2 -> USB -> host
```

- **CAN1 strictly LISTEN_ONLY.** There is no CAN1 transmit path; the mode is
  verified at init and every 100 ms (`mode_faults`, always 0). `p3b_sniffer`
  links no CAN2 transmit or bench code (`avr-nm` checked).
- **SPI integrity.** Init cross-selection check (Phase 2), then a burst
  write/read stress of the 8 TXB0 data registers of both chips in CONFIG mode
  (2,056 bytes per chip, every boot; 0 errors in every run). At runtime, the
  CNF1–3 readback every 100 ms is counted in `spi_errors` (0 everywhere), and
  every bench frame payload is verified.
- **Reception has strict priority over output.** Output never starts a record
  while INT is asserted, and a record in progress yields to CAN1 between bytes.
  Under overload, frames are therefore dropped in the queue, where they are
  counted at their exact position (DROP records), instead of being lost inside
  the controller.
- **Output modes** (runtime): `ASCII` (Phase 3A `F` lines), `BINARY`
  (protocol v1), `COUNT` (counters only, no frame output).
- **Bench commands** (bench builds only): `BENCH` (Phase 3A 200-frame
  integrity test), `FLOOD <0|8> <ms>` (one unACKed frame retransmitted
  back-to-back at full bus rate), `STREAM <0|8> <ms>` (unique frames, see
  §4), `WRAPTEST` (presets `millis()`/`micros()` ~3 s before their 32-bit
  wrap).
- **Memory.** No dynamic allocation. Static RAM is 2,481 B (`p3b_sniffer`) or
  2,690–2,694 B (bench) of 8,192 B. Measured minimum free RAM (stack painting)
  was ≥ 5,105 B in every final run (5,105–5,186 B). The queue high-water mark
  is reported (`queue_peak`, 64 = full). The full RAM scan takes ~5 ms and is
  only done while no test traffic runs; the heartbeat reports the cached value.

## 2. Configuration (PlatformIO environments)

| Environment | SPI | Baud (nominal → actual) | Pace | Purpose |
|---|---|---|---|---|
| `p3b_sniffer` | 4 MHz | 921600 → 1,000,000 | 20 kB/s | **Production sniffer** (no transmit code) |
| `p3b_bench_spi1_921k` / `spi4_921k` / `spi8_921k` | 1/4/8 MHz | 921600 → 1,000,000 | – | SPI sweep |
| `p3b_bench_spi4_460k` / `spi4_115k` | 4 MHz | 460800 → 500,000 / 115200 → 117,647 | – | Baud sweep |
| `p3b_bench_spi1_460k` | 1 MHz | 460800 | – | Transport-loss provocation |
| `p3b_bench_spi4_921k_pace20k` / `pace35k`, `p3b_bench_spi4_460k_pace20k` | 4 MHz | as named | 20 / 35 kB/s | Pacing |

**SPI clock.** The MCP2515 limit is 10 MHz (DS20001801K, SPI AC table; there is
no Fosc term). The only oscillator relation is the 128-OSC1-cycle start-up timer
(16 µs at 8 MHz) after reset, and the library already waits 10 ms. The AVR offers
F_CPU/2ⁿ, so only exact 8/4/2/1 MHz are accepted (compile-time check). At 8 MHz
the MCP2515 *output valid after clock low* (max 45 ns) leaves ~17 ns of margin
before the AVR samples 62.5 ns later. At 4 MHz the margin is ~80 ns.
Breadboard wiring and two chips on one MISO line consume part of that margin.

**Baud.** The AVR core computes UBRR with U2X: 460800 → **500,000** and 921600 →
**1,000,000** (+8.5 %). The 16U2 bridge computes the same divisors, so both
ends agree. This was confirmed by host-side operation: banners, commands and
CRC-valid binary records at every rate.

Relevant MCP2515 errata (DS80000179H): (1)/(4) TX-buffer corruption with
several pending buffers or TXB1/2 → the bench uses TXB0 only, one frame at a
time. (2) RTS with CS held low in SPI mode 11 → mode 00 is used and CS is raised
at once. (5) Forced-dominant ID bits on silicon before B4 → the silicon revision
is unknown; the integrity checks would catch it, and none was seen.

## 3. Binary protocol v1 (specification)

Encoder: `firmware/mega-can/src/phase3b/protocol.h` (pure C++, shared with the
host golden-vector test). Decoder: `host/python/phase3b_protocol.py`.

### Entering and leaving binary mode

The session starts in ASCII. The host sends `BINARY\n`. The firmware prints the
ASCII line `# MODE BINARY\r\n`, waits for its TX buffer to empty, and from the
next byte on sends only binary records, starting with a HELLO record. Command
responses and status text are carried in TEXT records. `ASCII` or `COUNT`
causes a final TEXT record `# MODE ASCII` / `# MODE COUNT`, after which the
stream is ASCII again. Commands from the host are always ASCII lines.

### Record framing

| Offset | Size | Field | Notes |
|---|---|---|---|
| 0 | 1 | sync0 | `0xA5` |
| 1 | 1 | sync1 | `0x5A` |
| 2 | 1 | version | `0x01` |
| 3 | 1 | type | see below |
| 4 | 2 | seq | uint16 little-endian; +1 for every record of any type; wraps 65535 → 0 |
| 6 | 1 | len | body length, 0..64 |
| 7 | len | body | type-specific, little-endian fields |
| 7+len | 2 | crc | CRC-16/CCITT-FALSE over bytes 2 .. 6+len (version..body), **big-endian** |

CRC-16/CCITT-FALSE: polynomial 0x1021, init 0xFFFF, no reflection, no final
XOR. Check value for ASCII `123456789` is `0x29B1`. Record size is 9 + len
bytes (max 73).

### Record types

| Type | Name | Body |
|---|---|---|
| 0x00 | HELLO | `protocol u8, variant u8 (0 sniffer, 1 bench), spi_hz u32, baud u32, queue_size u8, ts_us u32` (15 bytes) |
| 0x01 | FRAME | `ts_us u32, id_flags u32, dlc u8, data[n]`; n = 0 if RTR else min(dlc, 8) (9 + n bytes) |
| 0x02 | DROP | `dropped u32, total u32` (8 bytes): `dropped` frames were lost because the firmware output queue was full, immediately after the preceding FRAME record; `total` = cumulative queue drops counted when emitted |
| 0x03 | STATS | `ts_us, ms, rx, hw_overrun, queue_drop, merr, errif, int_missed, mode_faults, spi_errors, records` (11 × u32), `queue_peak, eflg_or, tec_peak, rec_peak` (4 × u8), `min_free_ram u16` (50 bytes); emitted about every 1 s (heartbeat) |
| 0x04 | TEXT | `flags u8` (bit0 = line continues in the next TEXT record), then 0..63 printable ASCII bytes; a line ends with a record whose bit0 is 0 |

FRAME `id_flags`: bit 31 = extended (29-bit) ID, bit 30 = RTR, bit 29 reserved
(must be 0), bits 28..0 = identifier (standard IDs ≤ 0x7FF). `dlc` is the raw
4-bit DLC (0..15); only min(dlc, 8) bytes are carried. `ts_us` is the AVR
`micros()` value when the frame was read from the MCP2515 (4 µs resolution).
It wraps every 2^32 µs (~71.6 min) and is unwrapped by the host. STATS
heartbeats guarantee records far more often than the wrap period.

### Decoder rules (host)

1. Search for `A5 5A`. Bytes skipped before a sync are reported as `garbage`.
2. If version ≠ 1 → `bad_version`; if len > 64 → `bad_length`; if CRC
   mismatches → `crc`. In every case, report 1 byte and resume the search at
   the next byte (resynchronization).
3. An incomplete record at the end of the capture is reported as `truncated`.
4. CRC-valid records whose content violates the rules above (length vs
   DLC/RTR, reserved bit, standard ID > 0x7FF, zero DROP count, non-printable
   TEXT, unknown type) are reported as `invalid_record`.
5. Byte accounting is exact: every input byte belongs either to a valid record
   or to exactly one reported error span. Nothing is discarded silently.
6. `seq` gaps mean records lost in transport. Duplicates or backwards steps
   are errors. Frame timestamps must be non-decreasing after unwrapping.

### Loss attribution

| Where | Detected by | Counter |
|---|---|---|
| MCP2515 receive buffers full (controller overrun) | EFLG RX0OVR/RX1OVR | `hw_overrun` (events; each ≥ 1 frame) |
| Firmware output queue full | firmware, exact | `queue_drop`; DROP records at the exact position |
| Serial/USB transport | host: record `seq` gaps, CRC errors | `record_seq_gaps`, `decode_errors` |
| Host decode | host | every error in `P.errors.jsonl` |
| STREAM unique-frame loss | firmware sequence check, exact | `lost` (must be explained by `hw_overrun`) |

## 4. Benchmark methodology

- **Host tool.** `host/python/capture_phase3b.py` resets the Mega (DTR) and
  checks the banner and `Config:` line against the expected SPI/baud and every
  init PASS line (including the SPI stress). It switches the output mode, runs
  one test, and ends binary sessions with `ASCII` so the stream stops at a record
  boundary. It saves `P.raw` (every byte), `P.log`, `P.frames.jsonl`,
  `P.errors.jsonl`, `P.timeline.json` (USB read arrival times) and
  `P.summary.json`. Live control uses an incremental parser; the verdict comes
  from a full re-analysis of the saved raw bytes. `--from-raw P.raw.gz`
  re-validates committed evidence offline.
- **Unique frames vs retransmissions.** With no ACKing node, NORMAL-mode
  retransmission produces copies (Phase 3A). `STREAM` therefore puts CAN2 in
  **one-shot mode**: every RTS is attempted exactly once (failed attempts set
  ABTF/TXERR; ABAT is never used, so the sent count is exact). This is the
  documented reason for using one-shot mode, and Phase 3A showed that CAN1
  stores every attempt. DLC 8 frames carry a 32-bit sequence number
  (`4F 4D 5B s0 s1 s2 s3 ~s0`, ID 0x100). DLC 0 frames use the ID as an 11-bit
  rolling sequence. Retransmission copies (FLOOD, BENCH) are never counted as
  unique frames.
- **Exact accounting** (firmware and host independently):
  `sent = unique + lost`; `lost` (sequence gaps + tail) must be explained by
  `hw_overrun > 0` and vice versa. The firmware requires `rx == unique` and
  `printed + queue_drop == rx`. The host requires `host_frames == printed`,
  `host_frames + queue_drop == rx`, DROP records summing to `queue_drop`,
  `host_unique + host_missing == sent`, `host_missing == lost + queue_drop`,
  0 decode errors and 0 record sequence gaps, with non-decreasing unwrapped
  timestamps. A one-shot attempt that completes without an error flag
  (`tx_ok`) is impossible without an ACKer and fails the test.
- **Loss categories** are reported separately: controller RX overrun
  (`hw_overrun`), firmware queue drop (`queue_drop`), serial/USB transport
  (`record_seq_gaps`, `decode_errors`, ASCII `unrecognized line`), and host
  decode errors (every one listed in `errors.jsonl`).
- **Verdicts.** `STREAM: PASS` means no corruption, no unexplained loss and exact
  accounting. `loss_free=YES|NO` additionally reports whether any frame was lost
  or dropped. No PASS criterion was relaxed during Phase 3B; several were
  tightened (see §7).
- **Limitation of STREAM.** One MCU drives both CAN2 (generator) and CAN1
  (sniffer), so the unique-frame rate is limited by the shared CPU/SPI, not by
  CAN1 alone. FLOOD (CAN2 retransmits autonomously at full bus rate) measures
  sniffer capacity, but with a single unique frame.
- **Durations.** STREAM 5 s, FLOOD 1 s, soaks 30–60 s, WRAPTEST 8 s.

## 5. Results (final firmware; run directories `tests/phase3b/final-*`)

### 5.1 Phase 3A baseline, reproduced (`tests/phase3b-baseline-*-20261023T080831Z.log`)

| FLOOD (Phase 3A firmware, 1 MHz SPI, 115200) | 3A result (075313Z) | Reproduced |
|---|---|---|
| DLC 8, printing | 3,848 rx/s, 446 lines, 24 overrun events | 3,849 rx/s, 446 lines, 18 |
| DLC 8, count-only | 3,857 rx/s, 0 overruns | 3,860 rx/s, 0 |
| DLC 0, count-only | 7,827 rx/s, 513 overrun events | 7,826 rx/s, 512 |
| BENCH ×2 | PASS | PASS, PASS |

### 5.2 SPI sweep (921600 baud, no pacing)

| Test | 1 MHz | 4 MHz | 8 MHz |
|---|---|---|---|
| BENCH (200 frames, binary) | PASS | PASS | PASS |
| FLOOD DLC 8 count-only: rx/s, overruns | 3,855, 0 | 3,858, 0 | 3,854, 0 |
| FLOOD DLC 0 count-only: rx/s, overruns | 7,753, **294** | 7,841, **0** | 7,828, **0** |
| FLOOD DLC 8 binary: rx/s, overruns, printed/s | 3,857, 0, 408 | 3,861, 0, 1,011 | 3,856, 0, 1,096 |
| FLOOD DLC 0 binary: rx/s, overruns | 7,766, 59 | 7,843, 0 | 7,836, 0 |
| STREAM count, loss-free unique/s DLC 8 / DLC 0 | 1,983 / 2,990 | 2,839 / 4,788 | 3,204 / 5,274 |
| STREAM binary, loss-free unique/s DLC 8 / DLC 0 | **FAIL** (transport) | 1,587 / 2,093 | 1,688 / 2,190 |
| STREAM ASCII, loss-free unique/s DLC 8 / DLC 0 | 1,077 / 1,715 | 1,421 / 2,194 | 1,502 / 2,301 |
| Max gap between CAN1 service calls (floods) | 244–344 µs | 140–276 µs | 124–264 µs |
| Transport: decode errors, lost records / binary bytes | 6, 3 / 331 kB | 0, 0 / 2.83 MB (incl. 60 s soak) | 0, 0 / 503 kB |

1 MHz is **not adequate** for Phase 3B: overruns in count-only DLC 0 floods,
BENCH failures in 2 of 5 runs, and the burstiest output (see §6.1). 4 MHz and
8 MHz are equivalent in reliability on this bench. 8 MHz gives ~10–13 % more
unique frames/s in count-mode STREAM, but its timing margin is ~17 ns versus ~80 ns. **4 MHz is selected**,
on reliability grounds.

### 5.3 Baud sweep (4 MHz SPI, no pacing)

| | 115200 (117,647) | 460800 (500,000) | 921600 (1,000,000) |
|---|---|---|---|
| Host-measured binary throughput (FLOOD DLC 8) | 11.7 kB/s | 28.8–29.4 kB/s | 39–40 kB/s |
| Binary records/s printed (FLOOD DLC 8, + DROP records) | ~374 | ~767 | ~1,011 |
| Loss-free binary STREAM DLC 8 / DLC 0 (unique/s) | none: queue drops above ~430 rec/s | 1,297 / 1,728 | 1,587 / 2,093 |
| Transport errors in final run | 0 / 521 kB | **12 decode errors (4 lost records) + 1 ASCII line / 2.13 MB** | 0 / 2.83 MB |

The configured baud works on the host at all three rates. The **effective**
throughput at 921600 is CPU-bound at ~40–44 kB/s (≈40 % of the 100 kB/s line).
Evidence: it is independent of SPI clock and plateaus while the line is idle part
of the time. The likely cost is the HardwareSerial TX interrupt (~100k/s at
1 Mbaud) competing with CAN1 servicing; that is not separately instrumented.
ASCII prints more lines/s than binary DROP+FRAME pairs under overload, but
binary carries CRC protection, µs timestamps and positioned drops.

### 5.4 Recommended configuration (4 MHz, 921600, 20 kB/s pacing): 18/18 PASS

| Test | Result |
|---|---|
| modes, BENCH | PASS, PASS (200/200, 0 overruns) |
| STREAM count DLC 8 / DLC 0 | 2,823 / 4,781 unique/s, **loss-free** |
| STREAM binary DLC 8 / DLC 0 | 1,861 / 3,213 offered/s: PASS, loss_free=NO (paced output → 6,879 / 13,088 counted queue drops; 0 controller loss) |
| FLOOD count DLC 8 / DLC 0 | 3,858 / 7,840 rx/s, **0 overruns** |
| FLOOD binary DLC 8 / DLC 0 (1 s) | 3,858 rx/s, 0 overruns / 7,839 rx/s, **1** overrun |
| 60 s soak DLC 8 ×2 | 231,434 rx each, 0 overruns, **0 decode errors, 0 record gaps** (1.20 MB each) |
| 60 s soak DLC 0 | 470,099 rx, **58** overrun events (0.012 %), 0 decode errors |
| WRAPTEST ×2 | PASS: `millis()` and `micros()` each wrapped exactly once; STATS heartbeat 998–1,002 ms across the wrap; frame timestamps monotonic after unwrapping |

Loss-free capture throughput with pacing is ~20 kB/s ≈ **770 DLC 8 records/s**
sustained. Above that, frames are dropped in the queue with exact accounting.
For lossless *reception statistics* at full bus load, use COUNT mode (0 overruns
up to 7,840 frames/s).

### 5.5 Production sniffer (`p3b_sniffer`, run `final-sniffer-20261023T095120Z`)

| Test | Result |
|---|---|
| Passive 10 s, binary ×2 | PASS, PASS (silent bus: rx=0; 0 decode errors) |
| modes | PASS |
| Passive 10 s, ASCII ×2 | PASS, **FAIL**: unresolved ASCII anomaly, see §6.3 |

### 5.6 Comparison with Phase 3A

| Metric | Phase 3A (1 MHz SPI, 115200, ASCII) | Phase 3B recommended (4 MHz, 921600, binary, paced) |
|---|---|---|
| Lossless reception, DLC 8 at full bus rate (count-only) | 3,857/s, 0 overruns | 3,858/s, 0 |
| Lossless reception, DLC 0 at full bus rate (count-only) | **No**: 7,827/s, 513 overrun events/s | **Yes**: 7,840/s, 0 |
| Controller overruns while printing, DLC 8 flood | 18–24 events/s | 0 |
| Host capture rate, DLC 8 frames | ~330 lines/s (446 incl. queue) | ~770 records/s paced (≤1,600/s unpaced) |
| Drop accounting | counter only | counter + exact position (DROP records) |
| Integrity check on host | line syntax | CRC-16 per record + sequence numbers |
| Timestamp | ms | µs (4 µs resolution), wrap verified on hardware |

## 6. Unresolved limitations and open findings

### 6.1 Serial/USB transport byte loss (stock 16U2 bridge), mitigated by pacing

Unpaced output loses **single bytes** in transport, sporadically. Across all
322 Phase 3B captures (58 MB of binary data) there were **31 lost records in 10
unpaced binary captures** and 2 damaged unpaced ASCII lines, at both 1,000,000
and 500,000 baud. In addition, before the strict ASCII parser existed, one
ASCII line loss was caught only by the count check. Every case was detected
(CRC + record sequence, strict ASCII classification, accounting) and reported;
none was silent.

- **Not host-side.** Timeline analysis (`host/python/timeline_check.py`,
  `tests/phase3b/tdiag-*`) placed all 13 losses of two 460800 soaks in USB reads
  that arrived after ordinary 0.03–0.08 ms gaps (7th–47th percentile), and
  **every one in a 96-byte chunk**. 96 is the "buffer nearly full" flush
  threshold of the stock 16U2 firmware (128-byte ring buffer). Hypothesis: the
  bridge drops bytes when fed long back-to-back bursts. It is not proven at the
  16U2 level (no instrumentation of the bridge).
- **Pacing:** with output limited to 20 kB/s (64-byte bursts): **0 record gaps**
  in all 56 paced captures, including 11 × 60 s soaks (14.6 MB). The only decode
  errors in paced captures are the 2 truncated tails caused by the host-tool
  flaw in §7, plus the ASCII anomaly in §6.3. At 35 kB/s: 1 lost record in 7
  soaks (13.8 MB). **20 kB/s is the verified loss-free setting; the safe limit
  between 20 and 35 kB/s is not determined.** 1 MHz SPI produces the burstiest output (longest
  mid-record service pauses) and the most frequent losses.
- A real fix (16U2 firmware with flow control, or a different USB bridge) is out
  of scope (no new hardware).

### 6.2 DLC 0 floods with output active

At 100 % bus load of minimal frames (~7,840/s, one per ~128 µs) with binary or
ASCII output active, ~1 controller overrun event per ~8,000–9,000 frames remains
(58 in 470,099). COUNT mode has 0. Streaming CRC computation (§7) did not change
this measurably. Cause not isolated; likely aggregate CPU load (UART TX ISR +
SPI) leaves too little slack for two 128 µs frame periods.

### 6.3 ASCII passive anomaly (unresolved)

In 2 of 5 ASCII passive captures with the production sniffer (`final-sniffer-20261023T093946Z/01`,
`final-sniffer-20261023T095120Z/02`), 6–7 bytes inside the periodic `S` line
were replaced by the host's own `STATS\n` command bytes, sent at that moment.
Both cases sit at the identical stream offset (1,176/1,177) in the last USB read
before the port closed. Targeted reproduction failed: 79 marker lines sent during
continuous output, plus 426 (production firmware) and 214 (4 MHz bench firmware)
`STATS` commands colliding with long `S` lines, produced 0 leaks and 0 malformed
lines
(`tests/phase3b/echo-20261023T095345Z`). The validator reported both
occurrences. Binary mode's CRC would reject any such substitution.
**Recommendation:** use BINARY mode for evidence, and avoid sending commands
while ASCII output is streaming.

### 6.4 Other limits

- Unique-frame generation shares the MCU (§4). STREAM rates are system
  rates, not CAN1 limits.
- Effective serial throughput is CPU-bound (~40–44 kB/s at 1 Mbaud). A leaner
  custom UART driver could raise it; not implemented.
- `ts_us` has 4 µs resolution and is the MCU read time, not bus arrival time.
- One-shot completion flags can be read before the MCP2515 sets ABTF/TXERR
  (observed 2/16,763 at 115200); a second read classifies them (`flag_races`).
- No vehicle, OBD, ISO-TP or UDS validation; bench-only.

## 7. Investigation history (all logs preserved under `tests/phase3b/`)

| Runs | Finding | Change |
|---|---|---|
| `smoke`, `spi1/4/8-0821…0824` | Binary FLOOD printed only ~95 frames/s: pending DROP had priority and was re-armed by every new drop (**starvation**) | Drops attached to queue entries; DROP emitted right after its frame |
| `spi*-0827…0830` | Binary output caused 733–1,108 controller overruns/s | Investigation → instrumentation |
| `diag-0842` | `max_flush_us` 5,488: binary STATS heartbeat scanned ~5 KB of RAM | RAM scan only when idle; heartbeat uses cached value |
| `diag2-0844` | `max_flush_us` 900 (binary FRAME+DROP) > CAN1 2-buffer slack | RX gate + byte-level preemptible output |
| `diag3-0846` | Binary floods: overruns 973 → 0 (DLC 8) | — |
| `spi1-0821`, `spi1-0834` | 1 MHz BENCH overrun (2 of 5 runs); transport byte loss mainly at 1 MHz | 1 MHz rejected |
| `spi8-0838` | Single lost byte in ASCII (`034` → `34`); host parser had accepted a line missing its `F` as text | Strict ASCII line classification (every unknown line is an error) |
| `spi4_115k-0901` | `tx_ok=2`: one-shot completion flags read too early | Second read; `flag_races` counter |
| `spi4_460k-0858`, `tdiag-0905` | Transport loss at 500 kbaud too; timeline places it in 96-byte 16U2 chunks | Output pacing option |
| `pace_*`, `pace35_*` | 20 kB/s: 0 errors; 35 kB/s: 1 in 3 soaks | 20 kB/s recommended |
| `crcdiag-0920` | Hypothesis "STATS CRC causes DLC 0 overruns" **falsified** (60 → 51) | Streaming CRC kept (byte-identical, verified by golden test) |
| `final-spi4_921k_pace20k-0932` | **WRAPTEST found a real bug**: a stale pacing timestamp (> 2³¹ µs) blocked output (would also hit after ~36 min of idle paced output) | Pacing credit refreshed every 100 ms |
| `final-sniffer-0949` | Capture closed mid-STATS record → `truncated` (host-tool flaw) | Binary sessions end with `ASCII` at a record boundary |
| `final-*` | Final results (§5). Tested HEX == clean rebuild for all 8 final runs | — |

Superseded final-campaign directories (firmware or tool later fixed):
`final-spi4_921k_pace20k-20261023T093235Z`, `final-spi4_921k_pace35k-20261023T093728Z`,
`final-sniffer-20261023T093946Z`, `final-sniffer-20261023T094901Z`. `spi1_460k`
lost two captures to a runner naming collision; the evidence guard refused to
overwrite. In earlier runs, `spi4_115k` and other pre-final directories used
earlier firmware revisions (HEX in each `upload.log`).

## 8. Reproduce

```sh
# host tests (stdlib + clang++); golden vectors from the firmware's protocol.h
host/python/.venv/bin/python tests/test_phase3b_protocol.py
# build
./scripts/pio run -d firmware/mega-can -e p3b_sniffer
# verify the port first, then (bench uploads only on the isolated bench):
./scripts/pio device list
./scripts/pio run -d firmware/mega-can -e p3b_bench_spi4_921k_pace20k -t upload --upload-port USB_PORT
host/python/.venv/bin/python host/python/capture_phase3b.py --port USB_PORT --baud 921600 \
    --expect-spi 4000000 --test stream --mode binary --dlc 8 --ms 5000 --output-prefix tests/phase3b/NEW/stream8
# re-validate committed evidence offline
host/python/.venv/bin/python host/python/capture_phase3b.py --from-raw tests/phase3b/<run>/<capture>.raw.gz \
    --test flood --mode binary --dlc 8 --ms 60000 --baud 921600 --expect-spi 4000000 --output-prefix /tmp/check
# aggregate
host/python/.venv/bin/python host/python/summarize_phase3b.py tests/phase3b/final-* --json out.json --md out.md
```

Evidence layout: each `tests/phase3b/<label>-<UTC>/` holds `upload.log` (with
the flashed HEX SHA-256) and per capture `.raw.gz`, `.timeline.json.gz`,
`.log`, `.errors.jsonl`, `.summary.json` and `.console`. Decoded
`*.frames.jsonl` dumps (382 MB) are not committed; they are regenerated exactly
by `--from-raw`. Run long sessions under `caffeinate -i` (the bench Mac
idle-sleeps on battery).
