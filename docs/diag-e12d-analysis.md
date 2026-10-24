# E12D semantics determination (service distance) — Phase 9 task 4

Goal: decide what E12D represents BEFORE choosing an encoded value to write, so we
never write the wrong quantity/unit to the cluster.

Evidence levels: **[APK]** MotoScan smali; **[BIKE]** our live read-backs;
**[LOGIC]** inference from the numbers; **[ASSUMPTION]** unresolved.

## Live values
- E119 odometer raw = `00 00 19 2C` = **6444**
- E12D raw = `14 3C` = **5180**
- E12C next service date = 2026-09-09 (a date in the FUTURE relative to capture)

## Candidate meanings and tests
1. **Absolute odometer threshold** (service due when odo reaches E12D):
   E12D(5180) < odo(6444) -> would be already overdue. But the service DATE is
   2026-09-09 (future). Contradiction. **[LOGIC] rejects "absolute threshold".**
2. **Remaining distance to next service** (counts down as you ride):
   next-service odo = odo + E12D = 6444 + 5180 = 11624. Self-consistent with a
   future date and a partially-used interval. **[LOGIC] supports "remaining".**
3. **Interval length** written once: indistinguishable from (2) at write time
   (both are "a distance value written directly"); the cluster decrements it.

## APK behaviour [APK]
- `sl0.C` writes E12D as the raw `int` argument, **BE16, no scaling, no odometer
  arithmetic** (it does NOT write odo+interval).
- `w1` default distance is the string `"6000"`, parsed and `double-to-int`, with
  **no unit conversion** in the write path.
- No km<->mile conversion code found near the service-distance write.

## Determination
E12D is a **distance value written directly to the cluster (BE16, unscaled)** that
the cluster treats as **distance remaining to the next service** (not an absolute
odometer threshold). [LOGIC: strong; APK: consistent]. The exact **unit** the
cluster uses is **[ASSUMPTION]**: the live numbers (odo 6444, remaining 5180) are
self-consistent as **kilometres**, and no conversion appears in the APK, so we
treat E12D as **kilometres, raw**.

## UNIT TRAP (the owner's explicit warning)
- MotoScan's default `"6000"` is almost certainly the **mile** interval
  (~6000 mi ≈ 10000 km) for imperial-market bikes — NOT 6000 km.
- The owner wants a **6,000 km** interval. Since E12D is written **unscaled in the
  cluster's own unit**, and our evidence says that unit is **km**, a 6,000 km
  interval encodes as **6000 = 0x1770**.
- If the cluster unit were actually miles, 6000 km would need ~3728 and 0x1770
  would mean 6000 miles (~9656 km). The km interpretation is supported by the live
  odometer being a plausible km value, but it is **not proven**. This MUST be
  confirmed (owner cross-check vs dash / a conservative first value) before writing.

## Chosen encoded value (pending owner confirmation)
- Target interval: **6,000 km** (owner's choice).
- Under the km interpretation: E12D value = **6000 = 0x1770** -> payload `2E E1 2D 17 70`.
- Current stored value (rollback) = 5180 = `0x143C` -> `2E E1 2D 14 3C`.

## Residual risk
- Unit (km vs mile) is [ASSUMPTION]; a read-back-after-write that shows a sane
  remaining distance on the dash is the confirmation. The rollback value `14 3C`
  restores the original exactly if the result looks wrong.
