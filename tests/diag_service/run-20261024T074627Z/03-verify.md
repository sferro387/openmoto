# diag_service_read post-flash verification (OBD disconnected)

Port /dev/cu.usbmodem21101 ; HEX c8aee63491dce1e4 ; flashed+verified 15692 bytes.

Boot: MCP2515 #1/#2 SPI PASS ; banner read-only diagnostic client ; READY.
STATUS: state=IDLE addr=UNSET pad=UNSET txn=0 estop=0 lockout=0 -> silent, nothing armed, no auto-request.

Policy (non-transmitting checks):
- 22 E1 19 ALLOW   (odometer)
- 22 E1 2B ALLOW   (clock)
- 22 E1 2C ALLOW   (service date)
- 22 E1 2D ALLOW   (service distance)
- 2E E1 2C 00 DENY service-forbidden   (write blocked)
- 27 01 DENY service-forbidden         (security blocked)
- 11 01 DENY service-forbidden         (ECU reset blocked)
- 31 01 0F 06 DENY service-forbidden   (routine blocked)
- 22 F1 50 DENY DID-not-allowlisted    (Phase 5 probe DID NOT in this build -> correct mode isolation)

Checklist:
- boots silent, IDLE, addr/pad UNSET ............. PASS
- no automatic diagnostic request ................ PASS (txn=0)
- four service reads allowed ..................... PASS
- write/security/reset/routine denied ............ PASS
- Phase 5 F150 probe isolated (denied here) ...... PASS
