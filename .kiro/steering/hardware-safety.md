---
inclusion: always
---
# Hardware scope

The allowed baseline is the existing USB-only Mega 2560 plus two MCP2515/TJA1050
modules on an isolated bench. No motorcycle, ECU, OBD cable, external CAN device,
external 12 V, vehicle diagnostic requests, ECU changes or firmware flashing is
authorized. Phase 3A sniffer CAN1 must stay LISTEN_ONLY and never transmit.
Phase 3A firmware upload requires explicit user approval (user instruction for
Phase 3A), notwithstanding the standing bench approval below.

Disconnect all power before changing wiring or measuring resistance; verify no
voltage before switching the meter to ohms. Confirm both 8 MHz crystals, independent
D10/D9 chip selects, shared SPI/common ground, H-to-H/L-to-L and termination before
powering changed hardware. Exact wiring/procedure is in docs/phase2.md.

Recorded termination NETWORK measurements are 109.6/109.8 ohms, combined 54.9–55.0;
nominal two 120-ohm terminations would give 60. Do not silently relabel actual
measurements or add resistors. Stop for shorts, unexplained changes or missing/
extra termination. Historical bench PASS is not blanket approval for new wiring.

The user has granted standing approval to upload bench firmware and run bench
tests without asking. Before each upload/run, identify the USB device with
`./scripts/pio device list` (Mega VID:PID 2341:0042) and pick the firmware
environment matching the current wiring. Do not open serial just to enumerate it:
DTR can reset and rerun NORMAL-mode transmissions.
If initialization/communication fails, preserve diagnostics and stop. Do not
bypass a physical failure with loopback or claim success from a completed upload.
