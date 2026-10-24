# diag_write (with READ) reflash verification (OBD disconnected)

Flashed+verified 15674 bytes. Boot: CONFIG/bitrate PASS, SERVICE-WRITE client, READY.
STATUS: IDLE addr=UNSET pad=UNSET writes=0 estop=0 lockout=0 -> silent, nothing armed.

READ gating (non-transmitting):
- READ 22F190       -> ERR read-not-allowed (only 22 E1 19|2B|2C|2D)   PASS
- READ 2EE12D2710   -> ERR read-not-allowed (READ cannot issue a 2E)    PASS

Offline (remote): test_diag_write.py PASS (reads + writes + guards).
Read-only firmware HEX unchanged (diag_readonly ff73ca37, diag_kombi_probe a30dded2,
diag_service_read c8aee634, diag_ack 5244f058, p3b_sniffer 4c684a9e).
