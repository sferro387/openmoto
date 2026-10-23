---
inclusion: always
---
# Technology

Use the existing `./scripts/pio` wrapper from the repository root. It selects
PlatformIO Core 6.1.18 in `.tools/pio-venv` and the local package/cache directory.
Do not reinstall PlatformIO or change Python/framework/library pins for migration.

`firmware/mega-can/platformio.ini` is authoritative: atmelavr 5.1.0,
megaatmega2560, Arduino; upstream autowp MCP2515 commit
`49e0d7d6755bab09cc3e68997bfdb75264c8daf7`. Other tool pins and environment details
are in HANDOFF.md. The Apple Silicon Mac already has Rosetta for Intel AVR tools.
Python 3.9.6/LibreSSL emits a known urllib3 warning; do not disguise or silently fix it.

Build-only checks:

```sh
./scripts/pio run -d firmware/mega-can -e loopback_8mhz
./scripts/pio run -d firmware/mega-can -e physical_dual_8mhz
python3 tests/test_phase2_firmware.py
```

Always specify the environment; preserve source filters and Phase 1 reference.
The actual modules are 8 MHz; both Phase 2 controllers use 500 kbps NORMAL mode,
1 MHz shared SPI, and 115200 serial. Host scripts use the existing
`host/python/.venv` and pyserial 3.5. Host C++ mock tests use Apple clang++.
Build/mock PASS is distinct from a real hardware PASS; hardware access requires
its own authorized scope. No upload, serial reset or hardware test on IDE startup.
