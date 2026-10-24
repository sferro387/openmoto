"""Offline tests for the Phase 9 service-WRITE core (ISO-TP multi-frame TX +
read-back verify). Drives the REAL write core (write_core.h) against a
write-capable mock KOMBI. Nothing is transmitted to hardware.

Covers:
  * single-frame write (E12D) and multi-frame writes (E12C, E12B) succeed and
    are read-back-verified (status=OK, verified=YES);
  * negative response (NRC) is surfaced, not treated as success;
  * responsePending (0x78) then positive works;
  * READBACK_MISMATCH is detected and alerts to use the rollback reference;
  * missing flow-control -> FC_TIMEOUT; silent ECU -> NO_RESPONSE;
  * only 2E E1 2B/2C/2D (exact length) can be armed -- any other 0x2E/0x2F is denied;
  * emergency stop latches and blocks further writes;
  * the DRYRUN build (no transmit code) reports DRYRUN and transmits nothing.
"""
import subprocess
import sys
import tempfile
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
SIM = ROOT / 'tests/diag_write_host/write_sim.cpp'


def expect(c, m):
    if not c:
        raise AssertionError(m)


def build(tmp, tx):
    out = Path(tmp) / ('wsim_tx' if tx else 'wsim_dry')
    cmd = ['clang++', '-std=c++17', '-Wall', '-Wextra', '-Werror',
           f'-DOPENMOTO_WRITE_ALLOW_TX={1 if tx else 0}', str(SIM), '-o', str(out)]
    subprocess.run(cmd, check=True)
    return out


def run(sim, sc):
    return subprocess.check_output([str(sim), sc], text=True, timeout=30)


def result(text):
    for ln in text.splitlines():
        if ln.startswith('RESULT '):
            return dict(t.split('=', 1) for t in ln[len('RESULT '):].split() if '=' in t)
    return None


def test_writes_verified(sim):
    for sc, payload, rb in [('write_e12d', '2EE12D2710', '62E12D2710'),
                            ('write_e12c', '2EE12C180A07EB', '62E12C180A07EB'),
                            ('write_e12b', '2EE12B00330A180A07EA', '62E12B00330A180A07EA')]:
        r = result(run(sim, sc))
        expect(r and r['status'] == 'OK', f'{sc}: status {r}')
        expect(r['verified'] == 'YES', f'{sc}: not verified')
        expect(r['readback'] == rb, f'{sc}: readback {r["readback"]} != {rb}')
        expect(r['silent'] == '1', f'{sc}: not silent at end')
    print('  writes: E12D single-frame + E12C/E12B multi-frame, all read-back verified')


def test_error_paths(sim):
    expect(result(run(sim, 'nrc'))['status'] == 'NEGATIVE', 'nrc not NEGATIVE')
    expect(result(run(sim, 'nrc'))['nrc'] == '33', 'nrc code wrong')
    expect(result(run(sim, 'pending_ok'))['status'] == 'OK', 'pending_ok not OK')
    rm = result(run(sim, 'readback_wrong'))
    expect(rm['status'] == 'READBACK_MISMATCH' and rm['verified'] == 'NO', 'readback mismatch not caught')
    expect('READBACK_MISMATCH' and 'ALERT readback-mismatch' in run(sim, 'readback_wrong'), 'no rollback alert')
    expect(result(run(sim, 'no_fc'))['status'] == 'FC_TIMEOUT', 'missing FC not timed out')
    expect(result(run(sim, 'silent'))['status'] == 'NO_RESPONSE', 'silent ECU not NO_RESPONSE')
    print('  errors: NEGATIVE/pending-OK/READBACK_MISMATCH+alert/FC_TIMEOUT/NO_RESPONSE all handled')


def test_policy_and_estop(sim):
    expect('not-an-allowed-service-write' in run(sim, 'deny_bad'), 'wrong DID not denied')
    expect('not-an-allowed-service-write' in run(sim, 'deny_write_other'), '2F not denied')
    for sc in ('deny_bad', 'deny_write_other'):
        expect('RESULT ' not in run(sim, sc), f'{sc} must not run a transaction')
    e = run(sim, 'estop')
    expect('EVT estop' in e and 'estop-latched' in e, 'estop did not latch/block')
    print('  policy/estop: only 2E E1 2B/2C/2D armable; estop latches and blocks ARM')


def test_dryrun(dry):
    r = result(run(dry, 'write_e12d'))
    expect(r['status'] == 'DRYRUN', f'dryrun status {r}')
    expect('BUILD tx=DRYRUN' in run(dry, 'write_e12d'), 'dryrun banner missing')
    print('  dryrun: no transmit code -> DRYRUN, nothing sent')


def test_read_command(sim):
    # The standalone READ returns each original value; verified=n/a (non-mutating).
    for sc, val in [('read_e12d', '62E12D143C'), ('read_e12c', '62E12C090907EA'),
                    ('read_e119', '62E11900001 92C'.replace(' ', '')), ('read_e12b', '62E12B00330A180A07EA')]:
        r = result(run(sim, sc))
        expect(r and r['status'] == 'OK', f'{sc}: status {r}')
        expect(r['value'] == val, f'{sc}: value {r.get("value")} != {val}')
        expect(r['silent'] == '1', f'{sc}: not silent')
    # READ cannot be turned into a write, and only the four DIDs are allowed.
    expect('read-not-allowed' in run(sim, 'read_cant_write'), 'READ must refuse a 2E request')
    expect('RESULT ' not in run(sim, 'read_cant_write'), 'READ 2E must not run anything')
    expect('read-not-allowed' in run(sim, 'read_denied'), 'READ must refuse non-service DID')
    print('  read: READ 22E119/2B/2C/2D return values; READ refuses 2E and other DIDs')


def main():
    with tempfile.TemporaryDirectory(prefix='openmoto-wr-') as tmp:
        sim = build(tmp, True)
        dry = build(tmp, False)
        test_writes_verified(sim)
        test_read_command(sim)
        test_error_paths(sim)
        test_policy_and_estop(sim)
        test_dryrun(dry)
    print('DIAG WRITE TESTS PASS (offline; real write core + multi-frame TX vs mock KOMBI; '
          'NOTHING transmitted to a vehicle)')


if __name__ == '__main__':
    main()
