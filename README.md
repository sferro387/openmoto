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
`phase1-verified-loopback` tag. No vehicle/OBD tests performed.

**Phase 3A PASS on real hardware (CAN sniffer):** CAN1 in verified LISTEN_ONLY
(`sniffer_8mhz`; bench variant `sniffer_bench_8mhz` uses CAN2 as test source).
30 s passive capture and 9 serial command checks passed; 7/7 BENCH runs received
all 200 deterministic std/ext/RTR frames with zero controller overruns and zero
serial drops. CAN2 measured at TEC 128 (error-passive, no bus-off) from the
expected no-ACK errors. The prediction that error-active attempts would be
invisible to CAN1 was falsified. Measured limits: ~3,850 8-byte frames/s
lossless count-only; printing at 115200 keeps ~10% of lines on a busy bus. See
[Phase 3A results](docs/phase3a.md) and
[result JSON](tests/phase3a-result-20261023T075313Z.json).

**Phase 3B (high-performance sniffer), benchmarked on real hardware:** 4 MHz SPI,
versioned binary protocol (CRC-16, record sequence numbers, positioned DROP
records), RX-priority output and optional pacing. Lossless reception at full
500 kbps bus load, including 7,840 DLC 0 frames/s (Phase 3A: 513 overrun
events/s). Host capture of ~770 DLC 8 records/s with zero transport loss when
paced to 20 kB/s (production env `p3b_sniffer`). Unpaced output was faster
(~1,600/s) but showed sporadic single-byte loss in the stock 16U2 USB bridge
(always detected). Failures and unresolved limits are documented in
[Phase 3B results](docs/phase3b.md) and the
[result JSON](tests/phase3b-result-20261023T095738Z.json).
