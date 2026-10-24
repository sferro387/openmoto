# Startup verification (post-flash, OBD disconnected)

Port: /dev/cu.usbmodem21101   HEX: 5244f058...   flashed+verified 11372 bytes

Checklist (instruction 6):
- CAN1 starts in LISTEN_ONLY ....... PASS (STATUS: mode=SILENT, state=IDLE; LISTEN_ONLY is the silent idle)
- CAN2 remains in CONFIG ........... PASS (MCP2515 #2 CONFIG: PASS at init; held in CONFIG)
- SPI init pass .................... PASS (MCP2515 #1 SPI self-check: PASS)
- Controller init pass ............. PASS (#1 CONFIG PASS, #2 CONFIG PASS, bitrate 500k/8MHz PASS)
- No CAN TX requests pending ....... PASS (ACK-only build: no TX path linked; tec=0 rec=0; TXREQ guards)
- No ACK-only NORMAL activated ..... PASS (mode=SILENT, runs=0; no ARM/CONFIRM issued)

Self-checks run (non-transmitting): power-on init self-check (SPI readback,
bitrate, both controllers CONFIG) + 2x STATUS. No ARM, no CONFIRM, no NORMAL.
