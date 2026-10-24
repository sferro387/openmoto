"""Offline tests for the diagnostic trace importers and replay engine.

Nothing here opens a serial port or touches a motorcycle. It verifies:
  * ELM327/STN log import (AT/ST setup + UDS request/response pairing);
  * CAN/ISO-TP log import with BMW extended addressing + multi-frame reassembly;
  * JSON trace round-trip;
  * replay of the verified READ-ONLY KOMBI trace (order + response validation);
  * the read-only guard: the KOMBI service-RESET trace is BLOCKED (never run),
    and only runs as an explicit, clearly-labelled dry run when asked;
  * that every request byte sequence we reconstructed matches the host policy's
    view of read vs. write (so a write can never slip through as a read).
"""
import sys
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
sys.path.insert(0, str(ROOT / 'host/python'))

from openmoto_diag import trace as tracemod          # noqa: E402
from openmoto_diag import replay as replaymod         # noqa: E402
from openmoto_diag import protocol                    # noqa: E402

TRACES = ROOT / 'host/python/openmoto_diag/traces'
FIXTURES = ROOT / 'tests/diag_traces'


def expect(cond, msg):
    if not cond:
        raise AssertionError(msg)


def test_elm_import():
    t = tracemod.parse_elm_log((FIXTURES / 'sample-elm-kombi.log').read_text(),
                               name='elm', evidence=tracemod.EV_INFERRED)
    at = [s for s in t.steps if s.kind == 'at']
    uds = [s for s in t.steps if s.kind == 'uds']
    expect(any(s.at_command == 'ATSPB' for s in at), 'ELM: missing ATSPB setup step')
    expect(any(s.at_command == 'ATFCSD60300008' for s in at), 'ELM: missing FC data step')
    expect(len(uds) == 3, f'ELM: expected 3 UDS steps, got {len(uds)}')
    probe = uds[0]
    expect(probe.request == bytes.fromhex('22F150'), f'ELM: probe req {probe.request.hex()}')
    expect(probe.expected == bytes.fromhex('62F15001 0203'.replace(' ', '')),
           f'ELM: probe resp {probe.expected.hex() if probe.expected else None}')
    expect(uds[1].expected == bytes.fromhex('62E1190001E240'), 'ELM: odometer resp mismatch')
    print(f'  elm import: {len(at)} setup + {len(uds)} UDS steps OK')


def test_can_import():
    t = tracemod.parse_can_log((FIXTURES / 'sample-can-kombi.log').read_text(),
                               tester_id=0x6F1, target_id=0x60, tx_ext=0x60, rx_ext=0xF1,
                               name='can')
    uds = [s for s in t.steps if s.kind == 'uds']
    expect(len(uds) == 2, f'CAN: expected 2 reassembled messages, got {len(uds)}')
    expect(uds[0].request == bytes.fromhex('22F150'), f'CAN: req0 {uds[0].request.hex()}')
    expect(uds[0].expected == bytes.fromhex('62F150010203'), f'CAN: resp0 {uds[0].expected.hex()}')
    # multi-frame: FF declares 10 bytes, CF supplies the rest -> 62 E1 2B + 7 = 10 bytes
    expect(uds[1].request == bytes.fromhex('22E12B'), f'CAN: req1 {uds[1].request.hex()}')
    expect(uds[1].expected == bytes.fromhex('62E12B0C1E0A0E102D15'),
           f'CAN: multiframe reassembly wrong: {uds[1].expected.hex()}')
    print('  can import: extended addressing + multi-frame ISO-TP reassembly OK')


def test_json_roundtrip():
    t = tracemod.load_json_trace(TRACES / 'kombi-read-r18.json')
    dumped = tracemod.dump_json_trace(t)
    import json
    d = json.loads(dumped)
    expect(d['target_hex'] == 0x60, 'JSON: target_hex lost')
    # round-trip by re-parsing the dumped structure through a temp file
    import tempfile
    with tempfile.NamedTemporaryFile('w', suffix='.json', delete=False) as fh:
        fh.write(dumped)
        p = fh.name
    try:
        t3 = tracemod.load_json_trace(p)
    finally:
        Path(p).unlink()
    expect(len(t3.steps) == len(t.steps), 'JSON: step count changed on round-trip')
    expect(t3.steps[10].request == t.steps[10].request, 'JSON: request bytes changed on round-trip')
    print('  json round-trip: OK')


def test_replay_readonly():
    t = tracemod.load_json_trace(TRACES / 'kombi-read-r18.json')
    report = replaymod.replay(t)
    expect(report.ok, f'read replay should pass: {report.summary()}')
    expect(not report.blocked, 'read replay must not block anything')
    uds_results = [r for r in report.results if r.kind == 'uds']
    expect(all(r.status == 'OK' for r in uds_results),
           f'all UDS reads should validate OK: {[(r.index, r.status) for r in uds_results]}')
    at_results = [r for r in report.results if r.kind == 'at']
    expect(len(at_results) == 10, f'expected 10 AT setup steps, got {len(at_results)}')
    print(f'  replay read-only: {report.summary()}')


def test_replay_blocks_writes():
    t = tracemod.load_json_trace(TRACES / 'kombi-service-reset-r18.json')
    # every 2E... step must be classified as a write
    writes = t.writes()
    expect(len(writes) == 3, f'service-reset trace should have 3 writes, got {len(writes)}')
    for s in writes:
        expect(s.request[0] == 0x2E, f'unexpected write SID {s.request[:1].hex()}')
        # host policy must also deny these
        expect(not protocol.check_request(s.request).allowed,
               f'host policy wrongly allowed {s.request.hex()}')

    # default replay BLOCKS at the first write, nothing executed
    report = replaymod.replay(t)
    expect(not report.ok, 'service-reset replay must fail (blocked)')
    expect(report.blocked, 'service-reset replay must record a blocked step')
    statuses = [r.status for r in report.results]
    expect('BLOCKED' in statuses, 'service-reset replay must report BLOCKED')
    expect('WRITE_DRYRUN' not in statuses, 'default replay must not dry-run writes')

    # explicit dry-run mode logs intent but still executes nothing
    report2 = replaymod.replay(t, allow_write_dryrun=True)
    dry = [r for r in report2.results if r.status == 'WRITE_DRYRUN']
    expect(len(dry) == 3, f'dry-run should label 3 write steps, got {len(dry)}')
    expect(all('not transmitted' in r.note for r in dry), 'dry-run steps must say not transmitted')
    print(f'  replay write-guard: blocked by default; 3 writes dry-run-only when asked; '
          f'host policy denies all 3')


def test_read_requests_are_reads():
    # Every request in the read trace must be a non-mutating (allowed or benign
    # read) service, and NONE may be classified as a write.
    t = tracemod.load_json_trace(TRACES / 'kombi-read-r18.json')
    for s in t.steps:
        if s.kind != 'uds':
            continue
        expect(not s.is_write(), f'read trace contains a write: {s.request.hex()}')
        expect(s.request[0] in (0x22, 0x19, 0x3E),
               f'read trace has unexpected service {s.request[:1].hex()}')
    print('  read-trace classification: all steps are reads (22/19/3E)')


def main():
    test_elm_import()
    test_can_import()
    test_json_roundtrip()
    test_replay_readonly()
    test_replay_blocks_writes()
    test_read_requests_are_reads()
    print('DIAG REPLAY TESTS PASS (offline trace import + replay + read-only guard; '
          'not a hardware or vehicle result)')


if __name__ == '__main__':
    main()
