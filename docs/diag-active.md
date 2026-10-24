# Phase 4: active read-only diagnostics for a 2021 BMW R18 — evidence report

Date: 2026-10-24. Scope: research, design and OFFLINE implementation of a
minimal, read-only UDS diagnostic client for a 2021 BMW R18, plus the exact
prerequisites and a GO / NO-GO decision for the first real request. **Nothing in
this phase was transmitted to the motorcycle.** The bike is disconnected. All
results here are from documents, from the existing passive capture, or from the
offline simulator — never from a live bus.

## Verdict: **NO-GO for active diagnostics on the motorcycle**

The firmware, host client, policy and tests are complete and pass offline, but
the two things that make active diagnostics *safe and meaningful* are still
missing and cannot be invented:

1. **No verified diagnostic CAN addressing exists for the R18.** No request/
   response CAN IDs, ISO-TP addressing mode, or diagnostic bitrate were found
   from a primary source, and none can be derived from the capture we have
   (section 3). Transmitting without them is guessing.
2. **The bus behaviour at the diagnostic connector is unverified.** The only
   observed traffic (`0x130`) is not diagnostic and is consistent with a bus
   where our node would be the sole transmitter and get no acknowledgment
   (section 4). Transmitting into that is how you end up error-passive/bus-off.

Clear the prerequisites in section 7, fill in and verify an addressing profile,
then re-evaluate. Until then the only authorized build is the no-transmit
`diag_dryrun`, and even `diag_readonly` is build-only.

---

## 1. R18 diagnostic architecture (what the evidence supports)

Findings, with sources. Where a claim is about BMW cars or other Motorrad models
rather than the R18 specifically, it is labelled as such — this matters, because
Phase 3B already showed we must not assume car behaviour carries to this bike.

- **The R18 is diagnosed over a BMW Motorrad diagnostic connector, not a generic
  OBD-II CAN port.** Riders reset the R18 service reminder with a HEX GS-911 (via
  the Motorrad adapter) or MotoScan with an OBDLink adapter; a plain OBD-II
  scanner is not described as sufficient. See the R18 owners' community threads
  on resetting the service light
  ([r18forums, service warning](https://r18forums.com/threads/is-it-possible-to-reset-service-warning.277/),
  [r18forums, MotoScan + wrench icon](https://r18forums.com/threads/motoscan-and-the-dreaded-wrench-icon.4794/)).
  BMW's own tool chain (ISTA/ISTA+) connects to the R18 through an ICOM with the
  Motorrad cable ([R18 forum, ISTA connects](https://www.r18forums.com/threads/unable-to-connect-bimmer-utility-or-esys.4507/);
  [Maverick Diagnostics, ICOM Next D 10-pin Motorrad cable](https://www.maverickdiagnostics.com/guide/bmw-group/)).
  *Content rephrased for licensing compliance.*
- **BMW Motorrad connectors carry both K-line and CAN.** Documented BMW-bike
  adapters are "10-pin → OBD2 (K-lines + CAN)"
  ([kds-online adapter listing](https://kds-online.de/de/obd-adapter-bmw-motorrad-10-pin-auf-obd2-k-lines-can-bus.html);
  [coding-expert, BMW motorcycle diagnosis](https://www.coding-expert.de/diagnose-an-bmw-motorraedern-r-k-s-f-g-c-reihe/)).
  Which pins carry CAN on the R18, and how a given adapter maps them, is **not**
  established for this bike.
- **The diagnostic application protocol is UDS/KWP-family, reached via a
  gateway.** BMW's EDIABAS/ISTA stack speaks UDS (ISO 14229) and the older
  KWP-2000 over several "concepts" including D-CAN and BMW-FAST; the open
  re-implementation documents exactly this
  ([uholeschak/ediabaslib](https://github.com/uholeschak/ediabaslib) — "D-CAN,
  BMW-FAST, KWP-2000 … DS2 …"). On BMW CAN networks an ISO-TP request is routed
  by a gateway to the addressed ECU
  ([dissec.to vehicle scan](https://dissec.to/kb/chapters/HydraScope/vehicle_scan.html)).
  This is **car/diagnostic-stack** evidence; it tells us the *family* of
  protocol to expect, not the R18's specific IDs.
- **Regulatory context.** The R18 is a Euro-5 L-category vehicle, and the Euro-5
  step mandates OBD-stage-II with emissions-related communication per ISO 14229 /
  ISO 15765 for the regulated subset
  ([Reg (EU) 2019/129 amending 168/2013](https://eur-lex.europa.eu/eli/reg/2019/129/oj);
  [ISO 15765-4:2016](https://www.iso.org/fr/standard/67245.html?browse=tc)).
  This makes UDS-over-ISO-TP plausible for *emissions* data, but the service
  indicator and most Motorrad functions are manufacturer-specific and are **not**
  guaranteed to be reachable with generic ISO 15765-4 addresses.

**Conclusion for section 1:** expect UDS (ISO 14229-1) carried over ISO-TP
(ISO 15765-2) on a CAN bus reached through a gateway, most likely in BMW's
extended ("BMW-FAST", tester 0x6F1-style) addressing. Treat this as the protocol
*family to design for*, not as confirmed R18 addressing.

## 2. Addressing, request/response and ISO-TP — **not determined**

We did **not** find documented, R18-specific values for any of:

- physical request CAN ID and response CAN ID (per ECU, e.g. the instrument
  cluster/KOMBI that owns the service indicator);
- ISO-TP addressing mode (normal vs extended) and, if extended, the address
  extension bytes;
- padding convention and the diagnostic bitrate at the connector.

The illustrative "0x6F1 tester / 0x600+addr response, extended addressing" BMW
pattern is **not** asserted for the R18. Per the task ("do not guess"), the
addressing profile ships as a template with every id `null` and `verified:false`,
and the host CLI refuses to run until a human fills in documented/measured values
and marks it verified (`host/python/openmoto_diag/profiles/r18-UNVERIFIED.template.json`,
enforced by `openmoto_diag/profile.py`).

## 3. What the `0x130` traffic tells us (independent re-analysis)

The captured frames were re-decoded for this report directly from the raw
Phase 3B binary (`tests/phase3b/vehicle-r18-20261024T060152Z/passive.raw`) with
`host/python/phase3b_protocol.py`, not taken from the earlier summary:

- **105,051** decoded CAN frames, **100 %** identical: ID **`0x130`**, DLC 8,
  payload **`F7 FF FF FF FF FF 1F 0C`**. No other ID, no extended/RTR frames.
- Steady rate **≈ 3687 frames/s** (per-interval median across the capture),
  matching the reported ~3685/s while ignition was on.
- Controller counters at end of capture: `merr=16`, `tec_peak=0`, `rec_peak=0`,
  `eflg_or=0x40` (RX overflow only, from our host not draining fast enough),
  `mode_faults=0`, `spi_errors=0`. CAN1 stayed LISTEN_ONLY throughout.

**Timing model.** For an 11-bit data frame with this exact ID+payload, computing
the CAN-15 CRC and applying bit-stuffing gives a 109-bit stuffed field
(SOF…CRC). Three scenarios, at 500 kbps:

| scenario | modelled bits/frame | modelled fps |
|---|---|---|
| normal, acknowledged, back-to-back | 122 | 4098 |
| transmitter error-active, no ACK (active error flag) | 128 | 3906 |
| transmitter **error-passive**, no ACK (passive error flag + suspend) | 136 | 3676 |

The measured period is **135.6 bits/frame** (`500000 / 3687`). That lands on the
**error-passive, unacknowledged** case (136 bits), not on the acknowledged case
(122 bits). `merr=16` is consistent with a receive-side view of a node whose
transmit-error counter has crossed the 127 error-passive threshold and is
retransmitting the same frame without ever being acknowledged.

**Interpretation (evidence-based, not certainty):** `0x130` is almost certainly
**not** diagnostic traffic. The timing is the signature of a **lone transmitter
on an isolated bus segment getting no acknowledgment** — i.e. our interface was
most likely tapped onto a point where either only one ECU is present, or the
segment is isolated from the node that would ACK. The three candidate
explanations the task asks us to distinguish between resolve as:

- *Diagnostic gateway present and talking?* — No. A gateway/diagnostic exchange
  would not look like one 8-byte ID hammered at the error-passive no-ACK rate.
- *Isolated bus segment / missing ACK?* — **Yes, best fit.** The 135.6-bit period
  is the no-ACK, error-passive signature.
- *Just "frames received"?* — Technically true but misleading; the summary's
  "frames received" label hides that these are one node's unacknowledged repeats.

This is the single most important input to the GO/NO-GO: the one place we have
actually probed did **not** behave like a healthy multi-node diagnostic bus. We
do not yet know the correct place to connect for diagnostics.

## 4. Why 500 kbps alone proves nothing

Per the task's explicit warning: observing 500 kbps and seeing CAN frames does
**not** imply a UDS request will be answered. The capture shows an unacknowledged
single-ID stream, which is compatible with (a) the diagnostic ECUs living behind
a gateway on a different segment, (b) a wrong tap point, or (c) a bus that needs
a specific wake/tester-present sequence before it answers. None of these is
resolved by the bitrate.

## 5. Design of the active-diagnostics environment

`p3b_sniffer` is **unchanged** (`check_p3b_sniffer_passive.py` still passes on the
identical HEX `4c684a9e…`). Phase 4 is a separate source tree and separate build
environments:

- `firmware/mega-can/src/diag/diag_core.h` — portable core (compiles for AVR and
  for the host). Allowlist policy, ISO-TP single/multi-frame reassembly with
  flow control, UDS response validation, the ARM/CONFIRM shell, bounded timeouts,
  rate limiting, failure lockout and a latched emergency stop. Idle state is
  LISTEN_ONLY (silent); the controller is made active only inside one confirmed
  transaction and silenced again at its end.
- `firmware/mega-can/src/diag/main.cpp` — Arduino/MCP2515 glue. The only NORMAL-
  mode request and the only transmit routine are compiled in **only** when
  `OPENMOTO_DIAG_ALLOW_TX=1`. One-shot transmission (no auto-retransmit).
- `platformio.ini`: `[env:diag_dryrun]` (`ALLOW_TX=0`, no transmit code at all)
  and `[env:diag_readonly]` (`ALLOW_TX=1`, build-only).

Host side: `host/python/openmoto_diag/` (policy mirror, strict profile loader,
serial + simulator transports, CLI) with the entry point
`host/python/diag_cli.py`. The offline mock ECU and host harness live in
`tests/diag_host/diag_sim.cpp`, which links the **real** firmware core against an
in-process UDS responder.

### Safety properties (enforced, and audited)

- **Read-only by construction.** Only `TesterPresent (3E00)`,
  `ReadDataByIdentifier` for 14 ISO-14229 identification DIDs, and
  `ReadDTCInformation` sub-functions `01`/`02` can be armed. Every mutating,
  security, routine, coding, memory or comms service (`10,11,14,27,28,2A,2C,2E,
  2F,31,34–38,3D,83–87`) is denied by name and by default-deny. Enforced on both
  firmware and host; `test_diag.py` checks 18 cases agree.
- **Per-request confirmation.** Each transmission needs `ARM <req>` then
  `CONFIRM <code>`, where the 4-hex code is derived from the exact CAN frame the
  firmware will send and printed by the firmware. Confirmations are single-use
  and expire after 30 s; 3 wrong codes trips emergency stop.
- **Bounded everything.** P2=300 ms, P2*=3 s, N_Cr=300 ms, 6 s total per
  transaction, ≥1 s between transactions, ≤25 transactions per power cycle, at
  most one request frame + one flow-control frame per transaction.
- **Fail-safe latches.** 3 consecutive failures → lockout until power cycle;
  `!` or `ESTOP` → latched emergency stop; if LISTEN_ONLY can't be re-verified at
  boot or after a transaction, the client latches itself off.
- **No addressing defaults.** `ADDR` and `PAD` must be set explicitly; the
  functional broadcast `0x7DF` and `tx==rx` are rejected.
- **Build-level proof.** `check_diag_firmware.py` shows the `diag_dryrun` image
  links no NORMAL-mode/transmit code at all (so ARM/CONFIRM is riskless there),
  and that in `diag_readonly` the only path to NORMAL mode is
  `diagHalEnterNormal()`.

## 6. Offline tests (all pass; none are hardware results)

Run on the pinned toolchain host:

```sh
python3 tests/test_diag.py            # host core + mock ECU + CLI (clang++)
python3 tests/check_diag_firmware.py  # static audit of both diag ELFs
```

`test_diag.py`: 18 policy-agreement cases; 14 request/response flows (positive
single/multi-frame, negative, pending→positive, NO_RESPONSE, CF_TIMEOUT,
BAD_SEQUENCE, INVALID_RESPONSE, TOO_MANY_PENDING, MODE_FAIL); safety machinery
(confirm gating, policy deny, not-configured, `!`/ESTOP, lockout, boot latch);
DRYRUN no-transmit; profile loader refusals; and a full CLI session plus its
`--port`/unverified-profile guards. `check_diag_firmware.py`: PASS on both
environments (HEX hashes printed). The pre-existing suites remain green
(`check_p3b_sniffer_passive.py`, `test_phase3b_protocol.py` 28/28,
`test_phase2_firmware.py`).

## 7. Exact prerequisites for the first real read-only request

All of P0–P4 must be satisfied, in addition to the still-open electrical
blockers B1–B4 and recommendations in
[docs/vehicle-readiness-r18.md](vehicle-readiness-r18.md). None is cleared yet.

- **P0 — electrical/passive readiness.** Clear B1–B4 from the passive readiness
  audit (connector/adapter pinout, bus identity/termination, single ground path,
  floating-SPI-during-reset). Active use is strictly more demanding than passive.
- **P1 — correct diagnostic tap point.** Identify where on the R18 the diagnostic
  CAN actually is (section 3 shows the probed point is not a healthy diagnostic
  bus). A passive capture at the true diagnostic connector must show a **healthy,
  acknowledged, multi-ID** bus (acknowledged frame timing, `merr=0`), not the
  `0x130` no-ACK stream.
- **P2 — verified addressing profile.** Obtain, from a documented source or
  direct measurement, the request/response IDs, ISO-TP addressing mode, padding
  and bitrate for at least the target ECU that owns the data of interest. Fill in
  `profiles/r18-*.json`, cite each value in `evidence`, set `verified:true`. The
  CLI will keep refusing until this is done.
- **P3 — bench dry-run first.** With `diag_dryrun` on the bench (no transmit
  code), rehearse the full ARM/CONFIRM flow and confirm `RESULT status=DRYRUN`,
  `frames_tx=0`. Then build `diag_readonly` and dry-run it against the simulator.
- **P4 — single, supervised first request.** Stationary bike, ignition on, engine
  off, never cranking or riding while connected. Send exactly one
  `TesterPresent` (or one identification DID read), with interactive
  per-request confirmation, then stop and inspect the trace before anything else.
  The `--port` path additionally requires `--i-have-read-prereqs` and typing
  `TRANSMIT`.

## 8. GO / NO-GO

**NO-GO** for transmitting to the motorcycle. The implementation is ready and
proven offline, but P1 (correct, healthy diagnostic tap) and P2 (verified
addressing) are unmet, and the one bus behaviour we have measured is the no-ACK
signature of section 3. Proceeding now would mean guessing addresses and
transmitting into a segment that does not acknowledge — exactly what the task
forbids. Re-evaluate once P0–P4 are satisfied; the first real step is then a
single confirmed `TesterPresent`.
