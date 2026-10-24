# Phase 9: service-reminder WRITE — implementation & pre-write report

Date: 2026-10-24. Scope: implement the KOMBI service-write capability (ISO-TP
multi-frame TX + read-back verify), validate it OFFLINE, and report the exact
target values/bytes. **No write has been transmitted to the motorcycle.** This
stops at the final-write gate for owner authorization.

## 1. What was built

- **Separate write core** `firmware/mega-can/src/diag_write/write_core.h` + glue
  `main.cpp`, envs `diag_write_dryrun` / `diag_write`. Kept entirely apart from
  the read `diag_core.h` so the read-only builds stay **byte-identical** (verified:
  `diag_readonly ff73ca37`, `diag_kombi_probe a30dded2`, `diag_service_read
  c8aee634`, `diag_ack 5244f058`, `p3b_sniffer 4c684a9e` all unchanged).
- **ISO-TP multi-frame transmit** (the missing capability): single frame for
  E12D (5B); first-frame -> wait ECU flow-control (CTS/WAIT/OVFLW) -> consecutive
  frames honouring block size & STmin for E12C (7B) and E12B (10B). Multi-frame
  read-back responses are reassembled too.
- **Write policy:** only `2E E1 2B/2C/2D` with exact length are armable; any other
  `0x2E`, or `0x2F`/`0x27`/etc., is refused ("not-an-allowed-service-write").
- **Authorization:** ADDR+PAD required; per-write `ARM <bytes>` then `CONFIRM
  <code>` (code bound to the exact frame + addressing + counter + time, single
  use, 30 s TTL); 3 bad confirms -> estop; rate-limited; max 6 writes/power-cycle;
  `!`/ESTOP latched; auto-return to LISTEN_ONLY.
- **Negative-response handling:** `7F`/NRC surfaced as NEGATIVE (e.g. 0x33
  securityAccessDenied); responsePending 0x78 tolerated up to a bound.
- **Mandatory read-back verify:** after a `6E` positive, the firmware issues the
  matching `22 E1 xx`, reassembles the response, and byte-compares the stored data
  to what was written. `verified=YES` only on exact match; otherwise
  `READBACK_MISMATCH` + an ALERT to use the rollback reference.

## 2. Offline validation (task 7) — PASS

`tests/test_diag_write.py` (real write core vs write-capable mock KOMBI,
`tests/diag_write_host/write_sim.cpp`):
- E12D single-frame + E12C/E12B multi-frame writes: `status=OK verified=YES`,
  read-back equals written bytes.
- NRC 0x33 -> NEGATIVE; responsePending -> eventual OK; **READBACK_MISMATCH caught
  with rollback alert**; missing flow-control -> FC_TIMEOUT; silent -> NO_RESPONSE.
- Only the three service writes armable; `!`/estop latches and blocks ARM.
- Dry-run build -> `DRYRUN`, nothing transmitted.
No physical test bench is used; the mock exercises the exact firmware logic.

## 3. E12D semantics (task 4) — determined

See docs/diag-e12d-analysis.md. E12D is a distance value written **directly,
BE16, unscaled**, treated by the cluster as **distance remaining to next service**
(NOT an absolute odometer threshold — 5180 < odo 6444 would be already-overdue,
contradicting the future service date). Unit is taken as **km** (live values
self-consistent as km; no conversion in the APK). The km interpretation is
[ASSUMPTION], confirmed by the post-write read-back + dash check.

## 4. EXACT target values and payload bytes

Owner inputs: next service date **2027-09-09**; service interval **6,000 km**.
Owner inputs: next service date **2027-10-24**; service interval **10,000 km**.
Clock (E12B) **not** requested -> will NOT be written (left as-is). Odometer
(E119) is read-only and **never modified** (stays 6,444 km).

| DID | meaning | current (rollback) | NEW target payload | decoded target |
|---|---|---|---|---|
| E12C | next service date | `2E E1 2C 09 09 07 EA` (2026-09-09) | **`2E E1 2C 18 0A 07 EB`** | 24.10.**2027** |
| E12D | service distance | `2E E1 2D 14 3C` (5180) | **`2E E1 2D 27 10`** | **10000** (km, =0x2710) |
| E12B | cluster clock | `2E E1 2B 00 33 0A 18 0A 07 EA` | (not written) | unchanged |
| E119 | odometer | `62 E1 19 00 00 19 2C` (6444) | (read-only) | never modified |

On the wire (extended addressing, PAD 00), E12C is a multi-frame TX: first frame
`60 10 07 2E E1 2C 18 0A`, then after the ECU's flow control, consecutive
`60 21 07 EB`. E12D is a single frame `60 06 2E E1 2D 27 10 00`.

## 5. Preserved originals & recovery (task 6)

`tests/diag_service/rollback-reference.json` holds the original raw reads and the
`2E` payloads that restore them exactly:
- restore E12C -> `2E E1 2C 09 09 07 EA`
- restore E12D -> `2E E1 2D 14 3C`
Recovery for an interrupted/partial write: each write is independent and
read-back-verified, so a failure leaves a known state (the RESULT line says
whether the cluster accepted it). If `READBACK_MISMATCH` or any non-OK status
occurs, re-read with `diag_service_read` to see the actual stored value, then
write the rollback payload for that DID. Because writes are one-at-a-time and
verified, there is no multi-DID half-committed state beyond the single DID in
flight.

## 6. Remaining risks (task 10)

- **Unit (km vs mile) = [ASSUMPTION].** If the cluster unit were miles, `0x2710`
  would mean 10000 mi (~16093 km), not 10000 km. Mitigation: write E12D, read back,
  and confirm the dash shows a sane remaining distance before trusting it; the
  rollback payload restores 5180 if wrong.
- **Session/security unknown.** No `0x10`/`0x27` seen in the Java smali, but the
  native lib was not examined. The ECU may reject the `2E` with an NRC (e.g.
  0x33/0x7F) — the firmware reports this as NEGATIVE and writes nothing further.
  This is actually the safe first-contact outcome if it happens.
- **E12B not written** (out of scope). If a clock set is later wanted, it is a
  separate authorized write.
- Electrical B1-B4 remain unverified (prior phases; owner-accepted).

## 7. Deployment readiness (task 8) & GO/NO-GO

Firmware `diag_write` builds, is offline-verified, and is ready to flash
(OBD-disconnected, per B4). Proposed supervised sequence, each its own
ARM/CONFIRM, read-back-verified, with the owner approving every write:

```
# OBD disconnected: flash
./scripts/pio run -d firmware/mega-can -e diag_write -t upload --upload-port /dev/cu.usbmodem21101
# Reconnect OBD, ignition ON / engine OFF:
ADDR 6F1 660 60 F1
PAD 00
STATUS
# 1) distance first (single frame, lowest risk, resolves the unit question):
ARM 2EE12D2710   ->  CONFIRM <code>    # expect RESULT status=OK verified=YES, readback 62E12D2710
#    -> read dash: does remaining distance look like 10000 km? if not, write rollback 2EE12D143C.
# 2) then the date (multi-frame):
ARM 2EE12C180A07EB -> CONFIRM <code>   # expect status=OK verified=YES, readback 62E12C180A07EB
```

**GO to deploy (flash) the write firmware; NO-GO to transmit a write without the
owner's explicit per-write authorization.** The implementation is complete and
offline-verified; the actual ECU modification is gated on owner approval at the
CONFIRM step. Per Phase 9 task 10, STOPPING here before the first real write.
