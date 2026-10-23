# Architecture

Phase 1 path: Mac USB serial -> Mega Arduino firmware -> hardware SPI -> MCP2515
internal transmit/receive buffers. TJA1050 and physical CAN bus are outside the
loopback path. Only the upstream autowp MCP2515 library and framework SPI are used.

Firmware checks reset, bitrate configuration, and loopback entry; sends one
standard 0x123 eight-byte frame per second with a changing sequence; checks ID,
DLC, all payload bytes, receive timeout, and controller error flags. Any failure
prints FAIL and halts until reset. There is no normal-mode fallback or serial
command that enables bus transmission.

A passing test demonstrates basic MCU execution, serial output, and SPI/controller
loopback operation. It does not prove physical CAN signaling, transceiver health,
interrupt wiring, exact oscillator speed, vehicle bitrate, OBD pinout, diagnostic
protocols, or service reset safety.

Later phases may separate CAN transport, ISO-TP, vehicle-specific diagnostics,
verified service operations, and BLE/mobile transport. Require independent
vehicle-specific evidence and tests before implementing service writes. The
ATmega2560 proof of concept is not the commercial dongle hardware selection.

## Phase 2 (passed on real hardware)

A separate `physical_dual_8mhz` environment compiles only src/phase2/main.cpp.
Two library instances with separate D10/D9 chip selects share hardware SPI at
1 MHz. Both use 500 kbps, 8 MHz configuration and verified NORMAL mode; SPI raw
register access adds independent chip read/write checks and TX completion/mode
readback without changing the upstream library. Physical communication runs
100 + 100 + 1000 + 1000 frames in four paced stages and stops at the first error.
No Phase 2 loopback code path exists. Both controllers return to CONFIG after
the suite. See phase2.md for termination, methodology and hardware approval gates.

The original Phase 1 source remains untouched. Each Phase 1 environment explicitly
selects it using build_src_filter; Phase 2 sources are excluded. Reproduce Phase 1
on its original single-module wiring, not the new dual-controller configuration.

The authorized real Phase 2 run passed: 2200 transmitted/received/verified frames,
zero reported errors/timeouts, and both controllers stopped in CONFIG. Actual
evidence is linked in bringup.md and phase2.md.

## Phase 3A (passed on real hardware)

`sniffer_8mhz` and `sniffer_bench_8mhz` compile only src/phase3a/main.cpp. CAN1
runs in verified LISTEN_ONLY with accept-all filters and is serviced from its INT
line; the receive path only copies frames into a RAM queue; compact text lines are
printed later without blocking, and drops/overruns are counted. CAN1 has no
transmit path. CAN2 is silent in CONFIG, except during the bench build's BENCH
and FLOOD commands. See phase3a.md for results and measured limits.
