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
