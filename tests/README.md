# Phase 1 verification

**PASS: confirmed 8 MHz module, one MCP2515 + TJA1050, ELEGOO Mega 2560 R3.**

- PlatformIO 6.1.18 CLI and configuration verified; pip check passed.
- User installed Rosetta; 8 MHz build succeeded with pinned dependencies.
- Flash 5932 bytes; static RAM 239 bytes.
- Upload to /dev/cu.usbmodem21101 succeeded; ATmega2560 signature 0x1e9801.
- avrdude verified all 5932 flash bytes.
- Serial capture at 115200: READY followed by sequences 0–19, 20 consecutive PASS
  records, no FAIL. Capture process exited successfully.

See [raw serial evidence](loopback-8mhz-verified-serial.log) and
[acceptance summary](phase1-result.json). Python dependency snapshot is in
../requirements-tooling.lock.txt; firmware dependency pins are in platformio.ini.

Initial builds were blocked by the Intel compiler before Rosetta installation.
16/20 MHz builds were not repeated because the actual module is confirmed 8 MHz.
Physical CAN signaling, transceiver, INT, oscillator frequency measurement,
vehicle diagnostics, and service resets remain untested.

## Phase 2 software preflight

`python3 tests/test_phase2_firmware.py` compiles actual Phase 2 firmware against
minimal host-only hardware stubs; 21 clean/fault cases pass. Cases include absent
controller, shared CS, wrong bit timing/mode, missing/duplicate/malformed frames,
controller errors, pending TX completion timeout and stop failure. These are
software checks, not physical CAN results. Phase 2 builds successfully; actual
upload, resistance measurements and physical tests are pending. See phase2-preflight.json
and docs/phase2.md. Phase 1 evidence above remains unchanged.

Latest Phase 2 preflight: user reports both 8 MHz modules and wiring checks.
Rebuild PASS; 9746 bytes flash, 361 bytes static RAM. See
phase2-build-20261023T062058Z.log. Port enumerated without opening it. Actual
Phase 2 upload and physical testing remain NOT RUN, awaiting explicit approval.

## Phase 2 actual hardware result: PASS

See [phase2-serial-20261023T062731Z.log](phase2-serial-20261023T062731Z.log) for complete raw MCU
output and [phase2-result-20261023T062731Z.json](phase2-result-20261023T062731Z.json) for parsed
counts, durations, counters and evidence hashes. All 9746 flash bytes were
verified. Both SPI/bitrate/NORMAL initialization checks passed. Initial 100 and
reliability 1000 frames each way all passed: 2200 TX/RX/verified, zero reported
errors/timeouts, suite 18996 ms. Both controllers stopped in CONFIG. No vehicle,
OBD or Phase 3 work. Previous pending-status notes above describe earlier preflight.

## Phase 3A actual hardware result: PASS

Final run `20261023T075313Z`: passive 30 s, serial commands, BENCH ×2 (requested)
and BENCH ×5 (repeatability) PASS; FLOOD measurement recorded. Parsed summary,
firmware HEX/source hashes and log hashes: [phase3a-result-20261023T075313Z.json](phase3a-result-20261023T075313Z.json).
Earlier Phase 3A runs (`073111Z`, `073153Z`, `073909Z`, `074449Z`, `074927Z`) are
preserved failure/iteration evidence from superseded firmware revisions; see
the investigation history in [docs/phase3a.md](../docs/phase3a.md). The
`073909Z` firmware/validator reported PASS under criteria that were later found
too weak. It is not a valid PASS. BENCH duplicates and CAN2 ACK errors are
expected on this two-node no-ACK bench. No vehicle, OBD or later Phase 3 work.

## Phase 3B benchmark evidence

Run directories `phase3b/<label>-<UTC>/` (each with `upload.log` holding the
flashed HEX SHA-256). Final results come from `phase3b/final-*`; all other
directories are investigation history, including failures (see
[docs/phase3b.md](../docs/phase3b.md) §7). Phase 3A baseline reproduction:
`phase3b-baseline-*-20261023T080831Z.log`. Summary:
[phase3b-result-20261023T095738Z.json](phase3b-result-20261023T095738Z.json).
Host tests: `test_phase3b_protocol.py` (host-only, not hardware results).
Raw captures are committed gzip-compressed; decoded `*.frames.jsonl` files are
not committed and are regenerated with `capture_phase3b.py --from-raw`.

## Vehicle-readiness checks (software only)

`check_p3b_sniffer_passive.py` statically audits the linked `p3b_sniffer` ELF
(no transmit or non-silent mode path). See [docs/vehicle-readiness-r18.md](../docs/vehicle-readiness-r18.md).
This is not an electrical safety result.

## Phase 4 read-only diagnostics (offline only)

`test_diag.py` drives the offline simulator (`diag_host/diag_sim.cpp`, which
links the real firmware core `src/diag/diag_core.h`) against an in-process mock
ECU: policy agreement, every request/response and error flow, the ARM/CONFIRM
and emergency-stop safety machinery, the DRYRUN no-transmit build, the strict
addressing-profile loader, and the CLI with its real-hardware guards.
`check_diag_firmware.py` statically audits the built `diag_dryrun` (no
NORMAL/transmit code linked) and `diag_readonly` (NORMAL mode reachable only via
`diagHalEnterNormal`) ELFs. Both are software/offline results, not hardware or
vehicle results, and the verdict for active diagnostics is NO-GO; see
[docs/diag-active.md](../docs/diag-active.md). Build the simulator with
`clang++ -std=c++17 -DOPENMOTO_DIAG_ALLOW_TX=1 tests/diag_host/diag_sim.cpp -o tests/diag_host/.build/diag_sim`
(the test harness builds it in a temp dir itself).

## KOMBI service-reset trace replay (offline only)

`test_diag_replay.py` exercises the offline diagnostic-trace tooling
(`host/python/openmoto_diag/trace.py` + `replay.py`): ELM327/STN log import,
CAN/ISO-TP import with BMW extended addressing and multi-frame reassembly, JSON
round-trip, replay of the verified read-only KOMBI trace, and the read-only
guard that BLOCKS the recovered `2E E1 2B/2C/2D` service-reset writes (and only
dry-runs them, never transmitting, when explicitly asked). Sample fixtures are
under `tests/diag_traces/`; the verified traces live in
`host/python/openmoto_diag/traces/`. Findings and evidence references:
[docs/diag-kombi-findings.md](../docs/diag-kombi-findings.md). Offline only; not
a hardware or vehicle result. The verdict for a KOMBI service reset is NO-GO.

## Phase 5 KOMBI read-only probe (offline only)

`test_diag_kombi_probe.py` exercises the first KOMBI read-only probe (`22 F1 50`)
against the real firmware core compiled with `OPENMOTO_DIAG_KOMBI_PROBE=1`: the
exact request frame (`6F1 / 60 03 22 F1 50 00 00 00`), response reassembly on
`0x660` with the `0xF1` extension byte, the compile-time/host `--kombi-probe` gate
(denied by default), writes staying denied even in the probe build, a no-ACK bus
failing cleanly as `TX_FAILED` (no bus-off storm), and the CLI refusing the
APK-derived (unverified) R18 profile. `check_diag_firmware.py` now also audits the
`diag_kombi_probe_dryrun` (no transmit code) and `diag_kombi_probe` (NORMAL only
via `diagHalEnterNormal`) images; the Phase 4 `diag_dryrun`/`diag_readonly` HEX
hashes are unchanged. Offline only; not a hardware or vehicle result. The verdict
for a real transmission is NO-GO — see [docs/diag-kombi-probe.md](../docs/diag-kombi-probe.md).

## Phase 6 ACK-only CAN bring-up (offline only)

`test_diag_ack.py` drives the real ACK-only bring-up core
(`firmware/mega-can/src/diag_ack/ack_core.h`) against a simulated CAN controller
and checks the four bus conditions are distinguished (healthy /
no-ACK-was-missing-ACKer / wrong-bitrate / no-connection), that boot is
IDLE/SILENT with no auto-NORMAL, that ARM+CONFIRM is required (fresh each run,
wrong code refused), that STOP and `!`/ESTOP return to silent (estop latches),
and that an SPI fault forces fallback. `check_diag_ack_firmware.py` statically
audits the `diag_ack` ELF: no transmit/RTS/load-TX path linked (every
`SPI.transfer` caller is a register/read/mode helper), NORMAL mode present so it
genuinely acknowledges, and the RTS-pin/TXREQ guards present. ACK-only is ACTIVE
bus participation; these are offline/software results, not a hardware or vehicle
result. Verdict is GO-conditional for a short supervised test — see
[docs/diag-ack-bringup.md](../docs/diag-ack-bringup.md).

## Phase 7 KOMBI service-read (offline only)

`test_diag_service_read.py` drives the real firmware core (service-read build)
against a mock ECU: the four service DIDs `22 E1 19/2B/2C/2D` are allowed only
with `OPENMOTO_DIAG_SERVICE_READ=1` / host `service_read=True` and denied by
default (firmware + host agree); each uses the bike-verified frame
`6F1 / 60 03 22 E1 xx 00 00 00`; E119/E12B/E12C reassemble via one bounded
flow-control frame while E12D is single-frame; writes/reset/security and
neighbouring DIDs stay denied. `check_diag_firmware.py` now also audits
`diag_service_read_dryrun` (no transmit code) and `diag_service_read` (NORMAL
only via `diagHalEnterNormal`); Phase 3B/4/5/6 HEX unchanged. Offline/software
results only. Raw response bytes are recorded verbatim; field decodes stay
[APK]/[INFERRED] until bike-confirmed. See
[docs/diag-kombi-service-read.md](../docs/diag-kombi-service-read.md).

## Phase 8 service-reset codec (offline, no transmit)

`test_service_codec.py` tests the reverse-engineered KOMBI service-write
encoder/decoder (`host/python/openmoto_diag/service_codec.py`): the decoded real
Phase 7 read-backs re-encode to the exact `2E E1 2B/2C/2D` payloads; encode<->decode
round-trips are stable; the real values decode self-consistently (clock/date year
both 2026); out-of-range fields are rejected; and the preserved rollback reference
(`tests/diag_service/rollback-reference.json`) is byte-exact. Pure byte
manipulation — nothing is transmitted and no firmware write path is enabled. See
[docs/diag-kombi-service-reset.md](../docs/diag-kombi-service-reset.md) for the
APK-confirmed vs assumption split.

## Phase 9 service-write (offline, no transmit)

`test_diag_write.py` drives the real service-write core
(`firmware/mega-can/src/diag_write/write_core.h`, a SEPARATE tree so read builds
stay byte-identical) against a write-capable mock KOMBI
(`tests/diag_write_host/write_sim.cpp`): single-frame (E12D) and ISO-TP
multi-frame (E12C/E12B) writes succeed and are read-back-verified; NRC/pending/
FC-timeout/no-response are handled; READBACK_MISMATCH is caught with a rollback
alert; only `2E E1 2B/2C/2D` are armable; estop latches; the dry-run build
transmits nothing. Offline only; nothing is sent to a vehicle. Target write
values and the stop-before-write report are in
[docs/diag-service-write-plan.md](../docs/diag-service-write-plan.md).
