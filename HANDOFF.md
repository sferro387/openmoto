# OpenMoto handoff to Kiro

## Current state and scope

Open this exact project directory in Kiro: **`/Users/sergio/src/openmoto`**.
The terminal working directory for commands below is the repository root.

**Phase 1 and Phase 2 passed on real hardware. Phase 3 has not started.** This
handoff changes documentation and assistant guidance only. Working firmware,
PlatformIO configuration/dependencies, existing scripts and evidence are preserved.
No firmware upload, serial-port opening, reset or new physical test is required
for migration. The last observed board firmware is `physical_dual_8mhz`; both CAN
controllers reported CONFIG when its one-shot suite finished. Present live state
has not been re-probed by resetting the board during migration.

Phase 2 validates physical CAN communication between two MCP2515/TJA1050 modules
controlled by one Mega. **No vehicle connection, ISO-TP, UDS, OBD diagnostics, BMW
service reset, ECU configuration or motorcycle firmware flashing has been
implemented and validated by this work. No vehicle compatibility is claimed.**

## Product purpose and roadmap

OpenMoto aims to be an open, extensible motorcycle diagnostic platform, ultimately
a commercial BLE dongle with iOS/Android applications and independently verified,
vehicle-specific service interval reset operations. The present platform is a
USB-powered embedded bench proof of concept, not the commercial hardware design.

Future targets, not supported vehicles:

| Vehicle | Target year / uncertainty |
|---|---|
| BMW R18 | 2021 |
| BMW R1200GS LC | 2015 |
| Aprilia Tuono | Exact model/year unconfirmed |

Recommended milestones are proposals only, not authorization to start Phase 3:

1. Review this evidence/checkpoint in Kiro; agree the next phase's acceptance
   criteria with the user before code or hardware work.
2. Bench-only host transport/data logging and simulated ECU/ISO-TP experiments,
   with deterministic segmentation, flow-control and timeout tests.
3. Vehicle interface research/design: exact model/connector/protocol evidence,
   automotive supply protection, transients/ESD and suitable dongle hardware.
4. A separately authorized and reviewed listen-only vehicle observation plan;
   bench normal-mode firmware must not be treated as a passive vehicle logger.
5. Verified read-only diagnostics after vehicle addressing/protocols are proven.
6. Separately validated and explicitly authorized service operations, including
   documented prerequisites, request/response semantics and readback.
7. BLE authentication/versioned API, mobile applications, firmware recovery/update,
   manufacturing and commercial product qualification.

Do not execute any of these milestones as part of this migration.

## Architecture and file map

```text
openmoto/
├── AGENTS.md                     Cross-editor project instructions
├── HANDOFF.md                    Current handoff; start here
├── .kiro/steering/               Focused Kiro product/tech/structure/safety guidance
├── firmware/mega-can/
│   ├── platformio.ini            Existing pinned environments; explicit source filters
│   └── src/
│       ├── main.cpp              Phase 1: Arduino setup()/loop(), internal loopback
│       └── phase2/main.cpp       Phase 2: Arduino setup()/loop(), physical CAN suite
├── host/python/
│   ├── capture_loopback.py       Phase 1 serial evidence capture
│   ├── capture_phase2.py         Phase 2 capture and complete-result validation
│   ├── requirements.txt          pyserial==3.5
│   └── .venv/                    Existing local host Python, ignored by Git
├── scripts/pio                  PlatformIO wrapper; use this CLI
├── requirements-tooling.lock.txt Python tooling snapshot
├── docs/
│   ├── hardware.md              Phase 1 wiring plus Phase 2 pointer
│   ├── architecture.md          Bench architecture
│   ├── bringup.md               Chronological setup and actual hardware results
│   └── phase2.md                Dual wiring, termination, method and real results
├── tests/
│   ├── README.md                Evidence index and software/hardware distinction
│   ├── phase2_mock/             Host-only MCU/controller stubs and C++ harness
│   ├── test_phase2_firmware.py  Existing host fault/capture acceptance checks
│   └── *.log, *.json            Preserved actual evidence and result records
└── .tools/                      Existing local Core venv/packages, ignored by Git
```

Flow: Mac USB serial ↔ Mega firmware ↔ shared hardware SPI ↔ MCP2515 controllers.
Phase 1 loops inside one MCP2515. Phase 2 traverses TJA1050 transceivers and the
physical CAN-H/L pair, with the other controller acknowledging and receiving.
There is no ISO-TP layer, diagnostic service abstraction, BLE transport or mobile
application in this repository.

## Hardware and exact wiring

- ELEGOO Mega 2560 R3 / ATmega2560; USB-connected to this Mac.
- Two MCP2515 + TJA1050 CAN modules, 5 V supply and SPI logic.
- Both module oscillators confirmed **8 MHz** by user; #1 marking `8000`.
  #2 frequency was confirmed but its complete printed marking was not recorded.
- Breadboard/jumpers and a short H/L twisted pair are specified in the bench
  procedure; pair construction/length has not been independently documented.
- OBD breakout cable exists as available hardware but is **not connected**.

| Signal | Mega 2560 | Module #1 | Module #2 |
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

D53 is OUTPUT/HIGH to preserve SPI master mode. Phase 2 initializes both CS
latches HIGH before SPI starts and uses separate library instances at **1 MHz
SPI**. INT pins are inputs, but firmware polls reception: interrupt delivery has
not been tested. CAN bitrate is separately configured to **500 kbps**.

```text
Module #1 CAN-H ────────── Module #2 CAN-H
Module #1 CAN-L ────────── Module #2 CAN-L
Module #1 GND  ── Mega ─── Module #2 GND
          termination at both bus ends
```

User-reported checks before the authorized Phase 2 run:

| Measurement/check | Actual report |
|---|---|
| Module #1 termination network | 109.6 Ω |
| Module #2 termination network | 109.8 Ω |
| Completed bus CAN-H to CAN-L | 54.9–55.0 Ω |
| H-to-H and L-to-L continuity | PASS |
| Direct H/L short | None reported |
| 5V/GND short | None reported |
| SPI/CS/INT/power/ground wiring | Manually checked |
| Power and external connections | USB only; no motorcycle or OBD |

Nominal two-end termination is 120 Ω per end, giving approximately 60 Ω combined.
The **measured networks are not documented as measured 120 Ω resistor components**.
109.6 Ω parallel with 109.8 Ω predicts about 54.85 Ω, consistent with the measured
bus. The reason for the lower network readings remains unresolved. The real
bench run passed under those measured conditions; it does not explain component
values or certify a standardized bus load. See `docs/phase2.md` for the unpowered
meter procedure. Disconnect all power before wiring/resistance measurements;
stop for shorts, missing/extra termination or unexpected readings.

## Existing development environment and builds

Observed Mac: macOS 27.0.1 (26A434), arm64; Apple command-line tools, Git and
Homebrew present. Python 3.9.6 supplied by Apple command-line tools. Rosetta was
installed by the user for the Intel AVR compiler. Do not reinstall or migrate
these working tools merely to open the project in Kiro.

`./scripts/pio` runs `.tools/pio-venv/bin/pio` (Core **6.1.18**) and exports a
repository-local `PLATFORMIO_CORE_DIR` before Core startup. `platformio.ini`
alone did not prevent early Core state access to the home directory. The separate
`/Users/sergio/platformio-venv` Core 6.2.0 is not used by this project.

| Configuration | Pinned value |
|---|---|
| Platform / board / framework | `atmelavr@5.1.0` / `megaatmega2560` / `arduino` |
| AVR toolchain | `platformio/toolchain-atmelavr@1.70300.191015` (GCC 7.3.0) |
| Arduino framework | `platformio/framework-arduino-avr@5.2.0` |
| SCons | `platformio/tool-scons@4.40801.0` |
| Uploader | `platformio/tool-avrdude@1.60300.200527` |
| MCP2515 library | `autowp/arduino-mcp2515`, commit `49e0d7d6755bab09cc3e68997bfdb75264c8daf7` (v1.3.1) |
| Serial baud | `115200` |

Build-only commands (no upload or serial reset):

```sh
./scripts/pio --version
./scripts/pio run -d firmware/mega-can -e loopback_8mhz
./scripts/pio run -d firmware/mega-can -e physical_dual_8mhz
python3 tests/test_phase2_firmware.py
```

Use explicit `-e`: `loopback_8mhz` selects only `src/main.cpp`;
`physical_dual_8mhz` selects only `src/phase2/*.cpp`. `loopback_16mhz` and
`loopback_20mhz` remain available build variants but are not validated with the
present 8 MHz hardware. The Mega CPU's own 16 MHz clock is not the module crystal.

Migration build verification: both verified 8 MHz environments **PASS**; see
`tests/handoff-build-20261023T063359Z.log`. Phase 1 uses 5932 bytes flash / 239
bytes static RAM; Phase 2 uses 9746 / 361. Existing host checks also pass: 21
clean/fault scenarios and 8 corrupted/truncated capture rejections. These host
checks simulate controller behavior and are not new physical test results.

The environment/cache and host venv are local, ignored, and not portable by
copying a Git clone. A future fresh machine needs deliberate setup from
`requirements-tooling.lock.txt`, `host/python/requirements.txt` and pinned
PlatformIO packages; no such reinstallation is needed on this Mac.

## Firmware entry points and execution

**Phase 1 — `firmware/mega-can/src/main.cpp`:** `setup()` configures SPI/D53,
resets one controller, sets bit timing and enters internal loopback. `loop()`
sends a standard ID `0x123`, DLC 8, changing sequence about once per second;
checks exact receive data, a 250 ms timeout and error flags; halts on failure.
Use the original single-module wiring to reproduce it. Phase 1 does not initialize
D9 for a second chip and should not be used to test the dual-module assembly.

**Phase 2 — `firmware/mega-can/src/phase2/main.cpp`:** `setup()` runs the entire
one-shot suite; `loop()` is empty. Functions provide these stages:

1. `initialize()`: reset/CONFIG, swapped independent CNF1 SPI write/read challenges,
   restore/read back 500 kbps / `MCP_8MHZ` timing, then verify both NORMAL transitions
   using library results and register readback. No loopback path exists.
2. `runTest()` / `exchange()`: B: 100 #1→#2 frames (`0x123`); C: 100 #2→#1
   (`0x456`); D: 1000 each way in sequence. Later stages require earlier success.
3. `makeFrame()` / `receiveExpected()`: DLC 8, `4F 4D`, stage, direction, then
   32-bit little-endian sequence. Check ID/flags/DLC/all payload bytes/order;
   a per-stage bitmap catches duplicates. Unexpected sender reception also fails.
4. One TXB0 frame outstanding: queue success is not completion. Require TXREQ clear
   and TX0IF set plus verified reception at the other NORMAL controller. Bound
   completion/receive to 250 ms; poll both chips during at least 5 ms quiet pacing.
5. `sampleErrors()`: check operating modes, EFLG, ERRIF/MERRF and TEC/REC. Fail on
   observed errors; NORMAL can automatically retry before software notices.
6. `stopControllers()` / `finalResult()`: abort TX/request CONFIG and stop after
   error or final result. No continued traffic after completion; a reset reruns.

Captured counters are fail-fast: unattempted frames after a failure are not
counted as missing; a missing frame is a timed-out expected receive. Error
observations and TEC/REC peaks are sampled, not a continuous waveform analysis.

## Verified Phase 1 evidence

20 consecutive matching internal-loopback frames, sequence 0–19, no reported
failure. Flash written and verified: **5932 bytes**; MCU signature `0x1e9801`.
This establishes basic SPI/controller loopback, not the physical CAN bus.

- Raw serial: `tests/loopback-8mhz-verified-serial.log`
- Result: `tests/phase1-result.json`
- Source checkpoint: commit **`9b0ac61`**, tag **`phase1-verified-loopback`**
- Original HEX SHA-256: `1f4bb0f521066036af00cf34ec3e4ece92ccaf7de29146d6328cbe2fb0cbbff6`
- Raw serial SHA-256: `ee8bd8069dff48036e8544da6080c61d37c478d603ccdb09851d6532b1505490`

## Verified Phase 2 evidence

The recorded run completed **2026-10-23 06:27:55 UTC** (2026-10-22 Pacific).
Upload and capture exit status 0; all **9746 bytes** written and flash-verified.
Both SPI/bitrate/NORMAL initialization checks passed.

| Stage | Direction | TX complete | Received | Verified | Timeouts | Duration ms |
|---|---|---:|---:|---:|---:|---:|
| B | CAN1 → CAN2 | 100 | 100 | 100 | 0 | 852 |
| C | CAN2 → CAN1 | 100 | 100 | 100 | 0 | 853 |
| D | CAN1 → CAN2 | 1000 | 1000 | 1000 | 0 | 8535 |
| D | CAN2 → CAN1 | 1000 | 1000 | 1000 | 0 | 8535 |
| Total | Both ways | **2200** | **2200** | **2200** | **0** | **18996 suite** |

Requested/queued/transmitted/received/verified all total 2200. Missing, duplicate,
unexpected, payload mismatch, sequence, API and controller error observations
were zero in every stage. Both controllers: TEC/REC, sampled peaks, ORed EFLG
and error interrupt flags all zero. Both stopped in CONFIG before final PASS.

| Evidence | Repository path |
|---|---|
| Complete MCU serial | `tests/phase2-serial-20261023T062731Z.log` |
| Build/upload/readback verification | `tests/phase2-upload-20261023T062731Z.log` |
| Capture output/status | `tests/phase2-capture-20261023T062731Z.log` |
| Process times, command scope, statuses | `tests/phase2-run-20261023T062731Z.json` |
| Counts/counters/durations/hashes | `tests/phase2-result-20261023T062731Z.json` |
| Preflight/build/assembly record | `tests/phase2-preflight.json` |
| Pre-upload build | `tests/phase2-build-20261023T062058Z.log` |

Original Phase 2 HEX SHA-256:
`efd7a1602871970774de550f874cd1fea0ee78184dcd19b5b0fff7f1f9cb754b`.
Raw serial SHA-256:
`a5c0c438d951c4cea898643f4bb5e1aaef0a7006435d7f565e2563130cf6b2b6`.
Evidence hashes were rechecked during migration. Existing logs/results remain
unchanged. Some earlier sections of chronological bring-up docs describe pending
states that were subsequently resolved; use the final actual results and this
handoff for current status.

## Hardware execution is separate from build verification

The previous approved port was `/dev/cu.usbmodem21101`, VID:PID `2341:0042`,
serial `34530303238351110202`. Enumerate again with `./scripts/pio device list`;
port names can change. Do not treat a historical device path as authorization.

Only for a separately requested/authorized bench rerun, after hardware checks and
explicit firmware/target-port approval, use the existing tools:

```sh
# Replace CONFIRMED_USB_PORT and choose a NEW log filename; not part of migration.
./scripts/pio run -d firmware/mega-can -e physical_dual_8mhz -t upload --upload-port CONFIRMED_USB_PORT
host/python/.venv/bin/python host/python/capture_phase2.py --port CONFIRMED_USB_PORT --output tests/NEW_PHASE2_RUN.log
```

Capture uses 115200 baud, opens the port/toggles DTR (reset/retransmit), saves an
exclusive new log and checks four complete summaries and final PASS within 90 s.
Preserve failed runs as evidence. A timeout/incomplete capture is not a PASS.
Never open a monitor merely to inspect metadata: it can reset the Mega.

## Design decisions, limits and unresolved issues

- Keep Phase 1 source and reference tag; isolate phases with explicit source
  filters. Do not change working firmware as part of an editor migration.
- Use unmodified upstream MCP2515 library at a full commit pin; narrowly scoped
  raw register access provides SPI challenges, mode/TX readback, not a new driver.
- Keep serial logging out of the per-frame path; paced sequential traffic and
  one outstanding frame prevent overwhelming the two receive buffers.
- Poll INT-connected controllers; interrupts, simultaneous bidirectional traffic,
  maximum throughput, extended/RTR traffic and long-duration endurance are untested.
- Zero reported errors are sampled observations. No oscilloscope capture,
  independent frequency measurement, long cable, EMI, ESD, automotive transients,
  isolation or production power/interface qualification has been performed.
- Termination network readings below nominal remain unexplained; do not silently
  convert them to claimed 120-ohm resistor measurements or change termination.
- Python 3.9/LibreSSL 2.8.3 emits urllib3 v2's compatibility warning. Builds and
  downloads worked; the warning is not fixed or suppressed. AVR tools need Rosetta
  on this Apple Silicon Mac. Future environment modernization is separate work.
- Host mocks validate selected control/error paths, not real CAN timing or ACKs.
  Test harness depends on Apple `clang++` and uses temporary build artifacts.
- No CI configuration, automated hardware fixture, repository license decision,
  BLE/mobile implementation or commercial qualification is present.
- No vehicle connector mapping, diagnostic addressing, ISO-TP, UDS, OBD operation
  or BMW/Aprilia service reset is validated. Aprilia target details are unresolved.
- No physical rerun or upload occurred during handoff. **Stop at Phase 2.**

## Git checkpoint and Kiro guidance

Before migration, HEAD was the Phase 1 checkpoint. The completed Phase 2 source,
configuration, scripts, docs and real logs were uncommitted. The handoff checkpoint
includes those reviewed project changes plus this migration documentation, rather
than discarding or omitting the working Phase 2 state. Generated `.tools/`, `.pio/`,
venvs and Python caches remain ignored; no credentials or SSH keys are included.

Kiro guidance is in `.kiro/steering/{product,tech,structure,hardware-safety}.md`,
with `inclusion: always`. Root `AGENTS.md` gives concise cross-editor instructions
and links rather than duplicating the handoff. This follows the documented
[Kiro steering and AGENTS.md conventions](https://kiro.dev/docs/steering/).
Open the existing directory, read the guidance, and await the user's next task.
The earlier external `~/Downloads/r18reader.json`/`r18reader/SKILL.md` are historical
Codex handoff exports, not required runtime dependencies or the Kiro source of truth.

After the handoff commit, use `git log -1 --format=fuller`, `git show --stat HEAD`
and `git status --short` for the checkpoint identity, exact file inventory and
working-tree state. The commit cannot embed its own hash in its contents; its
full hash is reported separately to the user.

## GitHub checkout location

The project was transferred with its existing history and Phase 1 tag to
`/Users/sergio/src/openmoto`, cloned from
`https://github.com/sferro387/openmoto.git`. This is now the working checkout.
The original `/Users/sergio/Downloads/openmoto` remains unchanged.

On this Mac, `.tools/pio-venv`, `.tools/platformio` and `host/python/.venv` are
local symbolic links to the existing environments/cache in the original Downloads
checkout. This keeps `./scripts/pio` and host capture commands usable without
reinstalling tools or copying virtual environments. These links are ignored
(the host venv link is excluded locally in `.git/info/exclude`) and are not
published. **Do not delete the Downloads tooling while these links are in use.**
A fresh clone on another machine requires its own deliberate tooling setup; these
machine-local links are not a portable environment bundle. No firmware or hardware
evidence was changed by transferring the checkout.
