# KOMBI service-reset findings for the 2021 BMW R18 (MotoScan static analysis)

Date: 2026-10-24. Scope: how MotoScan communicates with the instrument cluster
(KOMBI) to read and reset the service reminder, recovered by **static analysis of
the MotoScan APK only**. No motorcycle was connected, MotoScan was not launched,
and no diagnostic request or write was sent to any vehicle. The R18 is
disconnected. This document records only what the bytecode shows, with the
evidence level of each claim.

Evidence levels: **[APK]** confirmed in decompiled MotoScan bytecode;
**[PUBLIC]** documented public behaviour; **[INFERRED]** hypothesis/placeholder;
**[UNKNOWN]** unresolved. Static code is not an on-bike capture.

## Source artifact

- MotoScan `base.apk`, package `de.wgsoft.motoscan`, version 1.110 (code 1110),
  SHA-256 `d1dbc9e2a57d7d96cfd1e77e251850f67e5fc0c7f430ef033f04ac5f7da07ce6`.
- Disassembled with Android dexlib2/baksmali 3.0.9 (all 7,231 classes); smali
  only, not Java/native decompilation. Analysis tree on the dev Mac:
  `~/Downloads/bmw-apk-comparison/analysis/` (prior work; its reports are
  cross-checked here against the raw smali, not taken on trust).
- Tools: JADX/apktool were unavailable (no network, DNS failures); baksmali
  library disassembly was used instead. ISTA/EDIABAS and GS-911 binaries were
  **not** present locally, so their trace/logging facilities are documented from
  public knowledge ([PUBLIC]) only — see "Other tools" below.

Each byte-level claim below was re-verified by me directly in the smali at the
cited file and line, not merely copied from the prior report.

## Transport: ELM327/STN, not raw custom frames

**[APK]** MotoScan drives standard ELM327/STN adapters over Classic Bluetooth or
TCP (`vq4` logs as `VciELM327`; class `sq4` does the socket I/O). The BMW-UDS
branch in `vq4.x(Lox2;I)Z` (`vq4.smali:4096`) configures the adapter with this
exact ordered sequence (`vq4.smali:4401`–`4490`):

```
ATSPB            user protocol B
ATPBC101         PB options C1: 11-bit, ISO 15765, 500 kbit/s   [PUBLIC: ELM datasheet]
ATSH6F1          tester (source) header 0x6F1
ATFCSH6F1        flow-control header 0x6F1
ATFCSD <tgt> 300008   FC data = target byte + 30 00 08         (literal "300008" at vq4.smali:4485)
ATFCSM1          flow-control mode 1
ATCEA <tgt>      CAN extended address = target byte
ATCM7FF          acceptance mask 7FF
ATCF 6 <tgt>     acceptance filter 6 + target byte
ATBI             bypass init
```

So the transport is **ELM/STN AT+ST commands with BMW extended-address ISO-TP**,
with the Android side doing BMW frame reconstruction (`yl4.v`: 5 payload bytes
from the first frame, 6 from each subsequent frame). It is **not** a bespoke raw
CAN protocol and **not** generic ISO 15765-4 OBD addressing. STN-specific paths
(`STCSEGT1`, `STCFCPC`, `STCFCPA 6F1`) also exist. This matches MotoScan's
supported-adapter list (OBDLink LX/MX/MX+, UniCarScan). [APK]

The BMW FC data `...300008` differs from the Biketune example `...30FF08`; do not
interchange them. [APK]

## Addressing: KOMBI = ECU address byte 0x60, tester 0x6F1

**[APK]** The cluster factory `cr.c(I)` loads the constant `0x60` immediately
before its cluster-type switch (`cr.smali:2200`) and builds the modern cluster
object (`i15`, shared base `sl0`/`el0`). With tester 0x6F1 and BMW extended
addressing, the physical response CAN id is **0x660** and frames carry an
address-extension byte (tester 0xF1 inbound, target 0x60 outbound).

**[APK]** `cr.smali` enumerates cluster variants `cuX_KOMB03/17/18/46/50/53/HI/
LO/B2/...` ("Instrumentenkombi K50" etc.). The modern liquid-cooled cluster
family (K50) is present.
**[UNKNOWN]** which exact variant the 2021 R18 selects at runtime — that decision
runs through native MotoScan data that was not recovered. 0x60 is the cluster
*address*; the *variant* only changes which DIDs/format table is applied.

## ECU probe and service READS (request bytes confirmed)

| Operation | Request | Evidence |
|---|---|---|
| Probe KOMBI | `22 F1 50` (expect prefix `62 F1 50`) | [APK] `mc0.t1`; `sl0.o0` |
| Read odometer | `22 E1 19` | [APK] `sl0.r()` `sl0.smali:7700`, `filled-new-array {0x22,0xE1,0x19}` |
| Read clock | `22 E1 2B` | [APK] same method |
| Read next service date | `22 E1 2C` | [APK] `sl0.smali` literal `:array_170` = `22 E1 2C` |
| Read service distance | `22 E1 2D` | [APK] same method |

**[APK]** `sl0.r()` parses E119 as a 7-byte response (3-byte positive header +
4-byte **big-endian** odometer at offset 3), E12B as 10 bytes, E12D as 5 bytes
(2-byte big-endian short at offset 3), E12C as 7 bytes (day / month / full binary
year). These are the response **shapes** the app expects; the actual data bytes
are not in the APK and need a real capture.

## Service RESET writes (request prefixes + lengths confirmed; payloads not)

**[APK]** The reset is three UDS WriteDataByIdentifier calls built in
`sl0.C(dl0,int,Date,Date)` (`sl0.smali:435`). Verified from the `new-array`
sizes and `aput-byte` of `0x2E`, `0xE1` and the DID low byte:

| Write | Bytes | Array size | Evidence |
|---|---|---|---|
| Clock | `2E E1 2B` + 7 data | 10 | [APK] `sl0.smali:631` (`v18=2E,v17=E1,v25=2B` @640, `new-array 0xA`) |
| Next service date | `2E E1 2C` + 4 data | 7 | [APK] `sl0.smali:812` (`0x2C`, size `v22=7`) |
| Service distance | `2E E1 2D` + 2 data | 5 | [APK] `sl0.smali:925` (`0x2D`, size `v6=5`) |

**[APK]** Order is clock → date → distance; `sl0.C` passes a 2000 ms adapter
timeout and sleeps 1500 ms between operations (app choices, not verified ECU
timing).

**[INFERRED]/[UNKNOWN]** The data-byte **encodings** (how a date/distance maps to
those 4/2 bytes), and **[UNKNOWN]** whether a diagnostic session (`10 xx`) or
security access (`27 xx`) must precede the writes, the valid ranges, NVM
persistence and read-back. None of this is established. A separate routine
`31 01 F6 51` and DTC/identification reads (`19 02 0C`, `22 20 00`, `22 10 0B`,
clear-all `14 FF FF FF`, KWP templates `3B 10/11/12`) also exist in `mc0`/`sl0`
but are **not** the service-reminder reset.

## Other tools (ISTA, EDIABAS, GS-911)

Not analysed from binaries — none were present locally.
- **[PUBLIC]** BMW's EDIABAS/ISTA stack speaks UDS and KWP-2000 over "concepts"
  incl. D-CAN and BMW-FAST, and produces API/IFH trace logs (`.trc`,
  `api.log`/`ifh.log`) — a documented source of real request/response bytes if
  one has authorised access to an ISTA session. The open re-implementation
  `ediabaslib` documents this concept set and a simulation-file trace format.
- **[PUBLIC]** MotoScan's own docs name KOMBI as the service-reset endpoint and
  describe "future date / remaining distance / clock correction", matching the
  E12B/E12C/E12D fields above; they do not publish the raw DIDs.
- **[PUBLIC]** GS-911 performs the same reset on BMW bikes; no binary was
  inspected here.

Capturing an authorised ISTA/GS-911/MotoScan session (adapter in the middle, or
the app's own log) is the straightforward way to turn the [INFERRED] payloads and
[UNKNOWN] session requirements into verified bytes. The replay tool below imports
such captures.

## Offline replay tooling (this repo)

- `host/python/openmoto_diag/trace.py` — trace model + importers for ELM327/STN
  logs (`parse_elm_log`), CAN/ISO-TP frame logs with extended addressing and
  multi-frame reassembly (`parse_can_log`), and JSON (`load_json_trace`).
- `host/python/openmoto_diag/replay.py` — offline replay against a mock ECU built
  from the trace's own responses; preserves order and session, validates
  responses, records timing, and **refuses writes** (BLOCKED) unless an explicit
  `--allow-write-dryrun` labels them as dry runs that are still never sent.
- `host/python/diag_replay.py` — CLI entry point.
- Verified traces: `openmoto_diag/traces/kombi-read-r18.json` (read-only, all
  request bytes [APK]) and `openmoto_diag/traces/kombi-service-reset-r18.json`
  (the recovered write sequence, [APK] prefixes, always BLOCKED by replay).
- Tests: `tests/test_diag_replay.py` (importers, reassembly, replay, write-guard,
  host-policy agreement) and sample fixtures under `tests/diag_traces/`.

The existing passive sniffer (`p3b_sniffer`) and the read-only diagnostic
firmware (`diag_dryrun`/`diag_readonly`) are unchanged.

## Remaining steps before the first controlled diagnostic request

1. **Confirm the KOMBI tap + bus health.** The only R18 bus we have probed shows
   the unacknowledged `0x130` stream (see `docs/diag-active.md` §3); that is not
   the diagnostic bus. Locate the diagnostic connector and confirm a healthy,
   acknowledged bus at the KOMBI, at the adapter's 500 kbit/s. Avoid further work
   on the `0x130` traffic — it does not advance diagnostic connectivity.
2. **Capture an authorised reference session** (MotoScan/ISTA/GS-911 via an
   ELM/STN adapter or an in-line CAN tap) on the actual 2021 R18. Import it with
   `diag_replay.py --elm/--can` to obtain the **verified** probe/read bytes and
   the exact E12B/E12C/E12D field encodings, plus any required session/security
   step. This replaces every [INFERRED] payload and resolves the [UNKNOWN] items.
3. **First real request = a single READ.** Using the verified addressing
   (0x6F1 → 0x60, protocol-B extended, FC `...300008`), issue exactly one
   `22 F1 50` probe, then `22 E1 2C`/`22 E1 2D` reads, through the existing
   read-only client with per-request confirmation. Compare the live response
   shapes to `sl0.r()`'s expected lengths.
4. **Only then design the reset write path.** Service writes (`2E ...`) stay
   default-denied in firmware and host policy throughout this phase; enabling
   them is a separate, later decision requiring the verified session/range/
   read-back contract. Do not transmit a guessed write.

## GO / NO-GO for a KOMBI service reset

**NO-GO to write.** The addressing and transport are now evidence-backed and the
read request bytes are confirmed, which is real progress toward connectivity, but
the write payload encodings and the session/security prerequisites are unverified
([INFERRED]/[UNKNOWN]), and no healthy diagnostic bus has been confirmed on the
bike. The next action is a captured reference session + a single read-only probe,
not a reset.
