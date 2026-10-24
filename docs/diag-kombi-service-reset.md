# Phase 8: BMW R18 KOMBI service-reset encoding (reverse-engineered)

Date: 2026-10-24. Scope: recover, OFFLINE, exactly how MotoScan resets the R18
service reminder through KOMBI, and build an offline encoder/decoder. **No write
is enabled and nothing was transmitted to the motorcycle.** This is the evidence
base for a later, separately-authorized write decision.

Evidence levels: **[APK]** confirmed in MotoScan 1.110 smali; **[BIKE]** matches
our real read-backs; **[ASSUMPTION]** inferred / not proven.

## 1. Source of truth

`sl0.C(Ldl0;ILjava/util/Date;Ljava/util/Date;)` — `motoscan-smali/sl0.smali:435-1008`.
Signature: `C(dl0 which, int distance, Date serviceDate /*p3*/, Date clock /*p4*/)`.
Caller computing the arguments: `w1.smali:~2397` (`r(Object)`), plus `jr.C`/`ry3`.
All byte offsets below were read directly from the `aput-byte` order in the smali.

## 2. Recovered payloads (byte-for-byte) [APK]

One `sl0.C` call can emit up to three WriteDataByIdentifier frames, selected by
the `dl0` enum `which`; the special enum `Ldl0;->c` emits all three. Between
writes it `Thread.sleep(1500)` (`0x5dc` ms). Each write goes straight to the
transport `vq4.t(byte[], 2000, -1)` (2000 ms timeout). The three payloads:

```
E12B clock           : 2E E1 2B [hour] [min] [sec] [day] [month] [yearHi] [yearLo]   (10 bytes)
E12C next svc date   : 2E E1 2C [day] [month] [yearHi] [yearLo]                        (7 bytes)
E12D service distance: 2E E1 2D [valHi] [valLo]                                        (5 bytes)
```

Field derivation [APK]:
- **Clock (E12B)** comes from Date `p4`, via `Calendar`:
  `hour`=field 11, `min`=field 12, `sec`=field 13, `day`=field 5, `month`=field 2
  **plus 1** (Java months are 0-based → on-wire 1-12), `year`=field 1 as **16-bit
  big-endian** (`yearHi = year>>8`, `yearLo = year&0xFF`). Order on wire is
  **hour, minute, second, day, month, yearHi, yearLo**.
- **Next service date (E12C)** comes from Date `p3`: `day`, `month+1`, `year`
  (BE16). Same day/month/BE-year encoding as the clock's date part.
- **Service distance (E12D)** is the `int distance` argument written as **BE16
  (`distance>>8`, `distance&0xFF`), with NO scaling or arithmetic** in `sl0.C`.

## 3. Byte order / encoding / units — answers to Task 3

- **Byte order:** big-endian for the 16-bit year and for the E12D distance. [APK]
- **Date encoding:** day, month (1-12, after +1), full 16-bit big-endian year.
  MotoScan displays it as `dd.mm.yyyy` (`"%02d.%02d.%04d"`, sl0.r). [APK]
- **Mileage units / scaling (E12D):** the written value is the **raw integer**
  the operator sets. MotoScan's default is the string `"6000"` (`w1.smali:2099`),
  parsed to a double then `double-to-int` with **no multiply/divide** → written
  as-is (`6000` = `0x1770`). So E12D carries the number directly; any km/mi
  scaling is a display-side and/or ECU-side convention, **not** applied in the
  write path. [APK for "no scaling in write"; units themselves = ASSUMPTION]
- **E12D meaning (remaining vs absolute threshold):** `sl0.C` does not compute it
  from the odometer — it just writes the operator's number. Whether the cluster
  interprets that number as remaining distance or an absolute odometer threshold
  is an **[ASSUMPTION]**; the smali does not decide it. Our read-back was
  `0x143C = 5180`, which is simply the currently-stored value.

## 4. Session / prerequisites / security — Task 4

- **No diagnostic-session change (0x10)** and **no security access (0x27)** appear
  in `sl0.C`, its supertypes (`pl0.C`, `el0`), or the surrounding service classes
  inspected. The three `2E` writes are issued directly through the transport. [APK]
- **[ASSUMPTION / UNKNOWN]:** absence in the inspected smali is NOT proof the ECU
  accepts an unauthenticated write. The cluster may still require a session or
  reject the write with an NRC (e.g. 0x33 securityAccessDenied, 0x7F …). This
  must be treated as unresolved until a real (authorized) attempt or further
  evidence. A dry run / NRC observation is the safe way to learn this.
- Native `libmotoscan-helper.so` was not reverse-engineered; a prerequisite
  hidden in native code cannot be excluded.

## 5. How MotoScan calculates next date & mileage — Task 5

From `w1.smali:~2397`:
- **Next service date** = `new Date(now + 0x757B12C00 ms)` = **now + exactly 365
  days**. The **clock** write (E12B) = `new Date(now)` (current time). [APK]
- **Distance** = operator-entered value, default `"6000"` (unscaled). [APK]
- This matches the MotoScan UI ("enter the desired future mileage and date"):
  the date defaults to +1 year, the distance defaults to 6000, both operator-
  editable, and the clock is set to the phone's current time. [APK + PUBLIC docs]

## 6. Cross-reference against the real ECU — Task 6

Applying the **write field order** recovered in §2 to our Phase 7 **read-backs**
yields fully self-consistent values (tests/test_service_codec.py):

| DID | read-back data | decoded (write layout) |
|---|---|---|
| E12B | `00 33 0A 18 0A 07 EA` | clock 24.10.2026 **00:51:10** (hh:mm:ss) |
| E12C | `09 09 07 EA` | next service date **09.09.2026** |
| E12D | `14 3C` | distance **5180** (raw BE16) |
| E119 | `00 00 19 2C` | odometer 6444 (read-only; not written by sl0.C) |

The clock and date years both decode to **2026**, and the clock is a valid
current timestamp — strong confirmation the field layout is correct [BIKE].
(Correction to the Phase 7 guess: the confirmed order is hour, minute, second —
so the clock time is 00:51:10, not the earlier tentative 10:51.)

## 7. Offline encoder / decoder — Task 7

`host/python/openmoto_diag/service_codec.py`: `encode_clock/date/distance` build
the exact `2E` payloads; `decode_*` parse either a `2E` write or a `62` read
response; range guards reject invalid fields. `tests/test_service_codec.py`
passes: decoded real values re-encode to the exact bytes, round-trip is stable
over a range, the real read-backs decode self-consistently, out-of-range inputs
are rejected, and the rollback reference is byte-exact. Nothing transmits.

## 8. Rollback reference — Task 8

`tests/diag_service/rollback-reference.json` preserves the ORIGINAL raw values
(`62 E1 2C 09 09 07 EA`, `62 E1 2D 14 3C`, `62 E1 2B …`, `62 E1 19 …`) and the
`2E` payloads that would restore E12B/E12C/E12D verbatim. This is the undo record
if a reset is ever performed.

## 9. Safety state — Task 9

- No write is enabled anywhere: the firmware still denies `0x2E`/`0x27`/`0x31`/
  `0x10` (serviceForbidden, checked first). This phase added only host-side
  Python (codec + tests + docs); no firmware build changed, no environment gained
  a write path. Nothing was transmitted.
- `r18-kombi-APK.json` remains `verified=false`.

## 10. APK-confirmed vs assumptions — summary

**Confirmed [APK] (and [BIKE] where noted):**
- Exact payloads `2E E1 2B/2C/2D` and their field byte-order (§2), incl. month+1
  and BE16 year.
- E12D is the raw operator integer, written unscaled (§3).
- No 0x10/0x27 in the inspected service code path (§4).
- Next date = now + 365 days; clock = now; distance default 6000 (§5).
- Field layout validated against the real cluster read-backs (§6, [BIKE]).

**Assumptions / unresolved (must clear before any write):**
- Whether the ECU requires a session/security step not visible in the Java smali
  (native code unexamined) — [UNKNOWN].
- E12D semantics: remaining distance vs absolute threshold, and its units — [ASSUMPTION].
- Valid ranges the cluster will accept, NVM persistence, and read-back-after-write
  behaviour — untested.
- Odometer (E119) scaling is unrelated to the writes but still [ASSUMPTION].

## Deliverable status

An evidence-backed, offline-tested service-reset encoder/decoder plus a rollback
reference, ready for final review. A real ECU modification remains a separate,
explicitly-authorized step requiring: resolving the §10 assumptions (ideally a
dry-run/NRC check first), enabling a gated write path with its own ARM/CONFIRM
and bounded safety, and operator sign-off. **Not done in this phase.**
