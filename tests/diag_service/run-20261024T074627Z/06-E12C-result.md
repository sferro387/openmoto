# 22 E1 2C — next service date (real R18 read)

Bike state: OBD connected, ignition ON, engine OFF. Single operator-approved read.

TX: id=6F1 dlc=8 data=60 03 22 E1 2C 00 00 00
RX (multi-frame, 1 flow-control frame):
  FF: id=660 data=F1 10 07 62 E1 2C 09 09   (first frame, total len=7)
  FC: id=6F1 data=60 30 00 02 ...           (our ClearToSend, BS=0, STmin)
  CF: id=660 data=F1 21 07 EA               (consecutive frame)
RESULT status=OK  resp=62 E1 2C 09 09 07 EA  frames_tx=2 frames_rx=2 ms=14 silent=1
Post-read: state=IDLE txn=1 estop=0 lockout=0 (returned to LISTEN_ONLY).

RAW (authoritative): 62 E1 2C 09 09 07 EA
  62 E1 2C = positive response to ReadDataByIdentifier, DID E12C echoed
  data bytes = 09 09 07 EA   (4 bytes)

TENTATIVE DECODE [INFERRED-from-APK, NOT yet bike-confirmed]:
  MotoScan sl0.r() renders E12C as dd.mm.yyyy with day=buf[6], month=buf[7],
  year=(buf[8]<<8)+buf[9] (16-bit BE). Mapping onto the 4 data bytes 09 09 07 EA:
    day   = 0x09 = 9
    month = 0x09 = 9  (September)
    year  = 0x07EA = 2026
  => tentative next service date 09.09.2026 (dd.mm.yyyy).
  CAVEAT: the APK byte offsets are into MotoScan buffer layout; the mapping onto
  our stripped UDS payload is [INFERRED]. Confirm against what the dash/MotoScan
  shows before trusting it. 0x07EA=2026 is a plausible year, which is consistent.
