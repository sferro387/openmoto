# Pre-write baseline verification (task 6) — effectively PASS

Read via diag_write READ command (read-only), ignition ON engine OFF, compared to
the Phase 7 baseline:

  E12C next service date : 62 E1 2C 09 09 07 EA  == baseline  MATCH  (09.09.2026)
  E12D service distance  : 62 E1 2D 14 3C        == baseline  MATCH  (5180)
  E119 odometer          : 62 E1 19 00 00 19 2C  == baseline  MATCH  (6444 km)
  E12B cluster clock     : 62 E1 2B 01 1D 26 18 0A 07 EA   (differs)

The three SERVICE-RELEVANT values are byte-identical to the baseline. The E12B
difference is EXPECTED: it is the cluster clock, which advances with real time.
  phase7: 00:51:10  24.10.2026
  now:    01:29:38  24.10.2026   (same date, ~38 min later; same field structure)
We do NOT write E12B. Conclusion: the original service values are intact and
readable; safe to proceed to the E12D write preparation (still gated on explicit
per-command confirmation).
