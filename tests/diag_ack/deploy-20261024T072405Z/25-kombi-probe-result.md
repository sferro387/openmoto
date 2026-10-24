# FIRST genuine KOMBI response — 22 F1 50 read (OBD connected, ignition ON, engine OFF)

Operator-approved single transmission. No retries. No write/reset/security.
Note: performed WITHOUT B1-B4 electrical verification, at operator instruction.

Addressing used (MotoScan-derived, as specified):
  ADDR 6F1 660 60 F1 ; PAD 00
  tx_id=6F1 txext=60 rxext=F1 rx_id=660

TX (single frame):  id=6F1 dlc=8 data=60 03 22 F1 50 00 00 00
RX (single frame):  id=660 dlc=8 data=F1 06 62 F1 50 0F 90 70
  -> ext byte F1 stripped, ISO-TP SF len=6
  -> UDS positive response: 62 F1 50 0F 90 70
     62     = positive response to service 22 (ReadDataByIdentifier)
     F1 50  = echoed DID (KOMBI identification)
     0F 90 70 = 3 identification data bytes (meaning not yet decoded)

RESULT status=OK  frames_tx=1 frames_rx=1 ignored=0 pending=0 ms=15 silent=1
Post-run: state=IDLE txn=1 fails=0 estop=0 lockout=0 ; returned to LISTEN_ONLY.

INTERPRETATION
- KOMBI answered on the expected response id 0x660 with the expected extension
  byte 0xF1 and a valid UDS positive response echoing DID F150. This is the first
  genuine ECU response: the addressing, bitrate, ISO-TP framing and read policy
  are all confirmed correct against the real 2021 R18 instrument cluster.
- Round-trip 15 ms, single request frame, single response frame, no pending, no
  errors; controller auto-returned to SILENT (not latched).
- This is READ-ONLY identification. No service reset, write, coding, security or
  session change was performed or is enabled.

OUTSTANDING
- The 3 data bytes 0F 90 70 are not yet decoded (no verified field map).
- B1-B4 remain electrically unverified (functional success is not an electrical
  safety proof).
- r18-kombi-APK.json remains verified=false (unchanged).
