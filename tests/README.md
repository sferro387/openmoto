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
