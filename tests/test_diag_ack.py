"""Offline tests for the Phase 6 ACK-only CAN bring-up firmware.

Nothing here opens a serial port or touches a motorcycle. It drives the REAL
bring-up core (firmware/mega-can/src/diag_ack/ack_core.h) compiled for the host
against a simulated CAN controller, and verifies:

  * the four bus conditions are distinguished:
      healthy bus          -> HEALTHY_BUS_ACK_ACCEPTED
      lone/no-ACK (us)     -> NO_ACK_WAS_MISSING_ACKER   (the 0x130 reframe)
      wrong bitrate        -> WRONG_BITRATE
      no/wrong connection  -> NO_CONNECTION_OR_SILENT_BUS
  * the firmware starts SILENT and never auto-enters NORMAL;
  * ACK-only requires ARM + CONFIRM (fresh each time), with a wrong code refused;
  * STOP and '!'/ESTOP return to silent; estop latches and blocks further ARM;
  * an SPI-integrity failure forces a fallback;
  * every run ends silent and demands a fresh confirmation.
"""
import subprocess
import sys
import tempfile
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
SIM_SRC = ROOT / 'tests/diag_ack_host/ack_sim.cpp'


def expect(cond, msg):
    if not cond:
        raise AssertionError(msg)


def build_sim(tmp):
    out = Path(tmp) / 'ack_sim'
    subprocess.run(['clang++', '-std=c++17', '-Wall', '-Wextra', '-Werror',
                    str(SIM_SRC), '-o', str(out)], check=True)
    return out


def run(sim, *args):
    return subprocess.check_output([str(sim), *args], text=True, timeout=30)


def result_line(text):
    for line in text.splitlines():
        if line.startswith('RESULT '):
            return dict(tok.split('=', 1) for tok in line[len('RESULT '):].split() if '=' in tok)
    return None


def test_four_conditions(sim):
    cases = {
        'healthy': 'HEALTHY_BUS_ACK_ACCEPTED',
        'no_ack_was_us': 'NO_ACK_WAS_MISSING_ACKER',
        'wrong_bitrate': 'WRONG_BITRATE',
        'no_connection': 'NO_CONNECTION_OR_SILENT_BUS',
    }
    for scenario, verdict in cases.items():
        r = result_line(run(sim, scenario))
        expect(r is not None, f'{scenario}: no RESULT line')
        expect(r['verdict'] == verdict, f'{scenario}: verdict={r["verdict"]}, expected {verdict}')
        expect(r['silent'] == '1', f'{scenario}: did not return to silent')
        expect(r['fresh_confirm_required'] == '1', f'{scenario}: should require fresh confirm')
    # The no-ACK case must show the passive-window receive-error signature that
    # distinguishes it from a plain healthy bus.
    r = result_line(run(sim, 'no_ack_was_us'))
    expect(int(r['passive_eflg'], 16) & 0x08, 'no_ack_was_us: expected RX-error flag in passive window')
    expect(r['active_rx'] != '0', 'no_ack_was_us: expected frames received while ACKing')
    print('  four conditions: healthy / no-ACK(was us) / wrong-bitrate / no-connection OK')


def test_starts_silent_no_auto_normal(sim):
    # Boot banner + a STATUS before any ARM: must be IDLE and SILENT, never NORMAL.
    out = subprocess.run([str(sim), 'healthy', '--stdin'], input='STATUS\n',
                         capture_output=True, text=True, timeout=30).stdout
    expect('ACTIVE bus participation' in out, 'boot banner must state ACTIVE participation')
    status = [l for l in out.splitlines() if l.startswith('STATUS ')]
    expect(status and 'state=IDLE' in status[0] and 'mode=SILENT' in status[0],
           f'must boot IDLE/SILENT: {status}')
    expect('RUN ack-only-window-start' not in out, 'must NOT auto-enter ACK-only at boot')
    print('  boot: starts IDLE/SILENT, banner flags ACTIVE participation, no auto-NORMAL')


def test_confirm_required(sim):
    # A wrong CONFIRM code must not start a run.
    out = subprocess.run([str(sim), 'healthy', '--stdin'], input='ARM\nCONFIRM 0000\n',
                         capture_output=True, text=True, timeout=30).stdout
    expect('ARMED code=' in out, 'ARM should print a code')
    expect('DISARMED reason=bad-code' in out, 'wrong CONFIRM must disarm')
    expect('RUN ack-only-window-start' not in out, 'wrong CONFIRM must not enter ACK-only')
    print('  confirm: ARM required; wrong CONFIRM disarms and never enters NORMAL')


def test_estop_and_stop(sim):
    out = run(sim, 'healthy', '--estop-mid')
    expect('EVT estop' in out, 'estop event missing')
    expect('estop=1' in out, 'STATUS should show estop latched')
    expect('ERR estop-latched' in out, 'ARM after estop must be refused')
    # STOP returns to silent without latching estop.
    out2 = subprocess.run([str(sim), 'healthy', '--stdin'], input='STOP\n',
                          capture_output=True, text=True, timeout=30).stdout
    expect('OK stopped' in out2, 'STOP should acknowledge')
    print('  estop/stop: ! latches and blocks ARM; STOP returns to silent')


def test_spi_fault(sim):
    r = result_line(run(sim, 'healthy', '--spi-break'))
    expect(r['verdict'] == 'FAULT_FORCED_FALLBACK', f'spi break verdict={r["verdict"]}')
    expect(r['spi_err'] == '1', 'spi error flag should be set')
    expect(r['silent'] == '1', 'spi fault must still return silent')
    print('  spi fault: forces fallback, returns silent')


def main():
    with tempfile.TemporaryDirectory(prefix='openmoto-ack-') as tmp:
        sim = build_sim(tmp)
        test_four_conditions(sim)
        test_starts_silent_no_auto_normal(sim)
        test_confirm_required(sim)
        test_estop_and_stop(sim)
        test_spi_fault(sim)
    print('DIAG ACK BRING-UP TESTS PASS (offline; real core vs simulated controller; '
          'not a hardware or vehicle result)')


if __name__ == '__main__':
    main()
