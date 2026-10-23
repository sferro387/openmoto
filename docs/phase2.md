# Phase 2: two-controller physical CAN bench

Status: **PASS ON REAL HARDWARE**. Explicitly authorized upload and flash
verification succeeded; both NORMAL-mode physical directions passed the initial
100-frame and subsequent 1000-frame tests. All 2200 frames were transmitted,
received and verified with zero reported errors/timeouts. See the actual run below.

Phase 1 remains at Git commit `9b0ac61`, tag `phase1-verified-loopback`.
Its source `firmware/mega-can/src/main.cpp`, verified serial log, and result JSON
are unchanged. The Phase 2 source is separate in `src/phase2/main.cpp` and only
built by the explicit `physical_dual_8mhz` environment. Nothing is installed again.

## Unpowered inspection and wiring

1. **Disconnect USB now**, plus any other power. Do not attach a motorcycle, ECU,
   OBD cable, external CAN node, or external 12 V supply at any time.
2. Inspect the metal crystal on module #2 and report the complete marking. Module
   #1 was confirmed `8000` (8 MHz); #2 must independently be confirmed 8 MHz before
   upload. Do not infer this from the boards looking alike. Stop if it differs.
3. Before joining CAN-H/L, inspect BOTH boards for a resistor across CAN-H/L and
   any termination jumper/switch. A marking `121` commonly denotes 120 ohms, but
   visual inspection alone does not prove the resistor is connected. Record each
   board's fitted/enabled termination. Do not add extra resistors blindly.
4. With USB still disconnected, wire the shared SPI/power signals as below. Check
   labels on each board rather than relying on pin order or wire colors.

| Signal | Mega 2560 | MCP2515 #1 | MCP2515 #2 |
|---|---|---|---|
| Power | 5V | VCC | VCC |
| Ground | GND | GND | GND |
| SPI clock | D52 | SCK | SCK |
| SPI MOSI | D51 | SI | SI |
| SPI MISO | D50 | SO | SO |
| Chip select 1 | D10 | CS | — |
| Chip select 2 | D9 | — | CS |
| Interrupt 1 | D2 | INT | — |
| Interrupt 2 | D3 | — | INT |

```text
                  Mega 2560
  5V/GND/D52/D51/D50 ── shared power/ground/SPI ──┬─ MCP2515 #1
                                              └─ MCP2515 #2
  D10 ──────────────────────────────────────────── #1 CS
  D9  ──────────────────────────────────────────── #2 CS
  D2  ──────────────────────────────────────────── #1 INT
  D3  ──────────────────────────────────────────── #2 INT

        Module #1                       Module #2
        CAN-H o──────── twisted ─────────o CAN-H
              │           pair           │
            [120Ω]                     [120Ω]
              │                          │
        CAN-L o──────────────────────────o CAN-L
        GND   o──── common via Mega ─────o GND
```

The two controllers share SCK/MOSI/MISO, but D10 and D9 independently select
which chip drives MISO. Do not join CS pins. Firmware drives both CS latches HIGH
and sets D53 OUTPUT/HIGH before starting SPI. SPI is paced at 1 MHz for jumper
wires. D2/D3 are wired but polled reception is used; interrupts are not validated.
Keep power leads short and use a short twisted pair for CAN-H/L between modules.

## Multimeter termination procedure — power MUST be disconnected

A resistance measurement on a powered bus is invalid and can damage a meter.

1. Confirm USB and all other power are removed. Allow capacitors to discharge;
   check with the meter in DC volts that there is no bus/supply voltage before
   selecting resistance. Never use current mode across CAN-H/L.
2. Select ohms (auto-range or a suitable low resistance range). Touch probes
   together to check lead resistance, typically near zero; separate them to check
   open-circuit indication. Use metal terminal contacts for measurements.
3. With the CAN pair NOT connecting the modules, measure CAN-H to CAN-L on each
   isolated module. Record both readings and the termination jumper positions.
   An enabled 120-ohm terminator should give approximately 120 ohms. A different
   reading requires inspection; do not infer a terminator from appearance.
4. If a module has no enabled termination, stop and report it. Arrange exactly
   one 120-ohm termination at each end before proceeding; use onboard termination
   where verified, otherwise an appropriate external resistor after identifying
   what is missing. No third termination is allowed.
5. With power still disconnected and exactly two terminations identified, connect
   #1 CAN-H to #2 CAN-H and #1 CAN-L to #2 CAN-L. Both grounds already share Mega
   GND. Measure CAN-H to CAN-L across the completed bus at each end.
6. Expect **approximately 60 ohms**: `120 || 120 = 60`. A practical sanity band is
   55–65 ohms after allowing for leads/tolerance, not a substitute for identifying
   both terminators. Both ends should agree. Remove meter leads afterward.

| Completed-bus reading | Interpretation / action |
|---|---|
| Approximately 60 ohms | Consistent with two 120-ohm terminators; report exact readings |
| Near zero / a few ohms | Possible short: STOP; inspect power-off wiring |
| Approximately 120 ohms | Likely only one termination or a broken CAN connection: STOP |
| OL / very high | Missing termination/open wiring: STOP |
| Approximately 40 ohms or lower than expected | Possible third/additional termination or fault: STOP |
| Other unexpected/unstable reading | STOP; inspect and report; do not power or upload |

Resistance does not prove CAN-H/L polarity, signal quality, or transceiver health.
Check H-to-H and L-to-L visually too. Do not modify wiring with USB connected.

## Hardware record and upload gate

Fill this in from actual inspection, not example values:

| Item | Current observation |
|---|---|
| Module #1 crystal | `8000`, 8 MHz, confirmed in Phase 1 |
| Module #2 crystal | 8 MHz, confirmed by user |
| Module #1 termination network | 109.6 ohms, reported by user; individual resistor value/jumper details not independently checked |
| Module #2 termination network | 109.8 ohms, reported by user; individual resistor value/jumper details not independently checked |
| Completed bus H-to-L | 54.9–55.0 ohms, reported by user |
| Dual SPI, individual CS, INT, power and common ground | Manually checked by user; no 5V/GND short reported |
| H-to-H, L-to-L continuity / no direct H-to-L short | PASS, reported by user; twisted-pair construction not separately reported |
| Vehicle/OBD absent | Confirmed by user; no external devices/12 V authorized |
| USB connected | Confirmed by user |
| Enumerated target USB serial port | `/dev/cu.usbmodem21101`, VID:PID 2341:0042, serial 34530303238351110202; user explicitly approved and upload completed |
| Explicit Phase 2 upload authorization | Granted for physical_dual_8mhz on /dev/cu.usbmodem21101; upload/run completed |

After acceptable unpowered checks, reconnect USB for enumeration. Phase 1 firmware
will still be on the Mega until the Phase 2 upload. The second CS input may float
while Phase 1 runs: disregard its output and do not use Phase 1 to test this dual
configuration. Phase 2 explicitly initializes both CS lines. Identify the current
device with `./scripts/pio device list`. The user has granted standing approval
for bench uploads and runs; no per-run confirmation is required.

## Build, upload, and evidence capture

From the existing repository root:

```sh
./scripts/pio run -d firmware/mega-can -e physical_dual_8mhz
```

Upload and capture (standing user approval):

```sh
./scripts/pio device list
./scripts/pio run -d firmware/mega-can -e physical_dual_8mhz -t upload --upload-port /dev/cu.REPLACE
host/python/.venv/bin/python host/python/capture_phase2.py --port /dev/cu.REPLACE --output tests/phase2-physical-serial.log
```

Replace the port with the newly verified USB device. Close other serial monitors
before upload/capture. The capture script uses existing pyserial 3.5, intentionally
resets the Mega through DTR, listens at 115200, saves a new exclusive log, and
waits at most 90 seconds for the one-shot test. It validates initialization and
four complete zero-error summaries plus final `PHASE 2: PASS`. It refuses to
overwrite an existing filename. Keep failed runs too; use new filenames on retries.
A capture timeout means no accepted result, not proof of physical bus failure.
Opening a serial monitor/resetting the board reruns all physical tests.

## Test methodology

- A: reset both chips into CONFIG; two swapped CNF1 read/write patterns establish
  independent SPI access and expose absent chips, stuck reads, or shared CS.
  Restore and read back CNF1/2/3 for 500 kbps at 8 MHz. Check library return values
  and CANSTAT operating-mode bits for both NORMAL transitions; one-shot/loopback
  mode is never selected.
- B: 100 standard 11-bit ID `0x123` frames from #1 to #2.
- C: 100 standard 11-bit ID `0x456` frames from #2 to #1.
- D: after B and C pass, 1,000 more #1 to #2 followed by 1,000 #2 to #1. A failure
  prevents subsequent tests; the full clean run is 2,200 frames.
- DLC is 8. Payload: `4F 4D`, test stage byte (1/2/3/4), direction byte (1/2), and
  a 32-bit little-endian sequence, starting at zero for each test. Validate exact
  CAN ID including frame flags, DLC, all payload bytes, stage, direction and order.
  A 1,000-bit history detects duplicates within each stage. Prior-stage frames
  are rejected by the payload tag. Out-of-order/out-of-range sequences are errors.
- Only one frame is outstanding. Queue via TXB0, require TXREQ to clear AND TX0IF
  to assert, and require the other chip to receive and verify the frame. A library
  `sendMessage` success alone means queued, not transmitted. The receiving NORMAL
  controller supplies CAN's physical ACK; a verified receive checks delivery.
- Bound each send/receive completion wait to 250 ms. After a matched frame, poll
  both controllers during at least 5 ms of quiet pacing to catch duplicates and
  avoid FIFO overflow. Do not print per-frame progress on the time-critical path.
- Inspect EFLG, ERRIF/MERRF, TEC/REC and operating modes throughout the test. Any
  observed controller flag, nonzero error counter, unexpected sender RX, API error,
  timeout, duplicate or invalid receive fails immediately. Ordinary NORMAL mode
  does perform automatic retries before software can react; errors are not hidden.
- On first failure abort pending TX, request CONFIG for both, print available
  counters and halt. After success also request/verify CONFIG before final PASS.
  A reset reruns the suite; no continuous test traffic follows the final result.

Each direction reports requested/queued/completed TX, received/verified, timeouts,
missing, payload mismatches, duplicates, unexpected frames, sequence errors,
API errors, controller error observations, current and sampled peak TEC/REC,
ORed error flags/interrupts, and duration. Suite duration includes reporting and
controller setup/stop, excludes the initial 500 ms serial startup delay.

Counts are fail-fast: `missing` counts an expected receive that times out; invalid
frames have their own counters. Frames after failure are **not attempted**, not
claimed as dropped. Received counts are at the designated receiver; a frame on
the sender is counted as unexpected. Error observations count affected sampled
controllers, not an exact count of physical bit errors. Peaks are sampled, so
short transient counter changes may go unobserved. No zero-error claim extends
beyond the actual observation window.

## Verification and actual results

- Phase 1 reference commit/tag saved; source and serial evidence retained.
- Both Phase 1 8 MHz and Phase 2 builds succeeded with the existing wrapper.
- Phase 2 software fault-injection: 21 host-only scenarios pass. Run with
  `python3 tests/test_phase2_firmware.py`; requires the existing Apple clang++.
  The harness compiles the actual firmware source against MCU/controller stubs
  and checks clean completion, bounded missing/TX waits, error rejection and stop.
  It does not model arbitration, CAN waveforms, a transceiver or real ACK timing.
- Real Phase 2 upload: **PASS**, 9746 flash bytes verified.
- Real Phase 2 CAN tests: **PASS**, 2200/2200 transmitted/received/verified; zero reported errors/timeouts. Full evidence below.

### Known limits

Short, paced bench test only; no maximum-throughput, simultaneous traffic,
extended/RTR, long-cable, EMI, isolation, voltage-transient, automotive power,
interrupt, or independent clock-frequency testing. Both controllers share one
MCU/power/SPI; this is not two independent diagnostic devices. Termination must be
measured by the user. Do not connect a motorcycle/ECU/OBD cable, transmit diagnostic
requests, modify ECU settings, supply 12 V or proceed to Phase 3.

Register definitions and bit timing are checked against the pinned upstream
[autowp source](https://github.com/autowp/arduino-mcp2515/tree/49e0d7d6755bab09cc3e68997bfdb75264c8daf7),
which remains unmodified.

## Latest build and user-reported assembly checks

Build log: tests/phase2-build-20261023T062058Z.log (UTC timestamp in filename).
`./scripts/pio run -d firmware/mega-can -e physical_dual_8mhz` exited 0.
Flash 9746 bytes (3.8%); static RAM 361 bytes (4.4%). HEX SHA-256 remains
`efd7a1602871970774de550f874cd1fea0ee78184dcd19b5b0fff7f1f9cb754b`.
Source uses CAN_500KBPS, MCP_8MHZ and setNormalMode() on both instances, then
checks NORMAL CANSTAT bits and CANCTRL; no loopback call exists in Phase 2.

User reports termination network readings 109.6 and 109.8 ohms, combined 54.9–55.0
ohms. Their parallel equivalent is approximately 54.85 ohms, consistent with the
reported bus reading. These network readings are lower than nominal 120/60;
the cause has not been established. Do not relabel them as measured 120-ohm
components or as evidence of a successful physical CAN test. H-to-H and L-to-L
continuity passed; no direct H/L or 5V/GND short; all SPI/CS/INT/power/ground
wiring was manually checked. Both oscillators are user-confirmed 8 MHz.

USB enumeration (without opening/resetting the port) found
/dev/cu.usbmodem21101, VID:PID 2341:0042, serial 34530303238351110202, location
2-1.1, matching the Phase 1 board. User explicitly withheld upload until firmware
and target port approval. No upload or physical CAN test was attempted.

## Phase 2 physical CAN PASS — actual hardware run

User explicitly approved `physical_dual_8mhz` on `/dev/cu.usbmodem21101`, with
both 8 MHz modules wired, termination checked and USB-only power; no vehicle or
OBD connection. The port identity was re-enumerated before upload and matched
VID:PID 2341:0042, serial 34530303238351110202. No Phase 3 work was performed.

The existing wrapper uploaded the approved HEX. avrdude identified ATmega2560
signature 0x1e9801, wrote **9746 bytes** and read back/verified all **9746 bytes**.
Upload exit status was 0. Firmware HEX SHA-256:
`efd7a1602871970774de550f874cd1fea0ee78184dcd19b5b0fff7f1f9cb754b`.

Serial was captured at **115200 baud** after an intentional DTR reset. Both
controllers passed independent SPI challenges, 500 kbps / MCP_8MHZ configuration,
and verified NORMAL mode transitions. The complete captured run reports:

| Test | Direction | Transmitted | Received | Verified | Timeouts | Duration ms |
|---|---|---:|---:|---:|---:|---:|
| B | CAN1 → CAN2 | 100 | 100 | 100 | 0 | 852 |
| C | CAN2 → CAN1 | 100 | 100 | 100 | 0 | 853 |
| D | CAN1 → CAN2 | 1000 | 1000 | 1000 | 0 | 8535 |
| D | CAN2 → CAN1 | 1000 | 1000 | 1000 | 0 | 8535 |
| Total | Both directions | **2200** | **2200** | **2200** | **0** | **18996 suite** |

All 2200 requested frames were queued and completed transmission. Missing frames,
payload mismatches, duplicates, unexpected frames, sequence errors, SPI/library/TX
API errors and CAN controller error observations were **zero in every stage**.
For both controllers in every stage: TEC=0, REC=0, sampled peak TEC/REC=0,
EFLG_OR=0 and ERRINT_OR=0. These are sampled observations, not certification that
no transient electrical error could occur between polls. Suite time includes
setup/reporting/stop; the sum of direction-test times is 18775 ms.

Both controllers reported `stopped: CONFIG` before `PHASE 2: PASS`. Capture exited
0 after checking all four stage summaries and final PASS. No further test traffic
is generated until a reset; reopening serial can reset the Mega and rerun tests.

Evidence (UTC timestamp in filenames):

- Complete MCU serial output: [tests/phase2-serial-20261023T062731Z.log](../tests/phase2-serial-20261023T062731Z.log)
- Upload/build and flash verification: [tests/phase2-upload-20261023T062731Z.log](../tests/phase2-upload-20261023T062731Z.log)
- Capture process output/status: [tests/phase2-capture-20261023T062731Z.log](../tests/phase2-capture-20261023T062731Z.log)
- Parsed actual counts, counters, durations and evidence hashes: [tests/phase2-result-20261023T062731Z.json](../tests/phase2-result-20261023T062731Z.json)
- Process run record: [tests/phase2-run-20261023T062731Z.json](../tests/phase2-run-20261023T062731Z.json)

Phase 1 source, reference tag and original verified capture are preserved. This
passes the isolated, paced, sequential physical-CAN bench test under the reported
109.6/109.8-ohm network and 54.9–55.0-ohm bus readings. It does not validate
interrupt delivery, simultaneous traffic, maximum throughput, automotive power,
EMI, connector pinouts, vehicle diagnostics or service resets. No motorcycle,
ECU, OBD interface or external 12 V supply was connected by this work. Stop at
Phase 2; further phases require a separate user request.
