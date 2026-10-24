# FIRST real service-write: E12D (service distance) — SUCCESS

Bike: OBD connected, ignition ON, engine OFF. Owner-authorized single write.

ARM 2E E1 2D 27 10  (DID E12D, single frame)  code=4B48
CONFIRM 4B48  -> REAL WRITE
RESULT status=OK write=2EE12D2710 readback=62E12D2710 verified=YES ms=30 silent=1
post: state=IDLE writes=1 estop=0 lockout=0  (returned to LISTEN_ONLY)

- ECU accepted the 2E write (positive 6E E1 2D) with NO security/session required.
- Firmware read back 22 E1 2D -> 62 E1 2D 27 10, byte-exact match to what was written.
- New stored value: 0x2710 = 10000 (interpreted as km; UNCONFIRMED unit).
- Previous value (rollback): 2E E1 2D 14 3C (5180).

NEXT: owner must inspect the DASHBOARD service-distance display to confirm the
unit (should read ~10000 km / service in ~10000 km). Do NOT proceed to E12C
(date) until the dash is checked. If the dash is wrong, roll back with
ARM 2EE12D143C / CONFIRM.
