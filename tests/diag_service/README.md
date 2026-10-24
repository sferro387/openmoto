# Phase 7 KOMBI service-read results

Raw responses from the supervised, operator-approved service reads on the real
2021 BMW R18 are saved here, one run directory per session
(`run-<UTC>/`), with the raw serial transcript and a per-DID record.

## What gets recorded (task 7: raw bytes, no invented interpretation)

For each of the four reads the firmware prints a `TRACE`/`RESULT` line; we save
the verbatim `RESULT status=OK req=22E1xx resp=62E1xx…` bytes. Decoded values
(odometer, dd.mm.yyyy service date, distance) are written separately and labelled
`[INFERRED-from-APK]` until a plausibility check confirms them against the dash.

## Expected response shapes (from docs/diag-kombi-service-read.md)

| Request | DID | expected resp (UDS) | framing | APK field meaning |
|---|---|---|---|---|
| `22 E1 19` | E119 | `62 E1 19` + 4 bytes | multi-frame | unsigned 32-bit BE odometer (scale [UNKNOWN]) |
| `22 E1 2B` | E12B | `62 E1 2B` + 7 bytes | multi-frame | clock (split [UNKNOWN]) |
| `22 E1 2C` | E12C | `62 E1 2C` + 4 bytes | multi-frame | next service date dd.mm.yyyy (day/month/BE year) |
| `22 E1 2D` | E12D | `62 E1 2D` + 2 bytes | single-frame | service distance, 2-byte BE (km [INFERRED]) |

## Status

No real service-read run recorded yet. The firmware (`diag_service_read`) and the
supervised procedure are prepared and offline-verified; the reads await operator
approval per docs/diag-kombi-service-read.md §6. This file is the destination for
those raw results.
