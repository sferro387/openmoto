"""Offline tests for the Phase 7 read-only KOMBI service-inspection mode.

Nothing here opens a serial port or touches a motorcycle. It drives the REAL
firmware core (firmware/mega-can/src/diag/diag_core.h) compiled for the host
against a mock ECU, and verifies:

  * the four service DIDs (E119/E12B/E12C/E12D) are allowed ONLY in the
    service-read build (OPENMOTO_DIAG_SERVICE_READ=1) / host service_read flag,
    and DENIED by default -- on both firmware and host policy;
  * each read uses the verified addressing (6F1 / 660 / ext 60/F1) and the exact
    request frame 60 03 22 E1 xx 00 00 00;
  * multi-frame responses (E119 7B, E12B 10B, E12C 7B) are reassembled via a
    single bounded flow-control frame; the single-frame response (E12D 5B) is
    handled without flow control;
  * positive responses validate (status=OK) with the correct echoed DID, and the
    raw response bytes are surfaced verbatim;
  * writes/resets/session/security stay denied even in the service-read build;
  * only these four DIDs are added -- neighbouring service DIDs stay denied.
"""
import subprocess
import sys
import tempfile
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
SIM_SRC = ROOT / 'tests/diag_host/diag_sim.cpp'
sys.path.insert(0, str(ROOT / 'host/python'))

from openmoto_diag import protocol  # noqa: E402

SERVICE_DIDS = (0xE119, 0xE12B, 0xE12C, 0xE12D)


def expect(cond, msg):
    if not cond:
        raise AssertionError(msg)


def build_sim(tmp, service):
    out = Path(tmp) / ('svc_sim' if service else 'nosvc_sim')
    cmd = ['clang++', '-std=c++17', '-Wall', '-Wextra', '-Werror', '-DOPENMOTO_DIAG_ALLOW_TX=1']
    if service:
        cmd.append('-DOPENMOTO_DIAG_SERVICE_READ=1')
    cmd += [str(SIM_SRC), '-o', str(out)]
    subprocess.run(cmd, check=True)
    return out


def run(sim, scenario):
    return subprocess.check_output([str(sim), scenario], text=True, timeout=30)


def parse_result(text):
    for line in text.splitlines():
        d = protocol.parse_result_line(line)
        if d:
            return d
    return None


def test_host_policy():
    # Denied by default; allowed only with service_read=True. Still service 0x22.
    for did in SERVICE_DIDS:
        req = bytes([0x22, did >> 8, did & 0xFF])
        expect(not protocol.check_request(req).allowed,
               f'default policy should deny {did:04X}')
        a = protocol.check_request(req, service_read=True)
        expect(a.allowed and a.kind == 'ReadDataByIdentifier',
               f'service_read should allow {did:04X}: {a}')
    # Writes/etc. stay denied even with the flag.
    for req in (b'\x2E\xE1\x2C\x00', b'\x2E\xE1\x2B\x00', b'\x11\x01', b'\x27\x01',
                b'\x31\x01\x0F\x06', b'\x10\x03', b'\x14\xFF\xFF\xFF'):
        r = protocol.check_request(req, service_read=True)
        expect(not r.allowed, f'service build wrongly allowed {req.hex()}: {r}')
    # Only these four DIDs are added, not neighbours.
    for did in (0xE118, 0xE11A, 0xE12A, 0xE12E, 0xE100):
        r = protocol.check_request(bytes([0x22, did >> 8, did & 0xFF]), service_read=True)
        expect(not r.allowed, f'service build wrongly allowed neighbour {did:04X}')
    # Named requests resolve to the right bytes.
    for did in SERVICE_DIDS:
        req = protocol.build_request_bytes(f'service_{did:04X}')
        expect(req == bytes([0x22, did >> 8, did & 0xFF]), f'named service_{did:04X} wrong: {req.hex()}')
    print('  host policy: 4 service DIDs allowed only with flag; writes/neighbours denied; names OK')


def test_reads(sim):
    # Expected UDS response data-byte counts from docs/diag-kombi-findings.md:
    #   E119 -> 4 (7B total), E12B -> 7 (10B), E12C -> 4 (7B), E12D -> 2 (5B)
    expected_data = {0xE119: 4, 0xE12B: 7, 0xE12C: 4, 0xE12D: 2}
    multiframe = {0xE119, 0xE12B, 0xE12C}  # >6 UDS bytes -> first frame + FC + CF
    for did in SERVICE_DIDS:
        text = run(sim, f'service_{did:04X}')
        r = parse_result(text)
        expect(r is not None, f'{did:04X}: no RESULT')
        expect(r['status'] == 'OK', f'{did:04X}: status={r["status"]}')
        expect(r['silent'] == '1', f'{did:04X}: not silent at end')
        resp = r['resp']
        prefix = f'62E1{did & 0xFF:02X}'
        expect(resp.startswith(prefix), f'{did:04X}: resp {resp} should echo {prefix}')
        data_bytes = (len(resp) // 2) - 3  # minus 62 + 2 DID bytes
        expect(data_bytes == expected_data[did],
               f'{did:04X}: {data_bytes} data bytes, expected {expected_data[did]}')
        if did in multiframe:
            expect(r['frames_tx'] == '2', f'{did:04X}: expected FC frame (frames_tx=2), got {r["frames_tx"]}')
            expect(int(r['frames_rx']) >= 2, f'{did:04X}: expected multi-frame rx')
        else:
            expect(r['frames_tx'] == '1', f'{did:04X}: single-frame should need no FC')
    print('  reads: E119/E12B/E12C multi-frame (FC) + E12D single-frame, all OK, DIDs echoed')


def test_tx_frame_encoding(sim):
    # The transmitted request frame must be the verified extended-addressing form.
    text = run(sim, 'service_E12C')
    armed = next((l for l in text.splitlines() if l.startswith('ARMED ')), '')
    a = protocol.parse_armed_line(armed)
    expect(a and a['tx_id'] == 0x6F1, f'tx_id not 0x6F1: {armed}')
    expect(a['data'] == '600322E12C000000', f'request frame wrong: {a["data"]}')
    print('  tx frame: 6F1 / 60 03 22 E1 2C 00 00 00 (verified extended addressing)')


def test_denied_without_flag(nosvc):
    text = run(nosvc, 'service_denied')
    expect('policy-deny DID-not-allowlisted' in text,
           f'service DID should be denied without the flag:\n{text}')
    expect('ARMED ' not in text, 'must not ARM a service DID without the flag')
    print('  default build: service DIDs denied (DID-not-allowlisted), never armed')


def main():
    test_host_policy()
    with tempfile.TemporaryDirectory(prefix='openmoto-svc-') as tmp:
        sim = build_sim(tmp, service=True)
        nosvc = build_sim(tmp, service=False)
        test_reads(sim)
        test_tx_frame_encoding(sim)
        test_denied_without_flag(nosvc)
    print('DIAG SERVICE-READ TESTS PASS (offline; real firmware core vs mock ECU; '
          'not a hardware or vehicle result)')


if __name__ == '__main__':
    main()
