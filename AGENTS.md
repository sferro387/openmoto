# OpenMoto project instructions

- Read [HANDOFF.md](HANDOFF.md) for the current verified state, commands and evidence.
  Phase 1 and Phase 2 passed on bench hardware; no vehicle compatibility is claimed.
- Phase 3A (CAN1 LISTEN_ONLY sniffer, `src/phase3a/`) passed on bench hardware;
  see docs/phase3a.md. Do not start later Phase 3 work or change working firmware
  without a new user request. Preserve unrelated work and real logs/results.
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
