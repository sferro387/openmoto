# diag_kombi_probe post-flash verification (OBD disconnected)

Port /dev/cu.usbmodem21101 ; HEX a30dded25c483094 ; flashed+verified 15670 bytes.

Boot (silent):
  MCP2515 #1 SPI: PASS / #2 SPI: PASS
  banner: Phase 4 read-only diagnostic client, BUILD tx=ENABLED
  READY ; STATUS state=IDLE addr=UNSET pad=UNSET txn=0 fails=0 estop=0 lockout=0 tx_build=1

Checklist:
- boots silent, IDLE, addr/pad UNSET ........ PASS (no addressing until operator sets it)
- no automatic diagnostic request ........... PASS (txn=0; nothing armed/sent at boot)
- write DID (2E) denied ...................... PASS (POLICY DENY service-forbidden)
- security access (27) denied ............... PASS (POLICY DENY service-forbidden)
- ECU reset (11) denied ...................... PASS (POLICY DENY service-forbidden)
- routine control (31) denied ............... PASS (POLICY DENY service-forbidden)
- KOMBI probe 22 F1 50 allowed (read) ....... PASS (POLICY ALLOW ReadDataByIdentifier)

Notes:
- banner says BUILD tx=ENABLED and "read-only diagnostic client": tx path is the
  bounded one-shot read path; writes/security/reset remain denied (verified above).
- POLICY checks are non-transmitting: they report allow/deny only, nothing armed.
- CAN2 held in CONFIG (SPI #2 PASS at init; never leaves CONFIG in this build).
