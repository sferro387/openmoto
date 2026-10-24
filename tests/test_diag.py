"""Offline tests for the Phase 4 read-only diagnostic client.

Everything here runs against the host simulator (tests/diag_host/diag_sim),
which links the REAL firmware core (firmware/mega-can/src/diag/diag_core.h)
against an in-process mock ECU. No hardware and no vehicle are involved.

Covered:
  * The host allowlist (openmoto_diag.protocol) agrees with the firmware policy
    on a matrix of allowed and forbidden requests.
  * Every request/response flow: positive single-frame, positive multi-frame
    (flow control), negative (NRC), responsePending->positive, and the error
    paths NO_RESPONSE, CF_TIMEOUT, BAD_SEQUENCE, INVALID_RESPONSE,
    TOO_MANY_PENDING, MODE_FAIL.
  * The safety machinery: ARM/CONFIRM gating, wrong-code disarm, policy denial,
    not-configured guard, emergency stop (! and ESTOP), consecutive-failure
    lockout, and boot silence-failure latch.
  * The DRYRUN build performs no transmission.
  * The addressing-profile loader refuses unverified/incomplete profiles.
  * The CLI runs a full session against the simulator and honours its guards.
"""
import subprocess
import sys
import tempfile
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
SIM_SRC = ROOT / 'tests/diag_host/diag_sim.cpp'
CORE = ROOT / 'firmware/mega-can/src/diag/diag_core.h'
sys.path.insert(0, str(ROOT / 'host/python'))

from openmoto_diag import protocol, profile  # noqa: E402


def build_sim(tmp, tx_enabled=True):
    out = Path(tmp) / ('diag_sim_tx' if tx_enabled else 'diag_sim_dry')
    subprocess.run(
        ['clang++', '-std=c++17', '-Wall', '-Wextra', '-Werror',
         f'-DOPENMOTO_DIAG_ALLOW_TX={1 if tx_enabled else 0}',
         str(SIM_SRC), '-o', str(out)],
        check=True)
    return out


def run(sim, scenario):
    return subprocess.check_output([str(sim), scenario], text=True, timeout=30)


def expect(cond, msg):
    if not cond:
        raise AssertionError(msg)


def result_status(text):
    for line in text.splitlines():
        d = protocol.parse_result_line(line)
        if d:
            return d
    return None


def test_policy_agreement():
    # (request bytes, expected allowed)
    cases = [
        (b'\x3E\x00', True), (b'\x3E\x01', False), (b'\x3E', False),
        (b'\x22\xF1\x90', True), (b'\x22\xF1\xA0', False), (b'\x22\xF1', False),
        (b'\x19\x01\xFF', True), (b'\x19\x02\xFF', True), (b'\x19\x03\xFF', False),
        (b'\x2E\xF1\x90\x00', False),  # write DID
        (b'\x11\x01', False),          # ECU reset
        (b'\x27\x01', False),          # security access
        (b'\x31\x01\x02\x03', False),  # routine control
        (b'\x14\xFF\xFF\xFF', False),  # clear DTC
        (b'\x2F\xF1\x90\x03', False),  # IO control
        (b'\x34\x00', False),          # request download
        (b'\x10\x03', False),          # session control
        (b'', False),
    ]
    for req, allowed in cases:
        got = protocol.check_request(req).allowed
        expect(got == allowed, f'policy mismatch for {req.hex()}: host says {got}, expected {allowed}')
    print(f'  policy: {len(cases)} host-policy cases OK')


def test_flows(sim):
    expected = {
        'read_did': 'OK', 'tester_present': 'OK', 'dtc_count': 'OK', 'dtc_list': 'OK',
        'read_vin_multiframe': 'OK', 'pending_ok': 'OK',
        'negative': 'NEGATIVE', 'no_response': 'NO_RESPONSE',
        'pending_forever': 'TOO_MANY_PENDING', 'bad_sid': 'INVALID_RESPONSE',
        'bad_echo': 'INVALID_RESPONSE', 'cf_timeout': 'CF_TIMEOUT',
        'bad_sequence': 'BAD_SEQUENCE', 'mode_fail': 'MODE_FAIL',
    }
    for scenario, status in expected.items():
        text = run(sim, scenario)
        d = result_status(text)
        expect(d is not None, f'{scenario}: no RESULT line')
        expect(d['status'] == status, f'{scenario}: status={d["status"]!r}, expected {status!r}')
        expect(d['silent'] == '1', f'{scenario}: not silent at end')
    # multi-frame actually reassembled a 20-byte response and used flow control
    text = run(sim, 'read_vin_multiframe')
    d = result_status(text)
    expect(d['frames_tx'] == '2' and int(d['frames_rx']) >= 3,
           f'multiframe did not use flow control: {d}')
    print(f'  flows: {len(expected)} request/response flows OK (+ multiframe FC)')


def test_safety(sim):
    # wrong CONFIRM disarms
    text = run(sim, 'wrong_confirm')
    expect('DISARMED reason=bad-code' in text, 'wrong confirm did not disarm')
    expect(result_status(text) is None, 'wrong confirm should not run a transaction')

    # policy denial at ARM time
    for scen, needle in [('deny_write', 'policy-deny'), ('deny_security', 'policy-deny')]:
        text = run(sim, scen)
        expect(needle in text, f'{scen}: expected {needle}')
        expect(result_status(text) is None, f'{scen}: must not transmit')

    # not configured
    text = run(sim, 'require_addr')
    expect('not-configured' in text, 'require_addr: missing not-configured guard')

    # emergency stop (bang) latches and blocks further ARM
    text = run(sim, 'estop_bang')
    expect('EVT estop reason=bang latched=1' in text, 'estop_bang: no estop event')
    expect('estop=1' in text, 'estop_bang: STATUS should show estop=1')
    expect('estop-latched' in text, 'estop_bang: ARM after estop should be refused')

    # ESTOP command
    text = run(sim, 'estop_command')
    expect('EVT estop reason=command' in text and 'estop-latched' in text, 'estop_command failed')

    # lockout after consecutive failures
    text = run(sim, 'lockout')
    expect('ALERT lockout' in text, 'lockout: no lockout alert')
    expect('lockout=1' in text, 'lockout: STATUS should show lockout=1')
    expect('lockout reset-required' in text, 'lockout: ARM after lockout should be refused')

    # boot silence failure latches the client off
    text = run(sim, 'silence_fail_boot')
    expect('silent-mode-unverified at boot' in text, 'silence_fail_boot: no boot alert')
    expect('estop=1' in text, 'silence_fail_boot: should latch estop')
    print('  safety: confirm gating, policy deny, not-configured, estop, lockout, boot-latch OK')


def test_dryrun(dry_sim):
    text = run(dry_sim, 'read_did')
    d = result_status(text)
    expect(d['status'] == 'DRYRUN', f'dryrun: status={d["status"]!r}')
    expect(d['frames_tx'] == '0', 'dryrun: transmitted frames in a no-TX build')
    expect('BUILD tx=DRYRUN' in text, 'dryrun: banner should say DRYRUN')
    print('  dryrun: no transmission in the no-TX build OK')


def test_profiles():
    prof_dir = ROOT / 'host/python/openmoto_diag/profiles'
    # Template must be refused (unverified).
    try:
        profile.load_profile(prof_dir / 'r18-UNVERIFIED.template.json')
        raise AssertionError('unverified template was accepted')
    except profile.ProfileError:
        pass
    # Simulator profile must load and produce a sane ADDR command.
    p = profile.load_profile(prof_dir / 'simulator.json')
    expect(p.addr_command('mock_ecu') == 'ADDR 6F1 612', f'bad ADDR cmd: {p.addr_command("mock_ecu")}')
    expect(p.pad_command() == 'PAD 00', f'bad PAD cmd: {p.pad_command()}')
    # A verified-but-broken profile (tx==rx) must be refused.
    import json
    bad = json.loads((prof_dir / 'simulator.json').read_text())
    bad['targets']['mock_ecu']['rx_id'] = bad['targets']['mock_ecu']['tx_id']
    with tempfile.NamedTemporaryFile('w', suffix='.json', delete=False) as fh:
        json.dump(bad, fh)
        bad_path = fh.name
    try:
        profile.load_profile(bad_path)
        raise AssertionError('tx==rx profile was accepted')
    except profile.ProfileError:
        pass
    finally:
        Path(bad_path).unlink()
    # Missing-evidence target must be refused.
    bad2 = json.loads((prof_dir / 'simulator.json').read_text())
    bad2['targets']['mock_ecu']['evidence'] = ''
    with tempfile.NamedTemporaryFile('w', suffix='.json', delete=False) as fh:
        json.dump(bad2, fh)
        bad2_path = fh.name
    try:
        profile.load_profile(bad2_path)
        raise AssertionError('no-evidence profile was accepted')
    except profile.ProfileError:
        pass
    finally:
        Path(bad2_path).unlink()
    print('  profiles: unverified/tx==rx/no-evidence refused; simulator profile OK')


def test_cli(tmp_sim):
    cli = ROOT / 'host/python/diag_cli.py'
    prof = ROOT / 'host/python/openmoto_diag/profiles/simulator.json'
    # Full session: two reads auto-confirmed.
    out = subprocess.check_output(
        [sys.executable, str(cli), '--sim', str(tmp_sim), '--profile', str(prof),
         '--target', 'mock_ecu', '--request', 'read_did_F190', '--request', 'dtc_list', '--yes'],
        text=True, timeout=30)
    expect(out.count('status=OK') == 2, f'CLI: expected 2 OK results\n{out}')

    # Forbidden request rejected, nothing sent.
    r = subprocess.run(
        [sys.executable, str(cli), '--sim', str(tmp_sim), '--profile', str(prof),
         '--target', 'mock_ecu', '--request', '1101', '--yes'],
        capture_output=True, text=True, timeout=30)
    expect(r.returncode == 2, 'CLI: forbidden request should exit 2')
    expect('service-forbidden' in r.stderr, 'CLI: missing forbidden message')
    expect('ARM' not in r.stdout, 'CLI: forbidden request must not reach ARM')

    # --port refuses without the prereq flag (no device is opened).
    r = subprocess.run(
        [sys.executable, str(cli), '--port', '/dev/null', '--profile', str(prof),
         '--target', 'mock_ecu', '--request', 'tester_present'],
        capture_output=True, text=True, timeout=30)
    expect(r.returncode == 4, 'CLI: --port without prereqs should exit 4')
    expect('REFUSING real transmission' in r.stderr, 'CLI: missing real-tx refusal')

    # Unverified profile refused.
    r = subprocess.run(
        [sys.executable, str(cli), '--sim', str(tmp_sim), '--profile',
         str(ROOT / 'host/python/openmoto_diag/profiles/r18-UNVERIFIED.template.json'),
         '--target', 'mock_ecu', '--request', 'tester_present', '--yes'],
        capture_output=True, text=True, timeout=30)
    expect(r.returncode == 2 and 'not verified' in r.stderr, 'CLI: unverified profile not refused')
    print('  cli: session, forbidden-reject, --port guard, unverified-profile guard OK')


def main():
    with tempfile.TemporaryDirectory(prefix='openmoto-diag-') as tmp:
        sim = build_sim(tmp, tx_enabled=True)
        dry = build_sim(tmp, tx_enabled=False)
        test_policy_agreement()
        test_flows(sim)
        test_safety(sim)
        test_dryrun(dry)
        test_profiles()
        test_cli(sim)
    print('DIAG HOST TESTS PASS (offline simulator + mock ECU; not a hardware or vehicle result)')


if __name__ == '__main__':
    main()
