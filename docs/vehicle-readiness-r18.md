# Vehicle readiness audit: `p3b_sniffer` on a 2021 BMW R18 (passive capture)

Date: 2026-10-23. Scope: whether the Mega 2560 + MCP2515/TJA1050 interface
running `p3b_sniffer` (HEX `4c684a9e…`) may be connected to a 2021 BMW R18 for
**strictly passive** CAN capture. No CAN operation was performed for this
audit, and nothing was uploaded.

## Verdict: **NO-GO** (until blockers B1–B4 are cleared)

The **software** evidence for passive behavior is strong, at both source and
linked-binary level. The **electrical and vehicle-side** preconditions are
unverified: connector pinout, bus identity/termination, ground path,
floating-SPI behavior during reset. These cannot be proven from this repository
and must be measured on the actual hardware. Project policy
(`.kiro/steering/hardware-safety.md`) and the firmware banner still forbid any
motorcycle/OBD connection. Both were left unchanged, because this review does
not justify removing them.

## 1. Software guarantees (verified)

| # | Property | Evidence |
|---|---|---|
| S1 | `p3b_sniffer` builds with `OPENMOTO_SNIFFER_BENCH=0`; every CAN2 traffic path is compiled out | `platformio.ini:98-100`; BENCH-only blocks in `src/phase3b/main.cpp` 427-558 (CAN2 TX/abort/stop), 740-986 (BENCH/FLOOD/STREAM/WRAPTEST), 1042-1051 (their commands) |
| S2 | No transmit method or non-silent mode request is linked | Linked MCP2515 methods: `reset, setConfigMode, setMode, setRegister(s), modifyRegister, start/endSPI` only (no `sendMessage`, `setNormalMode`, `setLoopbackMode`, `setNormalOneShotMode`, `setSleepMode`) |
| S3 | Every `setMode` request is CONFIG (0x80) or LISTEN_ONLY (0x60) | Disassembly: `setConfigMode` = `ldi r22,0x80; jmp setMode`; the only direct call has `ldi r22,0x60` (`main.cpp:1145`, inlined `setListenOnlyMode`). CANCTRL (0x0F) is modified only inside `setMode` |
| S4 | All SPI bytes go through `SPIClass::transfer`; the compiled sniffer source sends only READ/WRITE/BITMOD/READ STATUS/READ RX BUFFER plus the library RESET | Preprocessed `p3b_sniffer` source; `out 0x2e` (SPDR) only in `SPIClass::transfer` |
| S5 | Startup is silent: power-on → CONFIG; `reset()` → CONFIG and zeroes TXB0-2CTRL (TXREQ=0) (`mcp2515.cpp:44,51`); bitrate set in CONFIG (`mcp2515.cpp:213`); TXREQ verified clear, then LISTEN_ONLY verified by CANSTAT and CANCTRL (`main.cpp:1143-1146`). CAN1 is never in NORMAL | `main.cpp:1104-1152` |
| S6 | SPI stress writes only the TXB0 **data** registers (0x36-0x3D), in CONFIG, then zeroes them; it never touches TXBnCTRL | `main.cpp:1085-1101` |
| S7 | Hardware TXnRTS pins cannot trigger TX: the library never writes TXRTSCTRL, so the power-on default (pins = digital inputs) holds | `mcp2515.cpp` (no TXRTSCTRL write) |
| S8 | LISTEN_ONLY sends no ACK and no error frames; error counters are deactivated (DS20001801 §9.3). A wrong bit rate therefore produces receive errors (`merr`), not bus traffic | datasheet |
| S9 | Init failure → both controllers to CONFIG, MCU halts (silent) | `main.cpp:1178-1181` |
| S10 | Runtime: CAN1 mode re-read every 100 ms; any deviation → both to CONFIG, capture off, `# ALERT`, reset required | `main.cpp:695-702` |
| S11 | Serial commands in the sniffer (`START STOP STATS RESET HELP ASCII BINARY COUNT`) change only counters and output mode; `RESET` resets counters, not the MCP2515; unknown text is rejected | `main.cpp:1011-1053` |
| S12 | CAN2: reset to CONFIG, never leaves it (no CAN2 mode call except CONFIG is linked); also physically disconnected | `main.cpp:1110` (reset+CONFIG), `1136` (CNF1 only, still CONFIG), `1149` (CONFIG verified) |

Automated check (new): `python3 tests/check_p3b_sniffer_passive.py` re-builds
`p3b_sniffer` and asserts S2–S4 on the linked ELF. Its negative control,
`… p3b_bench_spi4_921k_pace20k`, correctly fails: it flags the transmit
functions and three `setMode(0x00)` NORMAL requests.

**Residual software-adjacent risks (not eliminated by code):**

- R-SW1: The MCP2515 has no mode lock. SPI corruption (EMI, a loose jumper)
  could in principle write CANCTRL. This would be detected within ≤100 ms and
  forced back to CONFIG (S10), but not prevented.
- R-SW2: During an MCU reset (every serial-port open toggles DTR) and the
  ~1 s bootloader, D10/D9/SCK/MOSI are inputs. **Whether the modules pull CS
  high is unverified**, and floating lines could clock spurious SPI. The
  probability is low, but it is not zero.

## 2. Is "ISOLATED BENCH ONLY" just a warning?

The string (`main.cpp:1176`) has no behavior attached. It reflects real
limitations: no galvanic isolation, no TVS/common-mode protection, an
unverified bus, and project policy. **It is kept.**

## 3. Electrical review (hardware limitations; not provable by software)

| Item | Assessment |
|---|---|
| Transceiver | TJA1050 datasheet: "an unpowered node does not disturb the bus lines", TXD dominant time-out, bus pins short-circuit proof to battery and ground, transient protected. It also has a **silent mode (pin 8 `S`) that disables the transmitter**. On these modules S is normally tied low (transmitter enabled), so silence is enforced only by MCP2515 configuration. **Unverified on your module.** |
| Termination | CAN1 jumper removed (good: a 120 Ω on a 2×120 Ω bus would load it to ~40 Ω). **Not yet verified by measurement.** The earlier bench reading of the module network was 109.6 Ω |
| Ground / USB | Arduino GND = laptop ground (USB) = bike chassis (OBD pin 4). Any second ground path (mains charger, powered USB hub/dock, HDMI/Ethernet) forms a ground loop through the bike chassis. The Mega is currently connected **through a VIA Labs USB 3.0 hub** |
| 5 V logic | Internally consistent (Mega, MCP2515, TJA1050 all at 5 V); the bike only sees the transceiver bus pins |
| +12 V | OBD pin 16 isolated: required, because +12 V on Arduino GND/5 V would be destructive. Insulate it mechanically, not just leave it unconnected |
| Stub | Breakout + jumpers form an unterminated stub; keep it short (≲ 30 cm at 500 kbps) |
| Isolation / transients | None. Cranking, ignition switching and load dump can shift grounds; ESD is unprotected. The main *bike-side* hazard is a damaged transceiver holding the bus dominant (TXD time-out mitigates driver-side faults only) |
| CAN2 module | CAN-H/L disconnected: insulate the loose wire ends so they cannot touch the breakout or chassis |

## 4. Bus speed and connector (vehicle side)

- **500 kbps is a bench assumption only.** No document in the repository or
  source found in this review confirms the R18's diagnostic-CAN bit rate. The
  firmware supports only 500 kbps. Because of S8, a wrong rate is not a bus
  hazard in LISTEN_ONLY, but the capture would show `rx=0` with `merr>0`.
- BMW Motorrad bikes use a proprietary round diagnostic socket and need a
  10-pin→OBD2 adapter (K-line + CAN). The R18's socket type, which pins carry
  CAN, and **how your adapter maps them to OBD 6/14/4/5/16 are unverified**.
- The diagnostic CAN may be gatewayed: a passive capture can legitimately be
  empty (`rx=0`, `merr=0`).

## 5. Blockers (must clear before GO)

| ID | Blocker | How to clear (measurements with the interface **disconnected** from the bike) |
|---|---|---|
| B1 | Connector/adapter pinout unverified | Get the adapter's wiring documentation; continuity-test R18 socket → OBD pins 6/14/4/5/16. With ignition ON, measure at the adapter: pin 16 ≈ battery voltage; pins 6 and 14 each ≈ 2.5 V to pin 4 (idle, recessive); pin 4 ↔ battery negative ≈ 0 Ω (ignition OFF) |
| B2 | Bus identity/termination unverified | Ignition OFF: resistance pin 6 ↔ 14 at the adapter ≈ 60 Ω (two 120 Ω terminations). Interface side, unpowered, jumper removed: CAN1 H ↔ L ≫ 1 kΩ (not ~120 Ω) |
| B3 | Ground path | Laptop on battery with the charger unplugged; remove the VIA Labs hub (direct USB) or prove it is bus-powered with no external supply; no other cables on the laptop |
| B4 | Floating SPI during MCU reset (R-SW2) | Measure CS1 (MCP2515 pin 16) to module VCC: if no pull-up, add 10 kΩ CS1/CS2 → 5 V. Procedurally, let the reset/init happen *before* connecting to the bike (`--vehicle` pauses after init for this) |

Strongly recommended (not blocking a first stationary capture):

- **H1:** Tie TJA1050 pin 8 (S) of the CAN1 module to VCC, which turns "never
  transmits" into a hardware property. Verify with S high that RXD still
  receives on the bench.
- **H2:** Stationary bike, ignition ON, engine OFF; do not crank while connected;
  never ride with it attached; short sessions.
- **H3:** Once the rate is known, add build-time bit-rate selection (separate
  environment) if 500 kbps shows `rx=0, merr>0`.
- **H4:** Policy: lifting the bench-only rule in `.kiro/steering/hardware-safety.md`
  is your explicit decision, to make after B1–B4.

## 6. Host workflow review and changes

`capture_phase3b.py --test passive` opens the port (DTR reset), waits for
READY, then sends only `BINARY`/`COUNT` (mode), `START`, `STATS` and finally
`ASCII`. In `p3b_sniffer` none of these touches CAN (S11). Previously, though,
the same tool *could* send `BENCH`/`FLOOD`/`STREAM`/`WRAPTEST`, which transmit
on CAN2 if bench firmware were flashed by mistake.

New `--vehicle` guard (host-side, no firmware change):

- Only `--test passive` is accepted, and `--expect-spi` is required.
- A hard command allowlist `START STOP STATS BINARY ASCII COUNT HELP` is
  enforced before any byte is written. Everything else raises
  `VehicleGuardError`.
- Pre-flight: the banner must show `Variant: sniffer (CAN2 held silent in
  CONFIG)`, all init PASS lines, `MCP2515 #1 LISTEN_ONLY: PASS`, `MCP2515 #2
  CONFIG (silent): PASS`, and the expected `Config:` line. Otherwise it aborts
  with nothing sent.
- It pauses after reset and init for the operator (B4 procedure).
- Verdict criteria: mode faults, SPI errors, missed interrupts, wrong variant
  (HELLO), and transport/decode errors. Frames, overruns and queue drops on a
  live bus are reported as measurements, with an interpretation of `rx`/`merr`.

The detailed first-capture procedure is intentionally **not** given while the
verdict is NO-GO.

## 7. Tests run

- `tests/test_phase3b_protocol.py`: 28/28 OK (23 existing + 5 vehicle-guard tests)
- `tests/check_p3b_sniffer_passive.py`: PASS on `p3b_sniffer` (HEX `4c684a9e…`,
  unchanged); negative control FAILs as required
- `tests/test_phase2_firmware.py`: PASS
- Firmware: unchanged (no rebuild differences)
