# Phase 5: first real BMW R18 KOMBI read-only probe (22 F1 50)

Date: 2026-10-24. Scope: prepare — offline only — a single, allowlisted,
read-only KOMBI identification probe (`22 F1 50`) for a 2021 BMW R18, using the
Mega 2560 + MCP2515/TJA1050. **Nothing was transmitted to the motorcycle. The
bike is disconnected.** Everything below is from the firmware source, the
MotoScan static findings (docs/diag-kombi-findings.md), the prior passive
capture, public documentation, and the offline simulator.

Evidence levels: **[APK]** MotoScan bytecode; **[PUBLIC]** documented;
**[FW]** proven from our firmware/tests; **[INFERRED]**/**[UNKNOWN]** unverified.

## Verdict: **NO-GO to transmit on the motorcycle** (GO to build/rehearse offline)

The probe is implemented, encodes byte-for-byte to the MotoScan protocol-B
format, passes every offline test, and fails safe on a no-ACK bus. But two
preconditions for a *meaningful and safe* real request are still unmet:

- **No confirmed, healthy diagnostic tap.** The only R18 bus we have ever probed
  showed the unacknowledged `0x130` stream (error-passive, no ACK — see
  docs/diag-active.md §3). A diagnostic request needs an ECU to ACK our frame;
  that segment does not ACK. We have not located/confirmed the KOMBI diagnostic
  connector or shown a healthy acknowledged bus at 500 kbit/s.
- **Addressing is APK-derived, not bike-verified.** The task is explicit: APK
  evidence alone does not prove the specific 2021 R18 ECU variant will answer.
  The R18 KOMBI profile is therefore `verified=false` and the tooling refuses to
  transmit with it.

The firmware (`diag_kombi_probe`), host CLI (`--kombi-probe`) and tests are ready;
the remaining steps (§7) are a connector/bus confirmation and flipping a profile
to verified after that confirmation.

## 1. Firmware vs MotoScan protocol-B: byte-for-byte [FW][APK]

The Phase 4 ISO-TP engine (`firmware/mega-can/src/diag/diag_core.h`) was compared
against the recovered MotoScan protocol-B init (docs/diag-kombi-findings.md). The
relevant equivalences:

| MotoScan ELM/STN setup [APK] | Our firmware mechanism [FW] | Match |
|---|---|---|
| `ATSPB` + `ATPBC101` → CAN, 11-bit, ISO 15765, 500 kbit/s | MCP2515 NORMAL, 500 kbit/s, 11-bit std frames | yes |
| `ATSH6F1` tester header | `ADDR` tx id = 0x6F1 (`txId`) | yes |
| `ATCEA60` extended address (target byte prepended as data[0]) | `ADDR` tx ext = 0x60 → `buildFrame` writes `data[0]=txExt` | yes |
| `ATCF660` / `ATCM7FF` accept only responses from 0x660 | `ADDR` rx id = 0x660 → `canSetRxId(0x660)` + `onFrame` id filter | yes |
| response ext byte 0xF1 (tester) stripped | `ADDR` rx ext = 0xF1 → `onFrame` requires `data[0]==rxExt`, strips it | yes |
| `ATFCSD 60 30 00 08` / `ATFCSM1` flow control BS=0 STmin=8 | `sendFlowControl()` sends `30 00 FC_STMIN` (BS=0) | equivalent* |
| request padded to DLC 8 with 0x00 | `PAD 00` → `buildFrame` pads to 8 | yes |

\* STmin differs in value only (ours = 2 ms vs MotoScan's `08`). STmin is the gap
*we* ask the ECU to honour between consecutive frames; for a single-frame probe
response no flow control is used at all, so this never comes into play for
`22 F1 50`. For multi-frame reads we can set STmin to match later; it does not
affect this probe.

Conclusion: our ISO-TP/addressing implementation reproduces the MotoScan
protocol-B framing for this probe. The one difference (STmin) is inert for a
single-frame exchange.

## 2. Exact proposed CAN frame bytes [FW]

With `ADDR 6F1 660 60 F1` and `PAD 00`, the firmware emits (verified in the
simulator ARMED/TRACE output and in `tests/test_diag_kombi_probe.py`):

```
Request:  CAN id 0x6F1, DLC 8, data = 60 03 22 F1 50 00 00 00
                                       │  │  └──────┘ └──────┘
                                       │  │   UDS       0x00 padding
                                       │  └─ ISO-TP single-frame PCI: length 3
                                       └──── BMW extended address = target 0x60
```

- `60` — target ECU address byte (KOMBI), from `ATCEA60` / `tx_ext`.
- `03` — ISO-TP SingleFrame PCI, payload length 3.
- `22 F1 50` — UDS ReadDataByIdentifier, DID 0xF150 (BMW KOMBI identification).
- `00 00 00` — padding to DLC 8 (BMW/ELM convention).

This is a single frame; the firmware transmits **at most one request frame** and,
for this probe, **no flow-control frame** (the response is expected to be a single
frame). One-shot transmit: if nothing ACKs it, it is **not** retransmitted.

## 3. Expected response format [APK][FW]

```
Response: CAN id 0x660, DLC 8, data = F1 LL 62 F1 50 <d0> <d1> <d2> ...
                                       │  │  └──────────┘
                                       │  │   UDS positive response
                                       │  └─ ISO-TP PCI (single frame: 0x06+, or
                                       │     first frame 0x1L.. for a longer reply)
                                       └──── BMW extended address = tester 0xF1
```

- Response arrives on **0x660** (= `0x600 | 0x60`), first byte `0xF1` (tester). [APK]
- Firmware requires `data[0]==0xF1`, strips it, parses ISO-TP, and validates the
  UDS positive response: `rsp[0]==0x62` (`0x22+0x40`), `rsp[1..2]==F1 50`. [FW]
- `validateResponse` for service 0x22 accepts `n>=4 && rl==3 && echo matches`, so
  any identification payload shape is accepted; the actual identification bytes
  are **[UNKNOWN]** until captured. A single-frame reply up to 4 payload bytes
  fits one frame; a longer reply becomes a first frame + our one flow-control
  frame + consecutive frames, all handled by the engine (tested). 
- A negative reply `7F 22 <nrc>` is reported as `NEGATIVE` with the NRC; e.g. NRC
  0x31 (requestOutOfRange) would say the DID is not supported on this variant.

## 4. Does the 0x130 traffic prevent reliable communication? [FW][INFERRED]

On the tap point we have seen, **yes, it would prevent a successful exchange**,
and this is the decisive point:

- The `0x130` capture is one node transmitting unacknowledged at the
  error-passive rate (135.6 bit/frame; docs/diag-active.md §3). That means the
  node we were next to had **no one to ACK it** — either a single-node/isolated
  segment or a wrong tap.
- A UDS request only works if the addressed ECU is on the same segment and ACKs
  our frame. If we transmit into a segment that behaves like the `0x130` one, our
  one-shot probe gets **no ACK → `TX_FAILED`**, and the engine stops after exactly
  one attempt and returns to LISTEN_ONLY. It does **not** go error-passive/bus-off
  the way the `0x130` node has (one-shot mode is the key: no retransmission). This
  is proven offline: `tests/test_diag_kombi_probe.py::test_noack_clean_fail`
  (`RESULT status=TX_FAILED frames_tx=1 silent=1`).
- So the `0x130` traffic is not a hazard to us (we fail safe), but it is a strong
  signal that **the current connector is the wrong place** for diagnostics. The
  probe must be attempted only after a passive capture at the real diagnostic
  connector shows a healthy, acknowledged, multi-ID bus.

## 5. Can the current MCP2515/TJA1050 hardware do this exchange? [PUBLIC][FW][UNKNOWN]

Capability: **yes in principle** — the MCP2515 does standard 11-bit CAN at
500 kbit/s in NORMAL mode and the TJA1050 is a standard high-speed transceiver;
transmitting `0x6F1` and receiving `0x660` is ordinary CAN. One-shot mode makes a
failed arbitration/ACK safe. [PUBLIC][FW]

Preconditions still open before it is *safe and reliable* on the bike
(from docs/vehicle-readiness-r18.md, still unmet):

- **Termination/ACK.** For the ECU to ACK, the bus needs proper termination and
  the ECU must be present and powered. Our interface's termination and the R18
  diagnostic bus termination are unverified (B2). An unacknowledged transmit is
  exactly the `0x130` failure we must avoid relying on.
- **Connector/pinout (B1), single ground path (B3), floating-SPI-at-reset (B4)**
  are unverified. Active transmit is strictly more demanding than the passive
  capture these blockers were written for.
- **Bitrate.** 500 kbit/s is an [APK]/bench assumption; it must be confirmed at
  the actual diagnostic connector.
- **No alternative adapter is required** by capability — but note MotoScan itself
  uses an ELM/STN adapter that handles framing in firmware. Our Mega does the
  framing itself (proven equivalent in §1), so the Mega is a valid alternative to
  an ELM adapter for this probe, *if* the physical preconditions are met.

So: hardware capability is adequate; physical-layer readiness at the R18
diagnostic connector is **unverified**, which is a NO-GO input.

## 6. Offline test results [FW]

Build (remote PlatformIO toolchain host) and audit:

```
diag_dryrun              OK   (HEX unchanged from Phase 4: 2167ed9e…)
diag_readonly            OK   (HEX unchanged from Phase 4: ff73ca37…)
diag_kombi_probe_dryrun  OK   (new: no transmit code)
diag_kombi_probe         OK   (new: transmit path, build-only)
tests/check_diag_firmware.py  PASS  (all four images; dryrun/probe-dryrun cannot
                                     transmit; readonly/probe reach NORMAL only
                                     via diagHalEnterNormal)
```

The Phase 4 `diag_dryrun`/`diag_readonly` HEX hashes are **identical** to before,
proving the Phase 5 change (a compile-time-gated extra DID) did not alter the
Phase 4 firmware.

Behaviour (`tests/test_diag_kombi_probe.py`, real firmware core vs mock ECU):

- request frame encodes exactly `6F1 / 60 03 22 F1 50 00 00 00`, DLC 8;
- response on `0x660` with ext byte `0xF1` stripped → `resp=62 F1 50 …`, `OK`;
- no-ACK bus → `TX_FAILED`, exactly 1 frame, returns silent (no storm);
- `22 F1 50` is **denied** in the default build and allowed only with
  `OPENMOTO_DIAG_KOMBI_PROBE=1` / host `--kombi-probe`;
- writes/resets/session/security stay denied even in the probe build;
- only DID 0xF150 is added — neighbouring DIDs (F14F/F151/F100/F1A0) stay denied;
- CLI: probe succeeds with the flag + verified sim profile; refused without the
  flag; refused with the APK-derived (unverified) R18 profile.

All pre-existing suites still pass (`test_diag`, `test_diag_replay`,
`check_p3b_sniffer_passive` — `p3b_sniffer` HEX unchanged).

## 7. Exact remaining steps before the first real read-only request

1. **Locate & confirm the KOMBI diagnostic connector** on the 2021 R18 and clear
   electrical blockers B1–B4 (docs/vehicle-readiness-r18.md): connector/pinout,
   termination (≈60 Ω across the diagnostic CAN, ignition off), single USB ground
   path, CS pull-ups for floating-SPI-at-reset.
2. **Passive capture at that connector** (existing `p3b_sniffer`, LISTEN_ONLY).
   Require a healthy, **acknowledged**, multi-ID bus at 500 kbit/s (`merr=0`, not
   the `0x130` no-ACK signature). If `rx=0`/`merr>0`, the tap or bitrate is wrong —
   stop and fix before any transmit.
3. **Mark the addressing verified.** Only after step 2, copy
   `profiles/r18-kombi-APK.json` to a real profile and set `verified:true`, citing
   the passive-capture evidence in `evidence`. Until then the tooling refuses it.
4. **Bench dry-run.** Flash `diag_kombi_probe_dryrun` (no transmit code); rehearse
   `ADDR 6F1 660 60 F1` / `PAD 00` / `ARM 22F150` / `CONFIRM <code>` and confirm
   `RESULT status=DRYRUN frames_tx=0`.
5. **First real request = exactly one probe.** Stationary bike, ignition on,
   engine off, never cranking/riding while connected. Flash `diag_kombi_probe`,
   then with the verified profile run one
   `diag_cli.py --port <dev> --profile <verified> --target kombi --request
   kombi_probe --kombi-probe --i-have-read-prereqs`, type `TRANSMIT`, then the
   per-request confirmation code. Inspect the single `RESULT`/`TRACE` and stop.
6. **Interpret, do not escalate.** `OK` → compare the identification payload to
   MotoScan's expected shape. `NEGATIVE`/`TX_FAILED`/`NO_RESPONSE` → diagnose the
   tap/addressing; do not try other requests or any write. Service writes stay
   default-denied; the service-reset path remains a separate, later decision.

## 8. GO / NO-GO

**NO-GO to transmit on the motorcycle now.** GO to build, audit and rehearse the
probe offline — which is complete and green. The blocking items are physical:
a confirmed healthy diagnostic tap (step 1–2) and a bike-verified addressing
profile (step 3). The probe is designed to fail safe if those are wrong (one-shot
`TX_FAILED`, latched silence), but "fails safe" is not "works"; attempting it on
the `0x130`-style segment or with APK-only addresses would be guessing, which the
task forbids. Re-evaluate after steps 1–4; the first real action is then a single
confirmed `22 F1 50`.
