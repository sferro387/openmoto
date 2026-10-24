# diag_write post-flash verification (OBD disconnected)

Port /dev/cu.usbmodem21101 ; flashed+verified 15044 bytes.
Boot: MCP2515 #1/#2 CONFIG PASS, bitrate 500k/8MHz PASS, SERVICE-WRITE client, READY.
STATUS: state=IDLE addr=UNSET pad=UNSET writes=0 estop=0 lockout=0 -> silent, nothing armed.

Gating (non-transmitting, OBD disconnected):
- ARM before ADDR/PAD        -> ERR not-configured            PASS
- ARM 2F.. (IO control)      -> ERR not-an-allowed-service-write PASS
- ARM 27.. (security)        -> ERR not-an-allowed-service-write PASS
- ARM 2E E1 AA (wrong DID)   -> ERR not-an-allowed-service-write PASS
- ARM 11.. (ECU reset)       -> ERR not-an-allowed-service-write PASS
- after all attempts: STATUS still IDLE, writes=0  PASS (nothing armed/sent)

Only 2E E1 2B/2C/2D (exact length) can be armed. No automatic write at boot.
