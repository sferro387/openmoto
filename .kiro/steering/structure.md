---
inclusion: always
---
# Structure

- `HANDOFF.md`: start here for verified state, evidence and roadmap.
- `AGENTS.md`: concise instructions shared across Codex/Kiro.
- `firmware/mega-can/platformio.ini`: authoritative build pins/environments.
- `firmware/mega-can/src/main.cpp`: preserved Phase 1 setup()/loop() entry points.
- `firmware/mega-can/src/phase2/main.cpp`: separate Phase 2 one-shot setup() suite;
  loop() is empty. Independent controller init, four exchanges, stop/report.
- `firmware/mega-can/src/phase3a/main.cpp`: Phase 3A CAN1 LISTEN_ONLY sniffer;
  `OPENMOTO_SNIFFER_BENCH=1` adds the CAN2 BENCH traffic generator.
- `firmware/mega-can/src/phase3b/`: Phase 3B sniffer (`main.cpp`) and binary
  protocol encoder (`protocol.h`, shared with host tests). Do not edit 3A for 3B.
- `tests/phase3b/<label>-<UTC>/`: Phase 3B run evidence (`.raw.gz` captures,
  summaries, logs); `*.frames.jsonl` is regenerated with `--from-raw`.
- `scripts/pio`: existing project-local CLI wrapper.
- `host/python/`: serial capture/validation scripts and pinned host requirements.
- `tests/`: immutable original hardware logs/results plus host-only mock checks.
- `docs/`: chronological bring-up, wiring, architecture and Phase 2 procedure.
- `.kiro/steering/`: focused guidance, not a second copy of the full handoff.

Keep the two source trees isolated by build_src_filter. Do not fold Phase 2 into
the Phase 1 entry point. Preserve `phase1-verified-loopback` and real captures.
Never commit `.tools/`, `.pio/`, `.venv/`, caches, passwords or private keys.
When a new run occurs, choose new evidence filenames and link actual
results from docs; do not overwrite original captures or label mocks as hardware.
