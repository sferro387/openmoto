# OpenMoto

Open motorcycle diagnostics platform. Phase 1: USB-connected Mega 2560 plus one
MCP2515 in internal loopback. No motorcycle communication or service resets.

Start with [bring-up](docs/bringup.md), [wiring](docs/hardware.md), and
[architecture](docs/architecture.md).

Status: **Phase 1 PASS**. The 8 MHz firmware compiled, uploaded with flash
verification, and produced READY plus 20 consecutive matching internal loopback
frames (sequences 0–19). See [captured serial output](tests/loopback-8mhz-verified-serial.log).
Use `./scripts/pio` for the project-local CLI/cache. See docs/bringup.md for setup.
This validates SPI/controller loopback; physical CAN and vehicle diagnostics
have not been tested.

Targets for future verified support: BMW R18 (2021), BMW R1200GS LC (2015),
Aprilia Tuono (model/year pending). These are goals, not supported vehicles.
