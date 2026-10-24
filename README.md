# OpenMoto

CAN/UDS diagnostics for BMW Motorrad, built on an Arduino Mega 2560 with
MCP2515/TJA1050 transceivers. OpenMoto grew from a passive CAN sniffer into a
gated, read-back-verified diagnostic client — and, as of this release, a working
**service-reminder reset for the 2021 BMW R18**.

## Milestone: BMW R18 service reset ✅

The service reminder on a real 2021 BMW R18 instrument cluster (KOMBI) was reset
end-to-end, with every step gated and verified:

1. Passive capture → confirmed the diagnostic CAN addressing.
2. ACK-only bring-up → proved the link is healthy at 500 kbit/s.
3. First real UDS read (`22 F1 50`) → KOMBI identified itself on `0x660`.
4. Read the current service baseline (date / distance / odometer / clock).
5. Reverse-engineered the write encoding from documented evidence.
6. Wrote the two service values and **verified each by read-back**:
   - service distance `E12D` → `2E E1 2D 27 10`
   - next service date `E12C` → `2E E1 2C 18 0A 07 EB` (2027-10-24)
7. Odometer (`E119`) and clock (`E12B`) left untouched; originals preserved for
   rollback.

Every write requires explicit per-command `ARM`/`CONFIRM`. The read-only and
write firmwares are separate builds; read-only images are byte-stable (hashes in
`tests/`). See `docs/` for the full evidence trail and the GO/NO-GO reports.

## Hardware

- Arduino **Mega 2560** (VID:PID 2341:0042)
- Two **MCP2515 + TJA1050** CAN modules, **8 MHz** oscillator
  - CAN1: CS D10, INT D2  — the diagnostic interface
  - CAN2: CS D9, INT D3   — held in CONFIG (unused/disconnected)
- 500 kbit/s CAN
- A BMW Motorrad diagnostic connector → OBD adapter for the R18

> Electrical prerequisites (connector pinout, termination, single ground path)
> are the operator's responsibility; see `docs/vehicle-readiness-r18.md`.
> Writing to a cluster modifies NVM — understand the risk before using the
> write firmware on a vehicle.

## Firmware environments (`firmware/mega-can/platformio.ini`)

| Env | Purpose |
|---|---|
| `p3b_sniffer` | Phase 3B passive CAN sniffer (LISTEN_ONLY) |
| `diag_readonly` / `diag_dryrun` | Phase 4 read-only UDS client (tx / no-tx) |
| `diag_kombi_probe` | Phase 5 KOMBI identification probe (`22 F1 50`) |
| `diag_service_read` | Phase 7 service reads (`22 E1 19/2B/2C/2D`) |
| `diag_ack` | Phase 6 ACK-only CAN bring-up |
| `diag_write` / `diag_write_dryrun` | Phase 9 service WRITE (`2E E1 2B/2C/2D`) + READ |

Each capability is a compile-time-gated build; mutating services are default-denied.

## Basic usage

Build / flash (use the project wrapper; flash with the OBD **disconnected**):

```sh
./scripts/pio device list                       # find the Mega port
./scripts/pio run -d firmware/mega-can -e diag_service_read            # build
./scripts/pio run -d firmware/mega-can -e diag_service_read -t upload --upload-port <PORT>
./scripts/pio device monitor -p <PORT> -b 115200
```

Operator serial commands (read example):

```
ADDR 6F1 660 60 F1     # tester 0x6F1, response 0x660, ext tx 0x60 / rx 0xF1
PAD 00
READ 22E12C            # read next-service-date DID (diag_write build)
```

Writes additionally require `ARM <bytes>` then `CONFIRM <code>` per command, and
are read-back verified. See `docs/diag-service-write-plan.md` for the full
supervised procedure and `docs/diag-kombi-service-reset.md` for the encoding.

## Tests

Offline tests run against mock ECUs / simulated controllers (no hardware needed):

```sh
python3 tests/test_diag.py              # read-only client
python3 tests/test_diag_service_read.py # service reads
python3 tests/test_service_codec.py     # write encoder/decoder round-trip
python3 tests/test_diag_write.py        # write core (multi-frame + read-back)
python3 tests/test_diag_ack.py          # ACK-only bring-up
python3 tests/check_diag_firmware.py    # static firmware audits (needs toolchain)
```

## Repository layout

- `firmware/mega-can/` — PlatformIO project and the diagnostic firmwares
- `host/python/openmoto_diag/` — host policy mirror, profiles, codec, CLIs
- `tests/` — offline tests, fixtures, and sanitized vehicle-run evidence
- `docs/` — bring-up, protocol findings, and per-phase GO/NO-GO reports

## Safety

Read-only by default; mutating UDS services are denied unless a specific write
build is used. Nothing transmits without explicit operator confirmation, and the
idle state is always silent (LISTEN_ONLY). This is a research/DIY tool, not a
substitute for a manufacturer tester.
