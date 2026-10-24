# Second real service-write: E12C (next service date) — SUCCESS (with a verify nuance)

Bike: OBD connected, ignition ON, engine OFF. Owner-authorized single write.

ARM 2E E1 2C 18 0A 07 EB  (DID E12C, MULTI-FRAME)  code=4936
CONFIRM 4936 -> REAL WRITE
RESULT status=READBACK_MISMATCH readback=62E12C090907EA verified=NO ms=36

NUANCE: the firmware got a positive write response (it only read-backs after a
positive 6E), but its IMMEDIATE inline read-back (~tens of ms later) still
returned the OLD value 09 09 07 EA. The cluster committed the new date slightly
later: TWO independent re-reads ~1.4-1.6 s apart both return the NEW value:
  E12C[0]=62E12C180A07EB
  E12C[1]=62E12C180A07EB   (stable -> 24.10.2027)
So the write SUCCEEDED; the MISMATCH was a too-fast inline read-back (a timing
limitation of the verify step for the multi-frame date field), not a bad write.

Collateral checks (must be unchanged):
  E12D=62E12D2710          service distance still 10000  OK
  E119=62E1190000192C      odometer still 6444 km, NOT modified  OK
  E12B clock: not written.

State: IDLE, LISTEN_ONLY, estop=0 lockout=0.

Both target writes are now in place:
  next service date = 24.10.2027   (was 09.09.2026)
  service distance  = 10000 (0x2710) (was 5180)

NEXT: owner to check the dashboard — the service reminder should now clear, since
the stored next-service date is no longer in the past.

Known limitation to note: the inline read-back verify is too fast for the
multi-frame date commit; rely on an independent re-read (as done here). Not a
cluster or data problem.
