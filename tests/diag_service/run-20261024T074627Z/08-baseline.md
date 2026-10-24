# R18 KOMBI service baseline — all four reads (real bike, ignition ON, engine OFF)

Session run-20261024T074627Z. Addressing 6F1/660 ext 60/F1. Each read one operator-confirmed
ARM/CONFIRM, status OK, auto-return to LISTEN_ONLY. Writes/reset/security stayed
denied throughout. txn=3 after the batch (plus the earlier E12C = 4 reads total).

## RAW responses (authoritative, verbatim)
| DID | request | raw UDS response | data bytes |
|---|---|---|---|
| E12C next service date | 22 E1 2C | 62 E1 2C 09 09 07 EA | 09 09 07 EA |
| E12D service distance  | 22 E1 2D | 62 E1 2D 14 3C       | 14 3C |
| E119 odometer          | 22 E1 19 | 62 E1 19 00 00 19 2C | 00 00 19 2C |
| E12B clock             | 22 E1 2B | 62 E1 2B 00 33 0A 18 0A 07 EA | 00 33 0A 18 0A 07 EA |

## Decoded values  [INFERRED-from-APK unless marked]
- Next service DATE  (E12C): day=09 month=09 year=0x07EA=2026  -> 09.09.2026 (dd.mm.yyyy)
- Service DISTANCE   (E12D): BE16 0x143C = 5180  (units km [INFERRED]; likely remaining-to-service)
- ODOMETER           (E119): BE32 0x0000192C = 6444  (km [INFERRED])
- CLOCK              (E12B): tail 0x07EA=2026; tentative 24.10.2026 10:51 (sec/min/hr + day/mon/year)

## Consistency checks (support the inferences, not proof)
- E119 odo 6444 km: plausible mileage for the bike.
- E12B clock decodes to a coherent current timestamp (24.10.2026 10:51), year 2026
  matches E12C year -> byte interpretation is internally consistent.
- odo(6444) + dist(5180) = 11624; a 10000 km interval target would be ~16444.
  Whether E12D is remaining / target / interval is NOT resolved -> keep [INFERRED].

## Status
RAW bytes are the record. Decodes are APK-derived hypotheses; confirm each against
what MotoScan / the dashboard shows before relying on them for a future reset.
No write/reset performed. r18-kombi-APK.json still verified=false; B1-B4 unverified.
