# Phase 4 design: read-only active-diagnostics client

This is the engineering companion to [docs/diag-active.md](diag-active.md) (the
evidence report and GO/NO-GO). It records how the read-only diagnostic client is
built and why. Nothing here authorizes transmitting to a vehicle; see the
prerequisites and verdict in the evidence report.

## Goals and non-goals

Goal: a minimal capability to issue explicitly allowlisted, non-mutating UDS
read requests over ISO-TP, with operator confirmation of every transmission, and
to prove it safe offline. Non-goals (explicitly out of scope this phase):
service resets, coding, adaptations, security access, routine control, memory
or any write; and any transmission to the actual motorcycle.

## Module layout

```
firmware/mega-can/src/diag/
  diag_core.h   portable core: policy, ISO-TP, validation, shell, safety
  main.cpp      Arduino/MCP2515 hardware layer (the only transmit code)
host/python/
  diag_cli.py                 entry point
  openmoto_diag/protocol.py   allowlist mirror + transcript parsing
  openmoto_diag/profile.py    strict addressing-profile loader
  openmoto_diag/transport.py  serial + simulator line transports
  openmoto_diag/cli.py        session logic + hardware guard
  openmoto_diag/profiles/     r18-UNVERIFIED.template.json, simulator.json
tests/
  diag_host/diag_sim.cpp      mock ECU + host harness (links the real core)
  test_diag.py                offline behaviour tests
  check_diag_firmware.py      static audit of the built ELFs
```

The core header compiles unchanged for both the ATmega2560 (avr-g++, the
firmware) and the host (clang++, the simulator). That is deliberate: the exact
state machine that would run on the board is the one exercised by the tests,
so the tests are testing the firmware logic, not a reimplementation.

## Why a shared core + thin HAL

The risky parts (mode changes, transmit) are isolated behind a `diag::Hal`
interface with three implementations' worth of behaviour selected at compile
time:

- firmware `diag_dryrun` — `canEnterActive/canTransmit` are stubs; **no** NORMAL-
  mode or transmit code is compiled in. ARM/CONFIRM run; CONFIRM yields
  `status=DRYRUN` with `frames_tx=0`.
- firmware `diag_readonly` — the real one-shot NORMAL-mode + transmit path.
- host simulator — a `SimHal` that routes frames to an in-process mock ECU and a
  simulated clock, so the 6 s timeouts resolve instantly.

Because the HAL is the only thing that differs, a logic bug in the state machine
shows up identically offline and on hardware.

## ISO-TP and UDS scope

- Requests are single-frame only (all allowlisted requests are ≤ 7 payload
  bytes). The client sends at most one request frame plus, for a multi-frame
  response, one flow-control frame — a hard cap of two transmitted frames.
- Responses may be single- or multi-frame; the core reassembles consecutive
  frames with sequence-number checking, honours `responsePending` (NRC 0x78) up
  to a small bound, and validates the positive/negative response shape against
  the request before reporting `OK`/`NEGATIVE`.
- Both normal and extended (address-extension-byte) ISO-TP addressing are
  supported via `ADDR`, because the R18's mode is unknown and may be extended.

## Confirmation code

`ARM` computes a 16-bit code with FNV-1a over the exact frame bytes the firmware
will transmit, plus a per-arm counter and the current time. The firmware prints
it; `CONFIRM <code>` must echo it. This ties the human's approval to the precise
frame, makes confirmations single-use and time-limited, and means a replayed or
guessed code does not fire a transmission. Three wrong codes trip the emergency
stop.

## Host-side defence in depth

The host re-implements the same allowlist (`protocol.check_request`) and refuses
a forbidden or unverified request before anything is sent, even though the
firmware would also refuse it. The profile loader refuses any profile that is not
explicitly `verified:true` with complete, evidenced addressing — this is the
mechanism that keeps guessed IDs out of the transmit path. The `--port` transport
is gated behind `--i-have-read-prereqs`, an interactive `TRANSMIT` acknowledgement
and per-request confirmation, and disallows `--yes`.

## Testing strategy

Offline only. The mock ECU has fault modes for each error path so the state
machine's branches are all driven (see `tests/test_diag.py`). The firmware audit
(`tests/check_diag_firmware.py`) is a build-level proof about the linked image,
complementing the behavioural tests. "Build/mock PASS is distinct from a real
hardware PASS" (project policy) applies in full: none of these are vehicle
results.

## Known limits

- No real ISO-TP timing, arbitration, or ACK behaviour is exercised — the
  simulator delivers frames instantly and always wins arbitration.
- The allowlisted DIDs are the ISO 14229-1 identification range only; no
  manufacturer-specific R18 DID is included, because none is verified.
- The addressing profile for the R18 is a template with no real IDs; see the
  evidence report for why, and for what must be verified before use.
