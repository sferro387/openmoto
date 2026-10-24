"""Offline tests for the Phase 5 KOMBI read-only probe (22 F1 50).

Nothing here opens a serial port or touches a motorcycle. It verifies, against
the REAL firmware core compiled for the host (tests/diag_host/diag_sim.cpp):

  * the exact serialized request frame for 22 F1 50 under BMW extended
    addressing is `60 03 22 F1 50 00 00 00` on CAN id 0x6F1;
  * the expected response id is 0x660 with address-extension byte 0xF1, and the
    firmware strips it and reports the UDS payload 62 F1 50 ...;
  * 22 F1 50 is DENIED by default and ALLOWED only in the KOMBI-probe build
    (OPENMOTO_DIAG_KOMBI_PROBE=1), on both firmware and host policy;
  * writes/resets/session/security stay denied even in the probe build;
  * the APK-derived R18 profile is refused (verified=false) while the simulator
    KOMBI profile loads and produces the right ADDR/PAD commands.
"""
import subprocess
import sys
import tempfile
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
SIM_SRC = ROOT / 'tests/diag_host/diag_sim.cpp'
sys.path.insert(0, str(ROOT / 'host/python'))

from openmoto_diag import protocol, profile  # noqa: E402

PROBE_REQ = bytes([0x22, 0xF1, 0x50])
# Expected on-wire request frame (BMW protocol-B extended addressing, PAD 00):
#   [target 0x60][ISO-TP SF PCI 0x03][22 F1 50][00 00 00 pad] on CAN id 0x6F1
EXPECTED_TX_DATA = '600322F150000000'
EXPECTED_TX_ID = '6F1'


def expect(cond, msg):
    if not cond:
        raise AssertionError(msg)


def build_sim(tmp, probe):
    out = Path(tmp) / ('sim_probe' if probe else 'sim_noprobe')
    cmd = ['clang++', '-std=c++17', '-Wall', '-Wextra', '-Werror',
           '-DOPENMOTO_DIAG_ALLOW_TX=1']
    if probe:
        cmd.append('-DOPENMOTO_DIAG_KOMBI_PROBE=1')
    cmd += [str(SIM_SRC), '-o', str(out)]
    subprocess.run(cmd, check=True)
    return out


def run(sim, scenario):
    return subprocess.check_output([str(sim), scenario], text=True, timeout=30)


def test_frame_encoding(sim_probe):
    text = run(sim_probe, 'kombi_probe')
    armed = None
    result = None
    traces = []
    for line in text.splitlines():
        if line.startswith('ARMED '):
            armed = protocol.parse_armed_line(line)
        elif line.startswith('RESULT '):
            result = protocol.parse_result_line(line)
        elif line.startswith('TRACE tx '):
            traces.append(line)
    expect(armed is not None, 'probe did not ARM (is the build missing KOMBI probe flag?)')
    expect(armed['tx_id'] == 0x6F1, f'probe tx_id = {armed["tx_id"]:#x}, expected 0x6F1')
    expect(armed['data'] == EXPECTED_TX_DATA,
           f'probe frame data = {armed["data"]}, expected {EXPECTED_TX_DATA}')
    expect(armed['dlc'] == 8, f'probe dlc = {armed["dlc"]}, expected 8')
    # The emitted TX trace frame must match the ARMED data exactly.
    expect(any(EXPECTED_TX_ID in t and EXPECTED_TX_DATA in t.replace(' ', '') for t in traces),
           f'TX trace frame does not match expected {EXPECTED_TX_ID}/{EXPECTED_TX_DATA}: {traces}')
    print(f'  frame encoding: tx id=0x6F1 data={EXPECTED_TX_DATA} dlc=8 OK')
    return result


def test_response_reassembly(sim_probe):
    text = run(sim_probe, 'kombi_probe')
    result = protocol.parse_result_line(
        next(l for l in text.splitlines() if l.startswith('RESULT ')))
    expect(result['status'] == 'OK', f'probe status = {result["status"]}, expected OK')
    # The mock replies on 0x660 with ext byte F1; firmware strips it and reports
    # the UDS positive response 62 F1 50 <data...>.
    expect(result['resp'].startswith('62F150'),
           f'probe response = {result.get("resp")}, expected to start 62F150')
    # The response frame id must be 0x660 with the F1 extension byte.
    rx = [l for l in text.splitlines() if l.startswith('TRACE rx ')]
    expect(rx and 'id=660' in rx[0], f'response id not 0x660: {rx}')
    expect('F1' == rx[0].split('data=')[1][:2], f'response ext byte not F1: {rx}')
    print(f'  response: id=0x660 ext=F1 stripped -> resp={result["resp"]} OK')


def test_denied_without_flag(sim_noprobe):
    text = run(sim_noprobe, 'kombi_probe_denied')
    expect('policy-deny DID-not-allowlisted' in text,
           f'probe should be DENIED without the flag; got:\n{text}')
    expect('ARMED ' not in text, 'probe must not ARM in a non-probe build')
    print('  default build: 22 F1 50 denied (DID-not-allowlisted) OK')


def test_host_policy_agreement():
    # Denied by default; allowed only with kombi_probe=True. Still service 0x22.
    d = protocol.check_request(PROBE_REQ)
    expect(not d.allowed and d.detail == 'DID-not-allowlisted',
           f'default host policy should deny probe: {d}')
    a = protocol.check_request(PROBE_REQ, kombi_probe=True)
    expect(a.allowed and a.kind == 'ReadDataByIdentifier',
           f'kombi_probe host policy should allow probe: {a}')
    # Writes/resets/etc. stay denied even with the probe flag on.
    for req in (b'\x2E\xF1\x50\x00', b'\x2E\xE1\x2B\x00', b'\x11\x01', b'\x27\x01',
                b'\x31\x01\x0F\x06', b'\x10\x03', b'\x14\xFF\xFF\xFF'):
        r = protocol.check_request(req, kombi_probe=True)
        expect(not r.allowed, f'probe build wrongly allowed {req.hex()}: {r}')
    # Only 0xF150 is added, not neighbouring DIDs.
    for did in (0xF14F, 0xF151, 0xF100, 0xF1A0):
        r = protocol.check_request(bytes([0x22, did >> 8, did & 0xFF]), kombi_probe=True)
        expect(not r.allowed, f'probe build wrongly allowed DID {did:04X}')
    print('  host policy: probe denied by default, allowed with flag; writes stay denied')


def test_named_request():
    req = protocol.build_request_bytes('kombi_probe')
    expect(req == PROBE_REQ, f'kombi_probe named request = {req.hex()}, expected 22F150')
    print('  named request: kombi_probe -> 22 F1 50 OK')


def test_profiles():
    pdir = ROOT / 'host/python/openmoto_diag/profiles'
    # APK-derived R18 profile must be REFUSED (verified=false) -> no transmit.
    try:
        profile.load_profile(pdir / 'r18-kombi-APK.json')
        raise AssertionError('APK-derived R18 KOMBI profile was accepted (should be unverified)')
    except profile.ProfileError:
        pass
    # Simulator KOMBI profile loads and yields the right ADDR/PAD commands.
    p = profile.load_profile(pdir / 'kombi-sim.json')
    expect(p.addressing_mode == 'extended', 'sim profile should be extended addressing')
    expect(p.addr_command('kombi') == 'ADDR 6F1 660 60 F1',
           f'sim ADDR command wrong: {p.addr_command("kombi")}')
    expect(p.pad_command() == 'PAD 00', f'sim PAD command wrong: {p.pad_command()}')
    expect(p.bitrate_kbps == 500, 'sim bitrate should be 500')
    print('  profiles: APK R18 refused (unverified); sim KOMBI -> ADDR 6F1 660 60 F1 / PAD 00')


def test_cli_guards(sim_probe):
    cli = ROOT / 'host/python/diag_cli.py'
    sim_profile = ROOT / 'host/python/openmoto_diag/profiles/kombi-sim.json'
    apk_profile = ROOT / 'host/python/openmoto_diag/profiles/r18-kombi-APK.json'

    # Happy path: --kombi-probe + verified sim profile -> status=OK, exact frame.
    out = subprocess.check_output(
        [sys.executable, str(cli), '--sim', str(sim_probe), '--profile', str(sim_profile),
         '--target', 'kombi', '--request', 'kombi_probe', '--kombi-probe', '--yes'],
        text=True, timeout=30)
    expect('status=OK' in out, f'CLI probe should succeed:\n{out}')
    expect('data ' + EXPECTED_TX_DATA.lower() in out or EXPECTED_TX_DATA in out,
           f'CLI probe frame not shown as {EXPECTED_TX_DATA}:\n{out}')

    # Without --kombi-probe: host refuses (nothing armed).
    r = subprocess.run(
        [sys.executable, str(cli), '--sim', str(sim_probe), '--profile', str(sim_profile),
         '--target', 'kombi', '--request', 'kombi_probe', '--yes'],
        capture_output=True, text=True, timeout=30)
    expect(r.returncode == 2 and 'DID-not-allowlisted' in r.stderr,
           f'CLI should refuse probe without --kombi-probe: rc={r.returncode} err={r.stderr}')

    # With the APK-derived (unverified) R18 profile: profile loader refuses.
    r = subprocess.run(
        [sys.executable, str(cli), '--sim', str(sim_probe), '--profile', str(apk_profile),
         '--target', 'kombi', '--request', 'kombi_probe', '--kombi-probe', '--yes'],
        capture_output=True, text=True, timeout=30)
    expect(r.returncode == 2 and 'not verified' in r.stderr,
           f'CLI should refuse the unverified APK profile: rc={r.returncode} err={r.stderr}')
    print('  cli: probe OK with flag+verified profile; refused without flag; '
          'refused with unverified APK profile')


def test_noack_clean_fail(sim_probe):
    # On a bus with no acknowledging node (the 0x130 failure mode), the one-shot
    # transmit must fail cleanly as TX_FAILED with exactly one TX attempt, not a
    # retransmission storm, and the client must return to silent.
    text = run(sim_probe, 'kombi_probe_noack')
    result = protocol.parse_result_line(
        next(l for l in text.splitlines() if l.startswith('RESULT ')))
    expect(result['status'] == 'TX_FAILED', f'no-ACK probe status={result["status"]}, expected TX_FAILED')
    expect(result['frames_tx'] == '1', f'no-ACK probe sent {result["frames_tx"]} frames, expected exactly 1')
    expect(result['silent'] == '1', 'no-ACK probe did not return to silent')
    print('  no-ACK bus: one-shot TX_FAILED, exactly 1 frame, returns silent (no bus-off storm)')


def main():
    test_host_policy_agreement()
    test_named_request()
    test_profiles()
    with tempfile.TemporaryDirectory(prefix='openmoto-kombi-') as tmp:
        sim_probe = build_sim(tmp, probe=True)
        sim_noprobe = build_sim(tmp, probe=False)
        test_frame_encoding(sim_probe)
        test_response_reassembly(sim_probe)
        test_noack_clean_fail(sim_probe)
        test_denied_without_flag(sim_noprobe)
        test_cli_guards(sim_probe)
    print('DIAG KOMBI PROBE TESTS PASS (offline; real firmware core vs mock ECU; '
          'not a hardware or vehicle result)')


if __name__ == '__main__':
    main()
