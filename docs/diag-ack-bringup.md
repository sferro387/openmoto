# Phase 6: BMW R18 diagnostic-bus bring-up (ACK-only)

Date: 2026-10-24. Scope: prepare — offline only — a dedicated ACK-only CAN
bring-up firmware that establishes whether the R18 diagnostic connector is a
healthy CAN bus BEFORE the first KOMBI `22 F1 50` request (Phase 5). **Nothing
was transmitted to the motorcycle. The bike is disconnected.** The firmware is
built and tested against a simulated controller; the first real test is operator-
gated and described in §8.

Evidence levels: **[DS]** MCP2515 datasheet; **[CAP]** our capture;
**[FW]** proven from our firmware/tests; **[INFERRED]**/**[UNKNOWN]** unverified.

## 0. The question this phase answers

Phase 3-5 concluded from a passive capture that the only R18 bus we probed showed
continuous `0x130` frames at the error-passive, unacknowledged rate, and read that
as "wrong tap / isolated segment." The task correctly challenges that: **a passive
LISTEN_ONLY capture cannot prove a bus is unhealthy, because our own controller
refuses to acknowledge in LISTEN_ONLY mode.** This phase builds the minimal active
test that can tell the difference, and explains why it is active.

## 1. MCP2515 LISTEN_ONLY vs NORMAL — the acknowledgment fact [DS]

- In **LISTEN_ONLY** mode the MCP2515 is fully passive: it **sends no ACK and no
  error frames**, and its transmit-error/receive-error counters are deactivated
  (DS20001801 §10.1, and the project's own S8 in docs/vehicle-readiness-r18.md).
  It is a silent observer.
- In **NORMAL** mode the MCP2515 participates: for every valid frame it receives,
  it drives the ACK slot dominant (acknowledges), and on bit/stuff/CRC/form errors
  it transmits CAN error frames. It can also transmit application frames — but
  only if a TX buffer is loaded and a Request-To-Send is issued.
- There is **no mode that acknowledges but cannot transmit.** ACK generation is a
  property of NORMAL mode. So "ACK-only" is NORMAL mode with a firmware guarantee
  that no TX buffer is ever loaded.

**Consequence for the `0x130` capture [CAP][INFERRED]:** our capture was taken in
LISTEN_ONLY. The transmitting node got no ACK partly/possibly **because the only
other device present (us) was configured never to ACK.** The error-passive timing
signature is therefore consistent with *both* "wrong/isolated bus" *and* "normal
bus where the tester is expected to be the ACKing peer and we weren't ACKing."
Passive observation alone cannot separate these. That is exactly why an ACK-only
test is needed before concluding anything about the connector.

## 2. Why ACK-only is ACTIVE participation (not electrically passive) [DS]

Entering NORMAL mode, even with zero application transmissions, means the
controller **drives the bus**: it pulls the ACK slot dominant for frames it
accepts, and it can emit error frames. Electrically and per CAN protocol this is
active participation — the node is now a full bus peer, not a tap. This is stated
in the firmware boot banner and must be understood by the operator: the moment
ACK-only is confirmed, the Mega is talking on the R18 bus (ACK bits + possibly
error frames), even though it never sends a diagnostic request.

## 3. Can the MCP2515 ACK while suppressing all application TX? [DS][FW]

Yes — by construction in this firmware:

- **No transmit path is compiled in.** The ACK-only HAL (`ack_core.h AckHal`) has
  no "load TX buffer" or "request to send" method. The glue never issues the
  MCP2515 Load-TX-Buffer (0x4x) or RTS (0x8x) SPI commands. Proven by
  `tests/check_diag_ack_firmware.py`: every `SPI.transfer` caller in the linked
  image is a register-read/write/mode helper; no `sendMessage`/`loadTxBuffer`/
  `requestToSend`/transmit routine is linked.
- **TXREQ verified clear.** Before and during NORMAL mode, all three
  `TXBnCTRL.TXREQ` bits are checked 0 (`allTxIdle`); any pending TX forces an
  immediate fallback.
- **TXnRTS pins disabled.** `TXRTSCTRL` is written 0 so the hardware RTS pins are
  digital inputs and an external edge cannot trigger a transmission
  (`disableRtsPins`, re-asserted on every CONFIG entry).

So the only thing on the wire from us is the hardware-generated ACK (and error
frames if the bus is misconfigured). No application frame can leave.

## 4. The bring-up test design [FW]

One run has two bounded windows, then a verdict:

1. **Passive window** (LISTEN_ONLY, truly silent, 2 s): count received frames and
   record EFLG/REC. We do not ACK here.
2. **ACK-only window** (NORMAL, no TX, 2 s): we now ACK. Error counters are
   polled every 20 ms; any escalation (`TEC>=16`, `REC>=96`, error-passive or
   bus-off flag, loss of NORMAL, unexpected TXREQ, or an SPI self-check failure)
   triggers immediate fallback to LISTEN_ONLY/CONFIG.
3. **Verdict** from the two windows:

| Verdict | Meaning | Signature |
|---|---|---|
| `HEALTHY_BUS_ACK_ACCEPTED` | healthy bus, another node also present | frames in both windows, counters clean, passive window had no RX errors |
| `NO_ACK_WAS_MISSING_ACKER` | frames were unacknowledged while we were passive, and cleaned up once we ACKed | passive window showed RX-error flags (the `0x130`-style signature); active window received frames with clean counters |
| `WRONG_BITRATE` | our 500 kbit/s disagrees with the bus | little/no valid rx, REC/TEC climb fast → fallback |
| `NO_CONNECTION_OR_SILENT_BUS` | nothing on the wire | no rx in either window, counters stay 0 |
| `FAULT_FORCED_FALLBACK` | a fault (SPI, unexpected state) ended the run early | — |

A `HEALTHY`/`NO_ACK_WAS_MISSING_ACKER` verdict means the connector IS a usable CAN
bus and (for the latter) that the earlier "wrong tap" read was a passive-mode
artifact. "Diagnostic gateway not responding" is not decided here — that is the
Phase 5 `22 F1 50` step; this phase only establishes link health.

## 5. Firmware safety model [FW]

- Boots in **LISTEN_ONLY**; never auto-enters NORMAL. ACK-only requires `ARM`
  then `CONFIRM <code>` (code derived from a counter+time, printed by firmware,
  single-use, 30 s TTL). A wrong code disarms.
- Bounded ACK-only window (2 s) with continuous monitoring of mode, EFLG, TEC,
  REC, SPI integrity and rx count; immediate fallback on any fault.
- `STOP` returns to LISTEN_ONLY on demand; `!` / `ESTOP` latches an emergency
  stop that blocks further ARM until reset.
- Every run ends by re-verifying a silent state and demands a **fresh**
  confirmation for the next run (`fresh_confirm_required=1`). Run count is capped
  per power cycle.
- No transmit path exists; CAN2 is held in CONFIG and is physically disconnected.
- Existing Phase 3B/4/5 firmware is untouched: `src/diag_ack/` is a separate
  source tree and `[env:diag_ack]` a separate environment. Rebuilds confirm the
  Phase 4/5 (`diag_dryrun` `2167ed9e…`, `diag_readonly` `ff73ca37…`,
  `diag_kombi_probe_dryrun` `677c2de1…`, `diag_kombi_probe` `a30dded2…`) and
  `p3b_sniffer` (`4c684a9e…`) HEX hashes are unchanged.

## 6. Offline test results [FW]

```
./scripts/pio run -d firmware/mega-can -e diag_ack           # builds
python3 tests/check_diag_ack_firmware.py                     # static audit: PASS
python3 tests/test_diag_ack.py                               # behaviour: PASS
```

`check_diag_ack_firmware.py`: no transmit/RTS/load-TX path linked; NORMAL mode
present (so it genuinely ACKs — the guarantee is not vacuous); RTS-pin/TXREQ
guards present. `test_diag_ack.py` (real core vs simulated controller): the four
bus conditions are each classified correctly; boot is IDLE/SILENT with no
auto-NORMAL; ARM+CONFIRM required and a wrong code refused; `!`/ESTOP latches and
blocks ARM; STOP returns to silent; an SPI fault forces fallback; every run ends
silent and demands a fresh confirmation.

These are software/offline results against a *simulated* controller. They verify
the decision logic and the no-TX guarantee; they do **not** measure real CAN
timing, real ACK electrical behaviour, or the R18 bus.

## 7. Exact flashing, monitoring and operator commands

Preconditions: clear the electrical blockers in docs/vehicle-readiness-r18.md
(connector/pinout B1, termination B2, single USB ground path B3, CS pull-ups B4).
Bike stationary, **ignition ON, engine OFF**; never crank/ride while connected.

```sh
# 1. Identify the Mega (VID:PID 2341:0042) and note its port.
./scripts/pio device list

# 2. Flash the ACK-only firmware (replace USB_PORT).
./scripts/pio run -d firmware/mega-can -e diag_ack -t upload --upload-port USB_PORT

# 3. Open the serial monitor at 115200 (this resets the Mega; it boots SILENT).
./scripts/pio device monitor -p USB_PORT -b 115200
```

Operator serial commands (type into the monitor):

```
STATUS      # show state/mode/counters; expect state=IDLE mode=SILENT
            # (do a passive look first: STATUS a few times, confirm frames are
            #  being seen passively and nothing is latched)
ARM         # prints: ARMED code=XXXX ...  (ACTIVE participation warning)
CONFIRM XXXX   # echo the printed code within 30 s -> runs ONE bring-up:
               #   passive window (silent) then ACK-only window (ACTIVE),
               #   then prints RESULT verdict=...
STOP        # at any time: force back to LISTEN_ONLY
!           # emergency stop: latched; requires power-cycle/reset to clear
```

Read the `RESULT verdict=` line (§4 table). After each run a fresh `ARM`/`CONFIRM`
is required. Keep the session short. If any `ALERT`/`FAULT`/bus-off appears,
disconnect and stop.

## 8. GO / NO-GO for a short, supervised ACK-only connector test

**GO — conditional**, for a short, supervised ACK-only test on the R18 with
ignition ON / engine OFF, **only after** the electrical blockers B1-B4 are
cleared and a passive `STATUS` shows the controller healthy and receiving. The
rationale:

- The firmware is proven (offline) to be ACK-only with no transmit path, to start
  silent, to require explicit confirmation, to bound the active window, and to
  fall back immediately on any error-counter/SPI/mode fault. The failure modes
  that matter on a vehicle (wrong bitrate, bus-off) are detected in milliseconds
  and we stop driving the bus.
- ACK-only is genuinely the *minimal* active step that can resolve the question
  the `0x130` capture left open, and it sends no application data.
- It is still **active participation**: this is a real, if minimal, transmission
  on the motorcycle's bus, so it is operator-gated, supervised, and not automatic.

**NO-GO conditions (do not proceed if any holds):** B1-B4 not cleared; the
passive `STATUS` shows SPI errors, mode faults, or nothing received at all
(investigate the tap first); engine running or intent to ride; or any inability
to guarantee the single-ground, ignition-ON/engine-OFF, short-session conditions.

**This is not a service reset and not a diagnostic request.** A `HEALTHY`/
`NO_ACK_WAS_MISSING_ACKER` verdict is the green light to proceed to the Phase 5
single `22 F1 50` probe (docs/diag-kombi-probe.md); any other verdict means fix
the tap/bitrate/connection first.
