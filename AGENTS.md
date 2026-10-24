# OpenMoto project instructions

- Read [HANDOFF.md](HANDOFF.md) for the current verified state, commands and evidence.
  Phase 1 and Phase 2 passed on bench hardware; no vehicle compatibility is claimed.
- Phase 3A (CAN1 LISTEN_ONLY sniffer, `src/phase3a/`) passed on bench hardware;
  see docs/phase3a.md. Phase 3B (`src/phase3b/`, `p3b_*` environments, binary
  protocol) is benchmarked; see docs/phase3b.md for results and open limits. Do not start later Phase 3 work or change working firmware
  without a new user request. Preserve unrelated work and real logs/results.
- Phase 4 (read-only active diagnostics, `src/diag/`, `diag_dryrun`/`diag_readonly`
  envs) is designed, implemented and tested OFFLINE only; `p3b_sniffer` is
  unchanged. The verdict is **NO-GO**: nothing may be transmitted to the
  motorcycle until docs/diag-active.md prerequisites (P0–P4) are met and a
  verified addressing profile exists. See docs/diag-active.md and docs/diag-design.md.
- KOMBI service-reset research (MotoScan APK static analysis, OFFLINE): KOMBI =
  ECU address 0x60, tester 0x6F1, ELM/STN protocol-B extended addressing; reads
  `22 E1 19/2B/2C/2D`, reset writes `2E E1 2B/2C/2D` (prefixes confirmed [APK],
  payloads/session [UNKNOWN]). See docs/diag-kombi-findings.md. Writes stay
  default-denied; the offline trace replay tool (`host/python/diag_replay.py`,
  `openmoto_diag/trace.py`+`replay.py`) blocks them. Still NO-GO to write.
- Phase 5 (first KOMBI read-only probe `22 F1 50`, OFFLINE prep): a compile-time-
  gated extra DID (`OPENMOTO_DIAG_KOMBI_PROBE`, envs `diag_kombi_probe_dryrun`/
  `diag_kombi_probe`; host `--kombi-probe`). Frame = `6F1 / 60 03 22 F1 50 00 00 00`,
  response on `0x660` ext `F1`. Default builds still deny `22 F1 50`; Phase 4 HEX
  unchanged. Addressing is APK-derived (profile `verified=false`, refused for tx).
  Verdict NO-GO to transmit: no confirmed healthy diagnostic tap. See
  docs/diag-kombi-probe.md.
- Phase 6 (ACK-only CAN bring-up, `src/diag_ack/`, env `diag_ack`): dedicated
  firmware to establish link health before the Phase 5 probe. Boots LISTEN_ONLY,
  never auto-enters NORMAL; ARM+CONFIRM runs a passive window then a bounded
  ACK-only (NORMAL, no TX) window and prints a verdict distinguishing healthy /
  no-ACK-was-us / wrong-bitrate / no-connection. NO transmit path compiled in
  (audited). ACK-only is ACTIVE bus participation (hardware ACK + error frames).
  Reframes the 0x130 capture: passive LISTEN_ONLY can't ACK, so "no ACK" was
  ambiguous. Phase 3B/4/5 HEX unchanged. Verdict GO-conditional for a short
  supervised ignition-ON/engine-OFF test after B1-B4. See docs/diag-ack-bringup.md.
- Phase 6 ACK-only test RAN on the real R18 (ignition ON, engine OFF; B1-B4
  overridden by operator): verdict NO_ACK_WAS_MISSING_ACKER, tec=0 rec=0, link
  healthy at 500 kbps. Phase 5 probe `22 F1 50` then RAN on the bike: KOMBI
  replied `0x660 / F1 06 62 F1 50 0F 90 70` (status OK, 15 ms). Addressing/
  transport are now BIKE-confirmed. Logs in tests/diag_ack/deploy-*/.
- Phase 7 (read-only KOMBI service inspection, flag `OPENMOTO_DIAG_SERVICE_READ`,
  envs `diag_service_read_dryrun`/`diag_service_read`; host `--service-read`):
  adds EXACTLY four service DIDs `22 E1 19/2B/2C/2D` (odometer/clock/service-date/
  service-distance), all 0x22 reads. E119/E12B/E12C are multi-frame (one FC frame);
  E12D single-frame. Writes/reset/security still denied (audited). Offline tests
  pass; Phase 3B/4/5/6 HEX unchanged. Raw bytes recorded verbatim; field decodes
  are [APK]/[INFERRED] until [BIKE]-confirmed. See docs/diag-kombi-service-read.md.
- Phase 7 reads RAN on the real R18: E12C `09 09 07 EA` (next service 09.09.2026),
  E12D `14 3C` (5180), E119 `00 00 19 2C` (odo 6444), E12B clock. Baseline saved
  tests/diag_service/ (r18-kombi-baseline.txt, run-*/).
- Phase 8 (service-reset encoding, reverse-engineered OFFLINE from MotoScan
  sl0.C): payloads `2E E1 2B` clock=[hh mm ss dd MM yyyyBE16], `2E E1 2C` date=
  [dd MM yyyyBE16], `2E E1 2D` dist=[BE16 raw int, no scale]. Next date = now+365d,
  clock = now, distance default 6000. NO 0x10 session / 0x27 security in the
  inspected smali (but native code unexamined -> UNKNOWN). E12D remaining-vs-
  absolute = ASSUMPTION. Offline codec + round-trip tests
  (host/python/openmoto_diag/service_codec.py, tests/test_service_codec.py);
  rollback reference tests/diag_service/rollback-reference.json. NO write enabled,
  nothing transmitted. See docs/diag-kombi-service-reset.md.
- Phase 9 (service WRITE firmware, SEPARATE core src/diag_write/write_core.h,
  envs diag_write_dryrun/diag_write): adds ISO-TP MULTI-FRAME TRANSMIT (missing
  until now) + mandatory read-back verify. Only 2E E1 2B/2C/2D armable; per-write
  ARM/CONFIRM; NEGATIVE/pending/FC/readback-mismatch handled; rollback on mismatch.
  Read-only HEX byte-identical (separate tree). Offline tests
  tests/test_diag_write.py pass (mock KOMBI, multi-frame). E12D = remaining
  distance, km, raw BE16 (docs/diag-e12d-analysis.md). TARGET writes (owner):
  E12C 2027-09-09 = `2E E1 2C 09 09 07 EB`, E12D 6000 km = `2E E1 2D 17 70`
  (E12B clock not written). NOT transmitted — stopped at owner-authorization gate.
  See docs/diag-service-write-plan.md.
- Use `./scripts/pio` with an explicit environment from `firmware/mega-can/platformio.ini`.
  Keep the existing pinned tools/dependencies and separate Phase 1/2 source filters.
- Building is separate from uploading or opening serial: the latter can reset the
  Mega and transmit. The user has granted standing approval to upload bench
  firmware and run bench tests without asking. Before each upload, enumerate the
  port with `./scripts/pio device list` and use the Mega (VID:PID 2341:0042).
- USB-only isolated bench. No motorcycle/ECU/OBD/external CAN node or external 12 V.
  Disconnect power before rewiring/resistance checks; stop on hardware uncertainty.
- Treat raw captures as evidence; mocks/build success are not physical test success.
  Save new runs under unique filenames and report failures without bypassing them.
- See [hardware safety](.kiro/steering/hardware-safety.md),
  [technology](.kiro/steering/tech.md) and [structure](.kiro/steering/structure.md)
  for focused guidance. Keep these instructions concise; maintain details in docs.
