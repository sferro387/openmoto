"""Host-side tests for the Phase 3B binary protocol: framing, decoding,
corruption, resynchronization, sequence/timestamp/counter wrap handling and
stream accounting. Golden vectors come from the firmware's own protocol.h
(compiled with clang++), checked against an independent Python encoder.

Run: python3 tests/test_phase3b_protocol.py   (stdlib only; needs clang++)
These are host-only checks, not hardware results.
"""
from pathlib import Path
import importlib.util
import random
import struct
import subprocess
import sys
import tempfile
import unittest

ROOT = Path(__file__).resolve().parents[1]
sys.path.insert(0, str(ROOT / 'host/python'))
import phase3b_protocol as p  # noqa: E402

_spec = importlib.util.spec_from_file_location('capture_phase3b', ROOT / 'host/python/capture_phase3b.py')
cap = importlib.util.module_from_spec(_spec)
_spec.loader.exec_module(cap)


# ---- Independent reference encoder (Python) -------------------------------
def record(rtype, seq, body):
    head = bytes([p.VERSION, rtype, seq & 0xFF, (seq >> 8) & 0xFF, len(body)]) + body
    crc = p.crc16_bitwise(head)  # independent of the decoder table
    return p.SYNC + head + bytes([crc >> 8, crc & 0xFF])


def frame(seq, ts, can_id, ext=False, rtr=False, dlc=8, data=b''):
    idf = can_id | (p.ID_EXT if ext else 0) | (p.ID_RTR if rtr else 0)
    n = 0 if rtr else min(dlc, 8)
    return record(p.T_FRAME, seq, struct.pack('<IIB', ts, idf, dlc) + data[:n])


def text(seq, s, cont=False):
    return record(p.T_TEXT, seq, bytes([1 if cont else 0]) + s.encode())


def drop(seq, n, total):
    return record(p.T_DROP, seq, struct.pack('<II', n, total))


D8 = bytes([0x4F, 0x4D, 0x5B, 0xA5, 0x5A, 0x00, 0xFF, 0x5A])


def golden():
    with tempfile.TemporaryDirectory(prefix='openmoto-p3b-') as tmp:
        exe = str(Path(tmp) / 'golden')
        subprocess.run(['clang++', '-std=c++17', '-Wall', '-Wextra', '-Werror',
                        str(ROOT / 'tests/phase3b_host/golden.cpp'), '-o', exe], check=True)
        out = subprocess.check_output([exe], text=True)
    return dict(line.split(' ', 1) for line in out.strip().splitlines())


class GoldenVectors(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        cls.g = {k: v.strip() for k, v in golden().items()}

    def test_crc_check_value(self):
        self.assertEqual(p.crc16_ccitt_false(b"123456789"), 0x29B1)
        self.assertEqual(p.crc16_bitwise(b"123456789"), 0x29B1)
        for n in range(0, 300, 7):
            blob = bytes((k * 31 + n) & 0xFF for k in range(n))
            self.assertEqual(p.crc16_ccitt_false(blob), p.crc16_bitwise(blob))
        self.assertEqual(self.g['crc_check'], '29B1')

    def test_firmware_encoder_matches_reference(self):
        expect = {
            'std8': frame(0, 0x01020304, 0x123, dlc=8, data=D8),
            'ext3': frame(1, 0xFFFFFFFF, 0x1FFFFFFF, ext=True, dlc=3, data=D8),
            'std_rtr4': frame(2, 0, 0x321, rtr=True, dlc=4),
            'ext_rtr0': frame(3, 12345, 0x0ABCDEF, ext=True, rtr=True, dlc=0),
            'std0': frame(4, 7, 0x7FF, dlc=0),
            'dlc15': frame(5, 8, 0x000, dlc=15, data=D8),
            'seq_max': frame(65535, 9, 0x456, dlc=8, data=D8),
            'drop': drop(6, 17, 1000017),
            'stats': record(p.T_STATS, 7, p.STATS_STRUCT.pack(
                0x11111111, 0x22222222, 3, 4, 5, 6, 7, 8, 9, 10, 11, 12, 0x15, 0, 0, 5432)),
            'text': text(8, '# STREAM start dlc=8 ms=5000'),
            'text_cont': text(9, 'part one ', cont=True),
            'hello': record(p.T_HELLO, 10, p.HELLO_STRUCT.pack(1, 1, 4000000, 921600, 64, 42)),
        }
        for name, ref in expect.items():
            self.assertEqual(self.g[name], ref.hex().upper(), name)
        # The firmware's streaming-CRC emission must be byte-identical.
        self.assertEqual(self.g['std8_streaming'], self.g['std8'])
        self.assertEqual(self.g['stats_streaming'], self.g['stats'])

    def test_decode_golden_fields(self):
        # text_cont is a deliberately unterminated line (tested in Counters).
        s = p.analyze(bytes.fromhex(''.join(v for k, v in self.g.items()
                                            if k not in ('crc_check', 'seq_max', 'text_cont')
                                            and not k.endswith('_streaming'))))
        self.assertEqual(s.errors, [])
        f = s.records[0]
        self.assertEqual((f['id'], f['ext'], f['rtr'], f['dlc'], f['data'], f['ts_us']),
                         (0x123, False, False, 8, D8, 0x01020304))
        ext3 = s.records[1]
        self.assertEqual((ext3['id'], ext3['ext'], ext3['dlc'], ext3['data']), (0x1FFFFFFF, True, 3, D8[:3]))
        self.assertTrue(s.records[2]['rtr'] and s.records[2]['data'] == b'' and s.records[2]['dlc'] == 4)
        self.assertEqual(s.records[5]['dlc'], 15)
        self.assertEqual(len(s.records[5]['data']), 8)
        kinds = [r['type'] for r in s.records]
        self.assertEqual(kinds.count('DROP'), 1)
        st = next(r for r in s.records if r['type'] == 'STATS')
        self.assertEqual((st['rx'], st['eflg_or'], st['min_free_ram']), (3, 0x15, 5432))
        hello = next(r for r in s.records if r['type'] == 'HELLO')
        self.assertEqual((hello['spi_hz'], hello['baud'], hello['queue_size']), (4000000, 921600, 64))


def sample_stream(n=200, seed=1):
    rnd = random.Random(seed)
    out, seq = bytearray(), 0
    for k in range(n):
        if k % 37 == 5:
            out += text(seq, '# status line %d' % k)
        elif k % 53 == 7:
            out += drop(seq, 3, k)
        else:
            dlc = rnd.choice([0, 1, 3, 8, 8])
            ext = rnd.random() < 0.3
            out += frame(seq, k * 260, rnd.getrandbits(29 if ext else 11), ext=ext, dlc=dlc,
                         data=bytes(rnd.getrandbits(8) for _ in range(8)))
        seq = (seq + 1) & 0xFFFF
    return bytes(out)


class Framing(unittest.TestCase):
    def assert_accounting(self, s):
        self.assertEqual(s.bytes_total, s.record_bytes + sum(e.length for e in s.errors))

    def test_clean_stream_any_chunking(self):
        data = sample_stream()
        ref = p.analyze(data)
        self.assertEqual(ref.errors, [])
        self.assertEqual(len(ref.records), 200)
        for chunk in (1, 2, 7, 64, 1000):
            s = p.analyze(data, chunk=chunk)
            self.assertEqual([r['offset'] for r in s.records], [r['offset'] for r in ref.records])
            self.assertEqual(s.errors, [])
            self.assert_accounting(s)

    def test_corrupted_byte_resyncs_and_is_reported(self):
        data = bytearray(sample_stream())
        recs = p.analyze(bytes(data)).records
        target = recs[50]
        data[target['offset'] + 9] ^= 0x40          # inside body
        s = p.analyze(bytes(data), chunk=5)
        self.assertEqual(len(s.records), 199)       # exactly one record lost
        self.assertIn('crc', [e.kind for e in s.errors])
        self.assertEqual(s.seq_gaps, 1)             # and the gap is visible via seq
        self.assert_accounting(s)

    def test_every_single_byte_corruption_is_detected(self):
        base = sample_stream(30)
        n_ref = len(p.analyze(base).records)
        for pos in range(len(base)):
            for flip in (0x01, 0x80):
                data = bytearray(base)
                data[pos] ^= flip
                s = p.analyze(bytes(data))
                self.assert_accounting(s)
                # Either a record is lost (and an error reported) or nothing changed semantically.
                if len(s.records) != n_ref:
                    self.assertTrue(s.errors, f'silent loss at byte {pos}')
                self.assertTrue(s.errors, f'undetected corruption at byte {pos} flip {flip:#x}')

    def test_truncated_tail(self):
        data = sample_stream(20)
        s = p.analyze(data[:-5])
        self.assertEqual(s.errors[-1].kind, 'truncated')
        self.assertEqual(len(s.records), 19)
        self.assert_accounting(s)

    def test_truncated_record_followed_by_more_data(self):
        a, b = frame(0, 1, 0x100, dlc=8, data=D8), frame(1, 2, 0x101, dlc=8, data=D8)
        s = p.analyze(a[:12] + b)
        self.assertEqual(len(s.records), 1)
        self.assertEqual(s.records[0]['id'], 0x101)
        self.assertTrue(s.errors)
        self.assert_accounting(s)

    def test_garbage_with_fake_sync_between_records(self):
        g = b'\xA5\x5A\x01\x01\x00\x00\x09junkjunkj\xA5\x5A\xA5'   # false header, bad crc
        data = frame(0, 1, 0x100, data=D8) + g + frame(1, 2, 0x101, data=D8)
        s = p.analyze(data, chunk=3)
        self.assertEqual([r['id'] for r in s.records], [0x100, 0x101])
        self.assertTrue(all(e.kind in ('crc', 'garbage', 'bad_version') for e in s.errors))
        self.assertEqual(s.seq_gaps, 0)
        self.assert_accounting(s)

    def test_payload_containing_sync_bytes(self):
        data = b''.join(frame(i, i, 0x100, data=b'\xA5\x5A\xA5\x5A\xA5\x5A\xA5\x5A') for i in range(50))
        s = p.analyze(data, chunk=1)
        self.assertEqual(len(s.records), 50)
        self.assertEqual(s.errors, [])

    def test_bad_version_and_length(self):
        bad_ver = bytearray(frame(0, 1, 0x100, data=D8)); bad_ver[2] = 2
        bad_len = bytearray(frame(1, 1, 0x100, data=D8)); bad_len[6] = 200
        s = p.analyze(bytes(bad_ver) + bytes(bad_len) + frame(2, 3, 0x102, data=D8))
        kinds = {e.kind for e in s.errors}
        self.assertTrue({'bad_version', 'bad_length'} <= kinds)
        self.assertEqual([r['id'] for r in s.records], [0x102])
        self.assert_accounting(s)

    def test_invalid_record_content_is_reported(self):
        cases = [
            record(p.T_FRAME, 0, struct.pack('<IIB', 0, 0x800, 0)),              # std id > 0x7FF
            record(p.T_FRAME, 1, struct.pack('<IIB', 0, p.ID_RESERVED | 1, 0)),  # reserved bit
            record(p.T_FRAME, 2, struct.pack('<IIB', 0, 0x100, 8) + b'\x00'),   # length mismatch
            record(p.T_FRAME, 3, struct.pack('<IIB', 0, 0x100, 16)),             # dlc > 15
            record(p.T_DROP, 4, struct.pack('<II', 0, 5)),                       # zero drop
            record(p.T_TEXT, 5, b'\x00bad\x07'),                                 # non-printable
            record(0x7F, 6, b''),                                                # unknown type
        ]
        s = p.analyze(b''.join(cases))
        self.assertEqual(len(s.records), 0)
        self.assertEqual([e.kind for e in s.errors], ['invalid_record'] * len(cases))


class Counters(unittest.TestCase):
    def test_seq_wrap_gap_duplicate(self):
        data = (frame(65534, 0, 1, data=D8) + frame(65535, 1, 1, data=D8) + frame(0, 2, 1, data=D8)
                + frame(3, 3, 1, data=D8) + frame(3, 4, 1, data=D8))
        s = p.analyze(data)
        self.assertEqual((s.seq_gaps, s.seq_gap_events, s.seq_duplicates), (2, 1, 1))

    def test_timestamp_unwrap(self):
        u = p.Unwrapper(32)
        vals = [0xFFFFF000, 0xFFFFFF00, 0x00000100, 0x00001000]
        out = [u(v) for v in vals]
        self.assertEqual(out, [0xFFFFF000, 0xFFFFFF00, 0x100000100, 0x100001000])
        self.assertEqual((u.wraps, u.backwards), (1, 0))
        u(0x00000F00)  # small step backwards
        self.assertEqual(u.backwards, 1)

    def test_counter_delta_across_wrap(self):
        self.assertEqual(p.counter_delta(5, 0xFFFFFFFB), 10)
        self.assertEqual(p.counter_delta(100, 40), 60)
        self.assertEqual(p.counter_delta(3, 65533, bits=16), 6)

    def test_text_continuation(self):
        s = p.analyze(text(0, 'abc', cont=True) + text(1, 'def', cont=True) + text(2, 'ghi'))
        self.assertEqual([t for _, t in s.lines], ['abcdefghi'])
        s = p.analyze(text(0, 'abc', cont=True))
        self.assertEqual(s.errors[-1].kind, 'truncated')


class StreamAccounting(unittest.TestCase):
    """capture_phase3b stream checks on synthetic decoded sessions."""

    def make(self, seqs, drops=(), dlc=8):
        out, rs = bytearray(), 0
        out += text(rs, '# STREAM start dlc=%d ms=1000' % dlc); rs += 1
        for k, seq in enumerate(seqs):
            for pos, n in drops:
                if pos == k:
                    out += drop(rs, n, n); rs += 1
            if dlc == 8:
                d = bytes([0x4F, 0x4D, 0x5B]) + struct.pack('<I', seq) + bytes([~seq & 0xFF])
                out += frame(rs, 1000 + 250 * k, 0x100, dlc=8, data=d)
            else:
                out += frame(rs, 1000 + 130 * k, seq & 0x7FF, dlc=0)
            rs += 1
        out += text(rs, 'STREAM: PASS loss_free=YES')
        return p.analyze(bytes(out))

    def test_complete_stream(self):
        r = cap.check_stream_frames(self.make(range(100)), dlc=8)
        self.assertEqual((r['host_unique'], r['host_seq_gaps'], r['host_drop_reported']), (100, 0, 0))
        self.assertEqual(r['host_problems'], [])

    def test_gap_explained_by_drop_record(self):
        seqs = list(range(40)) + list(range(45, 100))
        r = cap.check_stream_frames(self.make(seqs, drops=[(40, 5)]), dlc=8)
        self.assertEqual((r['host_seq_gaps'], r['host_drop_reported']), (5, 5))
        self.assertEqual(r['host_unexplained_gaps'], 0)

    def test_gap_without_drop_record_is_unexplained(self):
        seqs = list(range(40)) + list(range(45, 100))
        r = cap.check_stream_frames(self.make(seqs), dlc=8)
        self.assertEqual(r['host_unexplained_gaps'], 5)

    def test_dlc0_id_wraps_mod_2048(self):
        r = cap.check_stream_frames(self.make(range(5000), dlc=0), dlc=0)
        self.assertEqual((r['host_unique'], r['host_seq_gaps']), (5000, 0))
        self.assertEqual(r['host_problems'], [])

    def test_duplicate_and_corrupt_frames_are_problems(self):
        r = cap.check_stream_frames(self.make([0, 1, 1, 2]), dlc=8)
        self.assertTrue(any('duplicate' in x for x in r['host_problems']))


class AsciiParsing(unittest.TestCase):
    def test_unrecognized_ascii_line_is_reported(self):
        raw = (b'OpenMoto Phase 3B CAN sniffer\r\nF 2313 3C4 S 0\r\n 2313 3C5 S 0\r\n'
               b'F 2314 3C6 S 0\r\nF 99 12 S 0\r\n# CAPTURE ON\r\n')
        evs, _, errs = cap.to_events(raw)
        self.assertEqual(sum(1 for k, _ in evs if k == 'frame'), 2)
        self.assertEqual([e['error'] for e in errs], ['unrecognized line', 'Malformed frame line: \'F 99 12 S 0\''])

    def test_binary_segment_ends_at_mode_ascii(self):
        raw = (b'READY x\r\n# MODE BINARY\r\n' + frame(0, 1, 0x100, data=D8) +
               text(1, '# MODE ASCII') + b'# MODE ASCII\r\nS ms=1 rx=0\r\n')
        evs, sessions, errs = cap.to_events(raw)
        self.assertEqual(errs, [])
        self.assertEqual(sessions[0][1].errors, [])
        self.assertEqual([v for k, v in evs if k == 'text'][-2:], ['# MODE ASCII', 'S ms=1 rx=0'])


if __name__ == '__main__':
    unittest.main(verbosity=2)
