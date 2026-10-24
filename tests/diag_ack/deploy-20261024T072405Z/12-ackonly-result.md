# First ACK-only run on the live R18 bus (OBD connected, ignition ON, engine OFF)

OVERRIDE: performed at operator explicit instruction WITHOUT B1-B4 electrical
verification. Operator accepted the electrical risk. Recorded for the record.

Sequence: boot SILENT -> pre-arm STATUS (SILENT, eflg=0x40 traffic present) ->
ARM (code 030D) -> CONFIRM 030D -> one passive window then one bounded ACK-only
(NORMAL, no TX) window -> auto-return to SILENT.

RESULT verdict=NO_ACK_WAS_MISSING_ACKER
  passive_rx=116      (frames seen while silent/LISTEN_ONLY)
  passive_eflg=0x40   (RX0OVR: frames present but unacknowledged while we were passive)
  active_rx=21        (frames received while we ACKed in NORMAL)
  tec=0 rec=0         (NO error accrual while actively ACKing)
  eflg=0x00           (clean at end of active window)
  spi_err=0
  silent=1            (returned to LISTEN_ONLY)
  fresh_confirm_required=1
Post-run: state=IDLE runs=1 mode=SILENT estop=0  (clean, not latched)

INTERPRETATION
- The bus carries traffic AND accepted our acknowledgment: error counters stayed
  at zero (tec=0 rec=0) throughout the active window. A wrong bitrate or bad
  physical connection would have driven REC/TEC up within ms and forced a fault
  fallback; that did NOT happen.
- verdict NO_ACK_WAS_MISSING_ACKER: the passive window showed the unacknowledged
  signature (eflg RX activity, the same class as the earlier 0x130 observation),
  and once we ACKed in NORMAL the exchange was clean. This supports the Phase 6
  hypothesis: the earlier "no ACK" was at least partly because our LISTEN_ONLY
  tap never acknowledges. The connector behaves like a real CAN bus that expects
  an acknowledging peer.
- This establishes LINK health only. It is NOT a diagnostic exchange and says
  nothing yet about whether KOMBI/the gateway will answer 22 F1 50.
