# Passive observation on the live R18 bus (OBD connected, ignition ON)

Mode held LISTEN_ONLY (silent) the entire time. No ARM, no CONFIRM, no NORMAL.
Nothing transmitted. This is NOT an electrical-safety check and does NOT clear B1-B4.

Observed STATUS (x6 over ~9s):
  state=IDLE runs=0 mode=SILENT tec=0 rec=0 eflg=0x40 estop=0  (stable)

Reading:
- eflg=0x40 => RX0OVR (receive-buffer overflow) set: frames ARE arriving at our
  500 kbit/s timing, faster than the STATUS-only loop drains them. The tap is
  live and carries CAN traffic.
- tec=0 rec=0: no error accrual (expected in LISTEN_ONLY; counters deactivated).
- Mode never left SILENT; nothing armed/activated.

Still outstanding before ANY ACK-only (NORMAL) window:
- B1 pinout, B2 termination, B3 ground path, B4 CS pull-ups: UNVERIFIED.
  A live-traffic observation does not substitute for these measurements.
