---
inclusion: always
---
# Product

OpenMoto is an open motorcycle diagnostic platform in an isolated USB bench
proof-of-concept stage. Long term: commercial BLE dongle, iOS/Android applications,
and independently verified vehicle-specific service interval resets.

Future targets are BMW R18 (2021), BMW R1200GS LC (2015) and an Aprilia Tuono whose
exact model/year is unconfirmed. These are goals, not supported vehicles.

Phase 1 internal loopback and Phase 2 dual-MCP2515 physical CAN passed on real
hardware. Phase 2 verified 2200/2200 frames with zero reported errors. No vehicle,
ISO-TP, UDS, OBD diagnostics or service reset is validated. Do not equate working
CAN transport with diagnostic compatibility. Stop at completed Phase 2 until the
user explicitly requests the next milestone. See HANDOFF.md for evidence/roadmap.
