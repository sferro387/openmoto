"""Offline round-trip tests for the KOMBI service-write encoder/decoder.

Reverse-engineered from MotoScan sl0.C (docs/diag-kombi-service-reset.md).
Nothing here transmits; it only builds and parses byte payloads. It also
cross-references the REAL ECU read-backs captured in Phase 7 against the
decoded field structure, and checks the preserved rollback reference matches.
"""
import sys
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
sys.path.insert(0, str(ROOT / 'host/python'))

from openmoto_diag import service_codec as sc  # noqa: E402

# Real Phase 7 read-backs (data portion of the 62 E1 xx responses).
REAL = {
    'E12B': bytes.fromhex('62E12B00330A180A07EA'),
    'E12C': bytes.fromhex('62E12C090907EA'),
    'E12D': bytes.fromhex('62E12D143C'),
}


def expect(cond, msg):
    if not cond:
        raise AssertionError(msg)


def test_encode_matches_known_bytes():
    # Encoding the decoded real values must reproduce the exact on-wire bytes
    # (with the write SID 0x2E instead of the read 0x62).
    clk = sc.decode_clock(REAL['E12B'])
    enc = sc.encode_clock(clk)
    expect(enc.hex().upper() == '2EE12B00330A180A07EA', f'clock enc {enc.hex()}')
    dt = sc.decode_date(REAL['E12C'])
    enc = sc.encode_date(dt)
    expect(enc.hex().upper() == '2EE12C090907EA', f'date enc {enc.hex()}')
    dist = sc.decode_distance(REAL['E12D'])
    enc = sc.encode_distance(dist)
    expect(enc.hex().upper() == '2EE12D143C', f'dist enc {enc.hex()}')
    print('  encode: decoded real values re-encode to the exact 2E payloads')


def test_roundtrip():
    # encode -> decode -> encode is stable across a range of values.
    for c in (sc.Clock(0, 0, 0, 1, 1, 2020), sc.Clock(23, 59, 59, 31, 12, 2099),
              sc.Clock(0, 51, 10, 24, 10, 2026)):
        expect(sc.decode_clock(sc.encode_clock(c)) == c, f'clock round-trip {c}')
    for d in (sc.ServiceDate(1, 1, 2020), sc.ServiceDate(9, 9, 2026), sc.ServiceDate(31, 12, 2099)):
        expect(sc.decode_date(sc.encode_date(d)) == d, f'date round-trip {d}')
    for v in (0, 1, 6000, 5180, 0x1770, 0xFFFF):
        expect(sc.decode_distance(sc.encode_distance(v)) == v, f'dist round-trip {v}')
    print('  round-trip: clock/date/distance encode<->decode stable over range')


def test_decoded_real_values():
    clk = sc.decode_clock(REAL['E12B'])
    expect((clk.hour, clk.minute, clk.second) == (0, 51, 10), f'clock time {clk}')
    expect((clk.day, clk.month, clk.year) == (24, 10, 2026), f'clock date {clk}')
    dt = sc.decode_date(REAL['E12C'])
    expect((dt.day, dt.month, dt.year) == (9, 9, 2026), f'service date {dt}')
    expect(sc.decode_distance(REAL['E12D']) == 5180, 'service distance 5180')
    # year consistency across clock and date confirms the field layout.
    expect(clk.year == dt.year == 2026, 'year must agree across E12B/E12C')
    print('  real values: clock 24.10.2026 00:51:10, date 09.09.2026, distance 5180 (self-consistent)')


def test_range_guards():
    for bad in (sc.Clock(24, 0, 0, 1, 1, 2026), sc.Clock(0, 60, 0, 1, 1, 2026),
                sc.Clock(0, 0, 0, 0, 1, 2026), sc.Clock(0, 0, 0, 1, 13, 2026)):
        try:
            sc.encode_clock(bad)
            raise AssertionError(f'clock {bad} should be rejected')
        except ValueError:
            pass
    try:
        sc.encode_distance(0x10000)
        raise AssertionError('distance > 16-bit should be rejected')
    except ValueError:
        pass
    print('  guards: out-of-range clock/date/distance rejected')


def test_rollback_reference():
    # The preserved raw baseline must still be byte-exact (task 8).
    ref = ROOT / 'tests/diag_service/rollback-reference.json'
    expect(ref.exists(), 'rollback reference file missing')
    import json
    d = json.loads(ref.read_text())
    expect(d['raw']['E12C'].replace(' ', '') == '62E12C090907EA', 'rollback E12C drifted')
    expect(d['raw']['E12D'].replace(' ', '') == '62E12D143C', 'rollback E12D drifted')
    expect(d['raw']['E12B'].replace(' ', '') == '62E12B00330A180A07EA', 'rollback E12B drifted')
    # And the rollback WRITE payloads re-encode from those raw reads.
    expect(d['restore_writes']['E12C'].replace(' ', '') == '2EE12C090907EA', 'rollback write E12C')
    expect(d['restore_writes']['E12D'].replace(' ', '') == '2EE12D143C', 'rollback write E12D')
    print('  rollback: preserved raw baseline byte-exact; restore payloads match')


def main():
    test_encode_matches_known_bytes()
    test_roundtrip()
    test_decoded_real_values()
    test_range_guards()
    test_rollback_reference()
    print('SERVICE CODEC TESTS PASS (offline; encoder/decoder round-trip + real-ECU '
          'cross-reference; NOTHING transmitted, writes remain disabled)')


if __name__ == '__main__':
    main()
