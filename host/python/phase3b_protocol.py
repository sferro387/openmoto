"""OpenMoto Phase 3B binary capture protocol v1: decoder and session analysis.

Specification: docs/phase3b.md ("Binary protocol v1"). The firmware encoder is
firmware/mega-can/src/phase3b/protocol.h.

Design rule: nothing is discarded silently. Every input byte ends up either in
a CRC-valid record or in a reported DecodeError span (garbage, crc, bad_version,
bad_length, truncated), so byte accounting is exact:
    total_bytes == sum(len(record.raw)) + sum(error.length)
"""
from dataclasses import dataclass, field
import struct

SYNC = b'\xA5\x5A'
VERSION = 1
HEADER_LEN, CRC_LEN, MAX_BODY = 7, 2, 64
T_HELLO, T_FRAME, T_DROP, T_STATS, T_TEXT = 0, 1, 2, 3, 4
TYPE_NAMES = {T_HELLO: 'HELLO', T_FRAME: 'FRAME', T_DROP: 'DROP', T_STATS: 'STATS', T_TEXT: 'TEXT'}
ID_EXT, ID_RTR, ID_RESERVED = 1 << 31, 1 << 30, 1 << 29
STD_MASK, EXT_MASK = 0x7FF, 0x1FFFFFFF
TEXT_CONTINUED = 0x01
STATS_FIELDS = ('ts_us', 'ms', 'rx', 'hw_overrun', 'queue_drop', 'merr', 'errif', 'int_missed',
                'mode_faults', 'spi_errors', 'records', 'queue_peak', 'eflg_or', 'tec_peak',
                'rec_peak', 'min_free_ram')
STATS_STRUCT = struct.Struct('<11I4BH')
HELLO_STRUCT = struct.Struct('<BBIIBI')


def crc16_bitwise(data, crc=0xFFFF):
    """Bitwise reference CRC-16/CCITT-FALSE (independent of the firmware's)."""
    for byte in data:
        crc ^= byte << 8
        for _ in range(8):
            crc = ((crc << 1) ^ 0x1021) & 0xFFFF if crc & 0x8000 else (crc << 1) & 0xFFFF
    return crc


_CRC_TABLE = [crc16_bitwise(bytes([i]), 0) for i in range(256)]


def crc16_ccitt_false(data, crc=0xFFFF):
    """Table-driven CRC-16/CCITT-FALSE (verified against crc16_bitwise in tests)."""
    tbl = _CRC_TABLE
    for byte in data:
        crc = ((crc << 8) & 0xFFFF) ^ tbl[(crc >> 8) ^ byte]
    return crc


@dataclass
class Record:
    offset: int      # absolute byte offset of the sync byte in the decoded stream
    type: int
    seq: int
    body: bytes
    raw: bytes


@dataclass
class DecodeError:
    offset: int
    length: int
    kind: str        # garbage | crc | bad_version | bad_length | truncated | invalid_record
    detail: str = ''
    raw: bytes = b''


class StreamDecoder:
    """Incremental framing decoder with resynchronization on any error."""

    def __init__(self):
        self.buf = bytearray()
        self.base = 0          # absolute offset of buf[0]
        self.bytes_in = 0

    def feed(self, data):
        self.buf += data
        self.bytes_in += len(data)
        return self._scan(final=False)

    def finish(self):
        return self._scan(final=True)

    def _err(self, events, start, end, kind, detail=''):
        if end > start:
            events.append(DecodeError(self.base + start, end - start, kind, detail,
                                      bytes(self.buf[start:end])))

    def _scan(self, final):
        events, buf, i = [], self.buf, 0
        while True:
            j = buf.find(SYNC, i)
            if j < 0:
                # Keep a trailing 0xA5 (possible first sync byte) unless final.
                keep = 1 if (not final and len(buf) > i and buf[-1] == SYNC[0]) else 0
                self._err(events, i, len(buf) - keep, 'garbage')
                i = len(buf) - keep
                break
            self._err(events, i, j, 'garbage')
            i = j
            avail = len(buf) - i
            if avail < HEADER_LEN:
                if final:
                    self._err(events, i, len(buf), 'truncated', 'incomplete header')
                    i = len(buf)
                break
            version, rtype, seq, length = buf[i + 2], buf[i + 3], buf[i + 4] | (buf[i + 5] << 8), buf[i + 6]
            if version != VERSION:
                self._err(events, i, i + 1, 'bad_version', f'version={version}')
                i += 1
                continue
            if length > MAX_BODY:
                self._err(events, i, i + 1, 'bad_length', f'len={length}')
                i += 1
                continue
            total = HEADER_LEN + length + CRC_LEN
            if avail < total:
                if final:
                    self._err(events, i, len(buf), 'truncated', f'need {total} bytes, have {avail}')
                    i = len(buf)
                break
            got = (buf[i + total - 2] << 8) | buf[i + total - 1]
            want = crc16_ccitt_false(buf[i + 2:i + total - 2])
            if got != want:
                self._err(events, i, i + 1, 'crc', f'got=0x{got:04X} want=0x{want:04X}')
                i += 1
                continue
            raw = bytes(buf[i:i + total])
            events.append(Record(self.base + i, rtype, seq, raw[HEADER_LEN:HEADER_LEN + length], raw))
            i += total
        del buf[:i]
        self.base += i
        return events


def decode_record(rec):
    """Return a dict for a CRC-valid record; raise ValueError for invalid content."""
    b, t = rec.body, rec.type
    if t == T_FRAME:
        if len(b) < 9:
            raise ValueError('FRAME body too short')
        ts, idf, dlc = struct.unpack_from('<IIB', b)
        if idf & ID_RESERVED:
            raise ValueError('FRAME reserved id bit set')
        if dlc > 15:
            raise ValueError('FRAME dlc > 15')
        ext, rtr = bool(idf & ID_EXT), bool(idf & ID_RTR)
        can_id = idf & EXT_MASK
        if not ext and can_id > STD_MASK:
            raise ValueError('FRAME standard id > 0x7FF')
        n = 0 if rtr else min(dlc, 8)
        if len(b) != 9 + n:
            raise ValueError(f'FRAME length {len(b)} != {9 + n} for dlc={dlc} rtr={rtr}')
        return {'type': 'FRAME', 'seq': rec.seq, 'ts_us': ts, 'id': can_id, 'ext': ext, 'rtr': rtr,
                'dlc': dlc, 'data': bytes(b[9:])}
    if t == T_DROP:
        if len(b) != 8:
            raise ValueError('DROP body length != 8')
        dropped, total = struct.unpack('<II', b)
        if dropped == 0:
            raise ValueError('DROP with zero count')
        return {'type': 'DROP', 'seq': rec.seq, 'dropped': dropped, 'total': total}
    if t == T_STATS:
        if len(b) != STATS_STRUCT.size:
            raise ValueError('STATS body length mismatch')
        d = dict(zip(STATS_FIELDS, STATS_STRUCT.unpack(b)))
        d.update(type='STATS', seq=rec.seq)
        return d
    if t == T_TEXT:
        if len(b) < 1 or b[0] & ~TEXT_CONTINUED:
            raise ValueError('TEXT flags invalid')
        text = b[1:]
        if any(c < 0x20 or c > 0x7E for c in text):
            raise ValueError('TEXT contains non-printable bytes')
        return {'type': 'TEXT', 'seq': rec.seq, 'continued': bool(b[0] & TEXT_CONTINUED),
                'text': text.decode('ascii')}
    if t == T_HELLO:
        if len(b) != HELLO_STRUCT.size:
            raise ValueError('HELLO body length mismatch')
        proto, variant, spi_hz, baud, qsize, ts = HELLO_STRUCT.unpack(b)
        if proto != VERSION:
            raise ValueError('HELLO protocol version mismatch')
        return {'type': 'HELLO', 'seq': rec.seq, 'protocol': proto, 'variant': variant,
                'spi_hz': spi_hz, 'baud': baud, 'queue_size': qsize, 'ts_us': ts}
    raise ValueError(f'unknown record type {t}')


class Unwrapper:
    """Extend a wrapping unsigned counter (timestamps, sequence numbers)."""

    def __init__(self, bits):
        self.mod, self.half = 1 << bits, 1 << (bits - 1)
        self.last_raw = None
        self.value = None
        self.wraps = 0
        self.backwards = 0

    def __call__(self, raw):
        if self.last_raw is None:
            self.value = raw
        else:
            delta = (raw - self.last_raw) % self.mod
            if delta < self.half:
                if raw < self.last_raw:
                    self.wraps += 1
                self.value += delta
            else:
                self.backwards += 1
                self.value -= self.mod - delta
        self.last_raw = raw
        return self.value


def counter_delta(new, old, bits=32):
    """Increase of a wrapping unsigned counter."""
    return (new - old) % (1 << bits)


@dataclass
class Session:
    """Ordered, decoded view of one binary capture."""
    records: list = field(default_factory=list)      # decoded dicts, in stream order
    errors: list = field(default_factory=list)       # DecodeError
    seq_gaps: int = 0                                 # records missing per sequence numbers
    seq_gap_events: int = 0
    seq_duplicates: int = 0
    seq_backwards: int = 0
    lines: list = field(default_factory=list)        # reassembled TEXT lines (index, text)
    bytes_total: int = 0
    record_bytes: int = 0


def analyze(data, chunk=None):
    """Decode a complete binary byte string into a Session."""
    dec, s = StreamDecoder(), Session()
    events = []
    if chunk:
        for k in range(0, len(data), chunk):
            events += dec.feed(data[k:k + chunk])
    else:
        events += dec.feed(data)
    events += dec.finish()
    s.bytes_total = dec.bytes_in
    last_seq, partial = None, ''
    for ev in events:
        if isinstance(ev, DecodeError):
            s.errors.append(ev)
            continue
        s.record_bytes += len(ev.raw)
        if last_seq is not None:
            delta = (ev.seq - last_seq) % 65536
            if delta == 0:
                s.seq_duplicates += 1
            elif delta >= 32768:
                s.seq_backwards += 1
            elif delta > 1:
                s.seq_gaps += delta - 1
                s.seq_gap_events += 1
        last_seq = ev.seq
        try:
            d = decode_record(ev)
        except ValueError as exc:
            s.errors.append(DecodeError(ev.offset, len(ev.raw), 'invalid_record', str(exc), ev.raw))
            continue
        d['offset'] = ev.offset
        s.records.append(d)
        if d['type'] == 'TEXT':
            partial += d['text']
            if not d['continued']:
                s.lines.append((len(s.records) - 1, partial))
                partial = ''
    if partial:
        s.errors.append(DecodeError(dec.base, 0, 'truncated', 'unterminated TEXT line'))
    return s
