# OpenMoto

Open motorcycle diagnostics platform. Phase 1: USB-connected Mega 2560 plus one
MCP2515 in internal loopback. No motorcycle communication or service resets.

For the Kiro migration, start with [HANDOFF.md](HANDOFF.md) and [AGENTS.md](AGENTS.md).

Detailed references: [bring-up](docs/bringup.md), [wiring](docs/hardware.md), and
[architecture](docs/architecture.md).

Status: **Phase 1 PASS**. The 8 MHz firmware compiled, uploaded with flash
verification, and produced READY plus 20 consecutive matching internal loopback
frames (sequences 0–19). See [captured serial output](tests/loopback-8mhz-verified-serial.log).
Use `./scripts/pio` for the project-local CLI/cache. See docs/bringup.md for setup.
Phase 1 validates SPI/controller loopback; physical CAN was subsequently validated
by Phase 2 below. Vehicle diagnostics remain untested.

Targets for future verified support: BMW R18 (2021), BMW R1200GS LC (2015),
Aprilia Tuono (model/year pending). These are goals, not supported vehicles.

**Phase 2 PASS on real hardware:** two MCP2515/TJA1050 modules in NORMAL mode,
500 kbps with 8 MHz oscillators; 100 + 1000 frames each direction, **2200/2200
transmitted, received and verified**, zero reported errors/timeouts. Both
controllers returned to CONFIG after the one-shot suite. See the
[actual serial capture](tests/phase2-serial-20261023T062731Z.log), [result report](tests/phase2-result-20261023T062731Z.json), and
[dual-controller guide](docs/phase2.md). Phase 1 remains preserved by the
`phase1-verified-loopback` tag. No vehicle/OBD tests or Phase 3 work performed.
