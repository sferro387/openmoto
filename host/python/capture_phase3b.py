"""Capture and validate a Phase 3B sniffer session. Opening serial/DTR resets the Mega.

Saves, for --output-prefix P:
  P.raw            every byte received from the port (ASCII and binary), unmodified
  P.log            text lines in order (ASCII lines and binary TEXT records)
  P.frames.jsonl   every decoded CAN frame (and DROP/STATS records) with offsets
  P.errors.jsonl   every decode error (offset, length, kind, raw hex) - never dropped
  P.summary.json   firmware report, host checks, throughput and verdict

Tests (bench firmware unless noted):
  passive  (any firmware) capture --seconds with START/STATS/STOP
  modes    (any firmware) ASCII/BINARY/COUNT switching and command responses
  bench    200 deterministic frames (Phase 3A method)
  stream   unique frames, CAN2 one-shot: --dlc 0|8 --ms N
  flood    one unACKed frame retransmitted: --dlc 0|8 --ms N
  wraptest WRAPTEST, then a binary STREAM across the millis()/micros() 32-bit wrap
See docs/phase3b.md.
"""
import argparse
import json
from pathlib import Path
import re
import sys
import time

sys.path.insert(0, str(Path(__file__).resolve().parent))
import phase3b_protocol as p  # noqa: E402
import capture_phase3a as p3a  # noqa: E402  (frame line parser, BENCH sequence)

MODE_BIN_LINE = b'# MODE BINARY\r\n'
BIN_EXIT_TEXTS = ('# MODE ASCII', '# MODE COUNT')
VERDICT = re.compile(r'^(BENCH|STREAM|FLOOD): ')
KNOWN_ASCII = re.compile(
    r'^(# .*|S ms=.*|OpenMoto Phase 3B CAN sniffer|Variant: .*|Config: .*|CAN1: LISTEN_ONLY .*|'
    r'ISOLATED BENCH ONLY.*|Init - .*|MCP2515 #[12] .*: (PASS|FAIL)|READY .*|INIT: FAIL.*|'
    r'CS/INT port mapping: FAIL|(BENCH|STREAM|FLOOD): .*)$')


# ---- Segmentation (shared by live capture and post-processing) -------------
def split_segments(raw):
    """Split a raw session into [('ascii', bytes), ('binary', bytes), ...]."""
    segs, pos = [], 0
    while pos < len(raw):
        k = raw.find(MODE_BIN_LINE, pos)
        if k < 0:
            segs.append(('ascii', raw[pos:]))
            break
        k += len(MODE_BIN_LINE)
        segs.append(('ascii', raw[pos:k]))
        dec, end = p.StreamDecoder(), None
        partial = ''
        for ev in dec.feed(raw[k:]) + dec.finish():
            if isinstance(ev, p.Record) and ev.type == p.T_TEXT:
                try:
                    d = p.decode_record(ev)
                except ValueError:
                    continue
                partial += d['text']
                if not d['continued']:
                    if partial in BIN_EXIT_TEXTS:
                        end = k + ev.offset + len(ev.raw)
                        break
                    partial = ''
        end = len(raw) if end is None else end
        segs.append(('binary', raw[k:end]))
        pos = end
    return segs


def to_events(raw):
    """Ordered events: ('text', str) | ('frame', dict) | ('drop', dict) | ('stats', dict)
    | ('hello', dict), plus binary Sessions and ASCII parse errors."""
    events, sessions, ascii_errors, base = [], [], [], 0
    for kind, seg in split_segments(raw):
        if kind == 'ascii':
            for line in seg.decode('ascii', errors='replace').splitlines():
                line = line.strip()
                if not line:
                    continue
                if line.startswith('F '):
                    try:
                        ms, can_id, ext, rtr, dlc, data = p3a.parse_frame(line)
                    except ValueError as exc:
                        ascii_errors.append({'offset': base, 'error': str(exc), 'line': line})
                        events.append(('text', line))
                        continue
                    events.append(('frame', {'ts_ms': ms, 'id': can_id, 'ext': ext, 'rtr': rtr,
                                             'dlc': dlc, 'data': data, 'ascii': True}))
                elif KNOWN_ASCII.match(line):
                    events.append(('text', line))
                else:
                    # Never silently accept an unrecognized line (e.g. a frame
                    # line that lost its leading 'F' in transport).
                    ascii_errors.append({'offset': base, 'error': 'unrecognized line', 'line': line})
                    events.append(('text', line))
        else:
            s = p.analyze(seg)
            for e in s.errors:
                e.offset += base
            sessions.append((base, s))
            text_at = {idx: t for idx, t in s.lines}
            for idx, r in enumerate(s.records):
                r['offset'] += base
                t = r['type']
                if t == 'TEXT':
                    if idx in text_at:
                        events.append(('text', text_at[idx]))
                else:
                    events.append((t.lower(), r))
        base += len(seg)
    return events, sessions, ascii_errors


def events_from_session(s):
    ev, text_at = [], {idx: t for idx, t in s.lines}
    for idx, r in enumerate(s.records):
        if r['type'] == 'TEXT':
            if idx in text_at:
                ev.append(('text', text_at[idx]))
        else:
            ev.append((r['type'].lower(), r))
    return ev


def window(events, start_prefix, verdict_prefix):
    """Events strictly between the start text and the verdict text (last occurrence)."""
    starts = [i for i, (k, v) in enumerate(events) if k == 'text' and v.startswith(start_prefix)]
    ends = [i for i, (k, v) in enumerate(events) if k == 'text' and v.startswith(verdict_prefix)]
    if not starts or not ends or ends[-1] < starts[-1]:
        raise ValueError(f'missing {start_prefix!r} .. {verdict_prefix!r}')
    return events[starts[-1] + 1:ends[-1]], events[ends[-1]][1], ends[-1]


def kv(line):
    return dict(tok.split('=', 1) for tok in line.split() if '=' in tok)


def report(lines, prefix):
    out = {}
    for line in lines:
        if line.startswith(prefix):
            out.update(kv(line))
    return out


# ---- Stream frame checks ---------------------------------------------------
def stream_seq(frame, dlc):
    if dlc == 8:
        d = frame['data']
        if (frame['ext'] or frame['rtr'] or frame['id'] != 0x100 or frame['dlc'] != 8 or
                d[:3] != b'OM[' or d[7] != (~d[3] & 0xFF)):
            return None
        return int.from_bytes(d[3:7], 'little')
    if frame['ext'] or frame['rtr'] or frame['dlc'] != 0:
        return None
    return frame['id']  # 11-bit rolling sequence


def check_stream_frames(evs, dlc):
    if isinstance(evs, p.Session):
        evs = events_from_session(evs)
    win, _, _ = window(evs, '# STREAM start', 'STREAM: ')
    problems, unique, gaps, drops, expect, last, frames = [], 0, 0, 0, 0, None, 0
    ts = p.Unwrapper(32)
    for kind, r in win:
        if kind == 'drop':
            drops += r['dropped']
        if kind != 'frame':
            continue
        frames += 1
        if 'ts_us' in r:
            ts(r['ts_us'])
        seq = stream_seq(r, dlc)
        if seq is None:
            problems.append(f'corrupt stream frame {r}')
            continue
        if dlc == 0:
            if last is not None and seq == (last & 0x7FF):
                problems.append(f'duplicate id {seq}')
                continue
            gap = (seq - (expect & 0x7FF)) & 0x7FF
            seq = expect + gap
        else:
            if last is not None and seq == last:
                problems.append(f'duplicate seq {seq}')
                continue
            if seq < expect:
                problems.append(f'reordered seq {seq} < {expect}')
                continue
            gap = seq - expect
        gaps += gap
        unique += 1
        last, expect = seq, seq + 1
    if ts.backwards:
        problems.append(f'frame timestamps went backwards {ts.backwards}x')
    return {'host_frames': frames, 'host_unique': unique, 'host_seq_gaps': gaps,
            'host_drop_reported': drops, 'host_unexplained_gaps': gaps - drops,
            'host_last_seq': last, 'host_expect': expect, 'host_ts_wraps': ts.wraps,
            'host_problems': problems}


# ---- Live capture ------------------------------------------------------------
class Live:
    """Incremental text extraction for live control decisions only. The saved
    raw bytes are re-analyzed from scratch afterwards (authoritative)."""

    def __init__(self, port, raw_file):
        self.port, self.raw_file = port, raw_file
        self.raw = bytearray()
        self.timeline = []          # (host_time, cumulative_bytes)
        self.text = []
        self._pos, self._binary, self._dec, self._dec_start, self._partial = 0, False, None, 0, ''

    def pump(self):
        n = self.port.in_waiting
        chunk = self.port.read(n if n else 1)
        if chunk:
            self.raw += chunk
            self.raw_file.write(chunk)
            self.timeline.append((time.monotonic(), len(self.raw)))
            self._parse()
        return chunk

    def _parse(self):
        while True:
            if not self._binary:
                nl = self.raw.find(b'\n', self._pos)
                if nl < 0:
                    return
                line = bytes(self.raw[self._pos:nl + 1])
                self._pos = nl + 1
                text = line.decode('ascii', errors='replace').strip()
                if text:
                    self.text.append(text)
                if line == MODE_BIN_LINE:
                    self._binary, self._dec, self._dec_start = True, p.StreamDecoder(), self._pos
            else:
                evs = self._dec.feed(bytes(self.raw[self._pos:]))
                self._pos = len(self.raw)
                for ev in evs:
                    if not (isinstance(ev, p.Record) and ev.type == p.T_TEXT):
                        continue
                    try:
                        d = p.decode_record(ev)
                    except ValueError:
                        continue
                    self._partial += d['text']
                    if d['continued']:
                        continue
                    line, self._partial = self._partial, ''
                    self.text.append(line)
                    if line in BIN_EXIT_TEXTS:
                        self._binary = False
                        self._pos = self._dec_start + ev.offset + len(ev.raw)
                        break
                if self._binary:
                    return

    def lines(self):
        return self.text

    def wait_text(self, predicate, seconds, start=0):
        deadline = time.monotonic() + seconds
        checked = start
        while time.monotonic() < deadline:
            self.pump()
            for line in self.text[checked:]:
                if predicate(line):
                    return line
            checked = len(self.text)
        raise TimeoutError('timed out waiting for firmware output (raw capture saved)')

    def settle(self, seconds=0.3):
        end = time.monotonic() + seconds
        while time.monotonic() < end:
            self.pump()

    def send(self, cmd):
        self.port.write(cmd.encode('ascii') + b'\n')
        self.port.flush()


def throughput(timeline, start_off, end_off):
    """Host-measured bytes/s between two raw offsets (chunk arrival times)."""
    t0 = next((t for t, n in timeline if n > start_off), None)
    t1 = next((t for t, n in timeline if n >= end_off), None)
    if t0 is None or t1 is None or t1 <= t0:
        return None
    return round((end_off - start_off) / (t1 - t0))


# ---- Validation --------------------------------------------------------------
def validate(args, raw, timeline):
    evs, sessions, ascii_errors = to_events(raw)
    lines = [v for k, v in evs if k == 'text']
    problems, res = [], {}
    if not lines or lines[0] != 'OpenMoto Phase 3B CAN sniffer':
        problems.append('missing Phase 3B banner')
    cfg = next((l for l in lines if l.startswith('Config: ')), '')
    c = kv(cfg)
    res['config'] = c
    if args.expect_spi and c.get('spi_hz') != str(args.expect_spi):
        problems.append(f'spi_hz {c.get("spi_hz")} != expected {args.expect_spi}')
    if c.get('baud') != str(args.baud):
        problems.append(f'firmware baud {c.get("baud")} != host baud {args.baud}')
    for item in ('MCP2515 #1 SPI: PASS', 'MCP2515 #2 SPI: PASS', 'MCP2515 #1 SPI stress: PASS',
                 'MCP2515 #2 SPI stress: PASS', 'MCP2515 #1 initialized 500k/8MHz: PASS',
                 'MCP2515 #1 accept-all std+ext filters: PASS', 'MCP2515 #1 LISTEN_ONLY: PASS',
                 'MCP2515 #2 CONFIG (silent): PASS'):
        if item not in lines:
            problems.append(f'missing init result: {item}')
    res['spi_stress'] = report(lines, '# SPI stress')
    expected_err = '# ERR unknown command: FOO' if args.test == 'modes' else None  # provoked
    if any('ALERT' in l or l.startswith('INIT: FAIL') or (l.startswith('# ERR') and l != expected_err)
           for l in lines):
        problems.append('firmware reported ALERT/ERR/INIT FAIL')
    # Decode integrity over every binary segment: zero tolerance.
    dec_errors = [e for _, s in sessions for e in s.errors]
    res['binary_bytes'] = sum(s.bytes_total for _, s in sessions)
    res['decode_errors'] = len(dec_errors)
    res['record_seq_gaps'] = sum(s.seq_gaps for _, s in sessions)
    res['record_seq_duplicates'] = sum(s.seq_duplicates + s.seq_backwards for _, s in sessions)
    res['ascii_parse_errors'] = len(ascii_errors)
    for _, s in sessions:
        if s.bytes_total != s.record_bytes + sum(e.length for e in s.errors):
            problems.append('byte accounting mismatch')
    if dec_errors:
        problems.append(f'{len(dec_errors)} binary decode errors')
    if res['record_seq_gaps'] or res['record_seq_duplicates']:
        problems.append('binary record sequence gaps/duplicates (transport loss)')
    if ascii_errors:
        problems.append(f'{len(ascii_errors)} malformed ASCII frame lines')
    hellos = [r for k, r in evs if k == 'hello']
    for h in hellos:
        if args.expect_spi and h['spi_hz'] != args.expect_spi or h['baud'] != args.baud:
            problems.append(f'HELLO config mismatch {h}')
    frames = [r for k, r in evs if k == 'frame']
    res['frames_total'] = len(frames)
    t = args.test
    if t in ('stream', 'wraptest'):
        win, verdict, _ = window(evs, '# STREAM start', 'STREAM: ')
        fw = report(lines, '# STREAM ')
        res['firmware'] = fw
        res['firmware_verdict'] = verdict
        if not verdict.startswith('STREAM: PASS'):
            problems.append(f'firmware {verdict}')
        if args.mode != 'count':
            host = check_stream_frames(evs, args.dlc)
            res.update(host)
            problems += host['host_problems']
            rx, printed, qd, lost, sent = (int(fw[k]) for k in
                                           ('rx', 'printed', 'queue_drop', 'lost', 'sent'))
            # Exact accounting: every frame CAN2 sent is either seen by the host
            # or missing; missing = controller loss (firmware 'lost') + queue drops.
            host_missing = host['host_seq_gaps'] + (sent - host['host_expect'])
            res['host_missing'] = host_missing
            if host['host_frames'] != printed:
                problems.append(f'host frames {host["host_frames"]} != firmware printed {printed}')
            if args.mode == 'binary' and host['host_drop_reported'] != qd:
                problems.append(f'DROP records {host["host_drop_reported"]} != queue_drop {qd}')
            if host['host_frames'] + qd != rx:
                problems.append('host frames + queue drops != firmware rx')
            if host['host_unique'] + host_missing != sent:
                problems.append(f'host unique {host["host_unique"]} + missing {host_missing} != sent {sent}')
            if host_missing != lost + qd:
                problems.append(f'host missing {host_missing} != lost {lost} + queue_drop {qd}')
            stats = [r for k, r in win if k == 'stats']
            res['stats_records_in_window'] = len(stats)
            if args.mode == 'binary' and len(stats) < int(fw['ms']) // 1000 - 1:
                problems.append(f'only {len(stats)} STATS heartbeats in {fw["ms"]} ms')
            if win and args.mode == 'binary':
                first = next((r['offset'] for k, r in win if k == 'frame'), None)
                last = max((r['offset'] for k, r in win if k == 'frame'), default=None)
                if first is not None and last is not None:
                    res['host_bytes_per_s'] = throughput(timeline, first, last)
        if t == 'wraptest':
            res['wraptest'] = report(lines, '# WRAPTEST')
            ms_unwrap = p.Unwrapper(32)
            wt = next(i for i, (k, v) in enumerate(evs) if k == 'text' and v.startswith('# WRAPTEST'))
            stats = [r for k, r in evs[wt:] if k == 'stats']
            ms_vals = [ms_unwrap(r['ms']) for r in stats]
            res['stats_ms_wraps'] = ms_unwrap.wraps
            res['host_ts_wraps'] = res.get('host_ts_wraps', 0)
            gaps = [b - a for a, b in zip(ms_vals, ms_vals[1:])]
            res['stats_interval_ms_min_max'] = [min(gaps), max(gaps)] if gaps else None
            if ms_unwrap.wraps != 1 or ms_unwrap.backwards:
                problems.append('millis() wrap not observed exactly once in STATS')
            if res.get('host_ts_wraps') != 1:
                problems.append('micros() wrap not observed exactly once in FRAME timestamps')
            if gaps and (min(gaps) < 900 or max(gaps) > 1600):
                problems.append(f'STATS heartbeat interval out of range {res["stats_interval_ms_min_max"]}')
    elif t == 'bench':
        win, verdict, _ = window(evs, '# BENCH start', 'BENCH: ')
        res['firmware'] = report(lines, '# BENCH ')
        res['firmware_verdict'] = verdict
        s_lines = [l for l in lines if l.startswith('S ')]
        res['stats'] = kv(s_lines[-1]) if s_lines else {}
        if verdict != 'BENCH: PASS':
            problems.append(f'firmware {verdict}')
        fr = [r for k, r in win if k == 'frame']
        uniq = []
        for r in fr:
            key = (r['id'], r['ext'], r['rtr'], r['dlc'], r['data'])
            if not uniq or uniq[-1] != key:
                uniq.append(key)
        res['host_frames'], res['host_unique'] = len(fr), len(uniq)
        if uniq != p3a.expected_bench_frames():
            problems.append('BENCH unique frame sequence differs from the 200-frame reference')
        if args.mode != 'count' and len(fr) != int(res['stats'].get('printed', -1)):
            problems.append('BENCH host frames != firmware printed')
    elif t == 'flood':
        win, verdict, _ = window(evs, '# FLOOD start', 'FLOOD: ')
        fw = report(lines, '# FLOOD dlc=')
        res['firmware'] = fw
        ref = (0x123, False, False, 8, bytes([0x4F, 0x4D, 0xF1, 0, 0, 0, 0xA5, 0x5A])) if args.dlc == 8 \
            else (0x7FF, False, False, 0, b'')
        fr = [r for k, r in win if k == 'frame']
        bad = [r for r in fr if (r['id'], r['ext'], r['rtr'], r['dlc'], r['data']) != ref]
        res['host_frames'] = len(fr)
        res['host_unique_frames'] = 1 if fr and not bad else len({(r['id'], r['data']) for r in fr})
        drops = sum(r['dropped'] for k, r in win if k == 'drop')
        res['host_drop_reported'] = drops
        if args.mode == 'binary':
            offs = [r['offset'] for k, r in win if k in ('frame', 'drop')]
            if len(offs) > 1:
                res['host_bytes_per_s'] = throughput(timeline, offs[0], offs[-1])
        if bad:
            problems.append(f'{len(bad)} unexpected frames during FLOOD')
        if fw.get('CAN2_stopped') != 'CONFIG':
            problems.append('CAN2 not stopped in CONFIG')
        if args.mode != 'count':
            if len(fr) != int(fw['printed']):
                problems.append('FLOOD host frames != firmware printed')
            if args.mode == 'binary' and drops != int(fw['queue_drop']):
                problems.append('FLOOD DROP records != queue_drop')
        for k in ('spi_errors', 'int_missed'):
            if int(fw.get(k, -1)) != 0:
                problems.append(f'FLOOD {k}={fw.get(k)}')
    elif t == 'passive':
        s_lines = [l for l in lines if l.startswith('S ')]
        res['stats'] = kv(s_lines[-1]) if s_lines else {}
        for k in ('mode_faults', 'spi_errors', 'int_missed', 'hw_overrun'):
            if res['stats'].get(k) != '0':
                problems.append(f'passive {k}={res["stats"].get(k)}')
    elif t == 'modes':
        need = ['# MODE BINARY', '# MODE ASCII', '# MODE COUNT', '# CAPTURE ON', '# CAPTURE OFF',
                '# COUNTERS RESET']
        for n in need:
            if n not in lines:
                problems.append(f'missing response {n!r}')
        if sum(1 for l in lines if l.startswith('S ')) < 3:
            problems.append('expected STATS responses in ASCII, BINARY and COUNT modes')
        if not hellos:
            problems.append('no HELLO record after BINARY')
        if not any(k == 'stats' for k, _ in evs):
            problems.append('no binary STATS heartbeat')
        if '# ERR unknown command: FOO' not in lines:
            problems.append('unknown command not rejected')
    res['problems'] = problems
    res['verdict'] = 'PASS' if not problems else 'FAIL'
    return res, evs, sessions, ascii_errors


def write_outputs(prefix, res, evs, sessions, ascii_errors, suffix=''):
    def path(ext):
        return prefix.with_suffix(suffix + ext)
    with path('.log').open('x') as f:
        for k, v in evs:
            if k == 'text':
                f.write(v + '\n')
    with path('.frames.jsonl').open('x') as f:
        for k, v in evs:
            if k in ('frame', 'drop', 'stats', 'hello'):
                d = {kk: (vv.hex() if isinstance(vv, bytes) else vv) for kk, vv in v.items()}
                d['kind'] = k
                f.write(json.dumps(d) + '\n')
    with path('.errors.jsonl').open('x') as f:
        for _, s in sessions:
            for e in s.errors:
                f.write(json.dumps({'offset': e.offset, 'length': e.length, 'kind': e.kind,
                                    'detail': e.detail, 'raw': e.raw.hex()}) + '\n')
        for e in ascii_errors:
            f.write(json.dumps({'ascii': True, **e}) + '\n')
    with path('.summary.json').open('x') as f:
        json.dump(res, f, indent=2, default=str)
        f.write('\n')


def revalidate(args):
    """Re-run validation offline from a saved .raw capture (no serial port)."""
    raw_path = Path(args.from_raw)
    data = raw_path.read_bytes()
    if raw_path.suffix == '.gz':   # committed evidence is gzip-compressed
        import gzip
        data = gzip.decompress(data)
        raw_path = raw_path.with_suffix('')
    res, evs, sessions, ascii_errors = validate(args, data, [])
    res.update(test=args.test, mode=args.mode, dlc=args.dlc, ms=args.ms, baud=args.baud,
               raw_file=raw_path.name, revalidated=True, host_bytes_per_s=None)
    out_prefix = Path(args.output_prefix) if args.output_prefix else raw_path.with_suffix('')
    write_outputs(out_prefix, res, evs, sessions, ascii_errors, suffix='.revalidated')
    print('VERDICT ' + res['verdict'], res['problems'])
    return 0 if res['verdict'] == 'PASS' else 1


def run(args):
    import serial
    prefix = Path(args.output_prefix)
    raw_path = prefix.with_suffix('.raw')
    with raw_path.open('xb') as raw_file:   # never overwrite evidence
        with serial.Serial(args.port, args.baud, timeout=0.05) as port:
            port.dtr = False
            time.sleep(0.1)
            port.reset_input_buffer()
            port.dtr = True
            live = Live(port, raw_file)
            live.wait_text(lambda l: l.startswith('READY ') or l.startswith('INIT: FAIL'), 15)
            if any(l.startswith('INIT: FAIL') for l in live.lines()):
                raise SystemExit('INIT: FAIL (raw capture saved)')
            t = args.test
            if t == 'modes':
                steps = [('HELP', '# Output:'), ('BINARY', '# MODE BINARY'), ('START', '# CAPTURE ON'),
                         ('STATS', 'S '), ('RESET', '# COUNTERS RESET'), ('FOO', '# ERR unknown'),
                         ('COUNT', '# MODE COUNT'), ('STATS', 'S '), ('ASCII', '# MODE ASCII'),
                         ('STATS', 'S '), ('STOP', '# CAPTURE OFF')]
                for cmd, want in steps:
                    before = len(live.lines())
                    live.send(cmd)
                    live.wait_text(lambda l, w=want: l.startswith(w), 5, start=before)
                    if cmd == 'BINARY':
                        live.settle(1.3)   # at least one STATS heartbeat
            else:
                if args.mode == 'binary':
                    live.send('BINARY')
                    live.wait_text(lambda l: l == '# MODE BINARY', 5)
                elif args.mode == 'count':
                    live.send('COUNT')
                    live.wait_text(lambda l: l == '# MODE COUNT', 5)
                if t == 'passive':
                    live.send('START')
                    live.wait_text(lambda l: l == '# CAPTURE ON', 5)
                    end = time.monotonic() + args.seconds
                    while time.monotonic() < end:
                        live.pump()
                    before = len(live.lines())
                    live.send('STATS')
                    live.wait_text(lambda l: l.startswith('S '), 10, start=before)
                else:
                    if t == 'wraptest':
                        live.send('WRAPTEST')
                        live.wait_text(lambda l: l.startswith('# WRAPTEST'), 5)
                        cmd = f'STREAM 8 {args.ms}'
                    elif t == 'bench':
                        cmd = 'BENCH'
                    else:
                        cmd = f'{t.upper()} {args.dlc} {args.ms}'
                    live.send(cmd)
                    live.wait_text(lambda l: bool(VERDICT.match(l)), args.ms / 1000 + 90)
                    live.settle(1.5)  # trailing S line / heartbeat
            if args.mode == 'binary' and t != 'modes':
                # End the binary segment at a record boundary: closing the port
                # while a record is in flight would (correctly) be reported as a
                # truncated record. ASCII makes the firmware finish and emit a
                # final '# MODE ASCII' TEXT record.
                before = len(live.lines())
                live.send('ASCII')
                live.wait_text(lambda l: l == '# MODE ASCII', 10, start=before)
                live.settle(0.2)
    raw = raw_path.read_bytes()
    res, evs, sessions, ascii_errors = validate(args, raw, live.timeline)
    res.update(test=t, mode=args.mode, dlc=args.dlc, ms=args.ms, port=args.port, baud=args.baud,
               raw_file=raw_path.name, raw_bytes=len(raw))
    write_outputs(prefix, res, evs, sessions, ascii_errors)
    with prefix.with_suffix('.timeline.json').open('x') as f:  # (host monotonic s, cumulative bytes)
        json.dump(live.timeline, f)
    for k in sorted(res):
        if k not in ('config',):
            print(f'RESULT {k}={res[k]}')
    print('VERDICT ' + res['verdict'])
    return 0 if res['verdict'] == 'PASS' else 1


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument('--port', default='')
    ap.add_argument('--from-raw', help='re-validate a saved .raw capture offline')
    ap.add_argument('--baud', type=int, required=True, help='host baud (must match firmware)')
    ap.add_argument('--expect-spi', type=int, default=0)
    ap.add_argument('--output-prefix', default='')
    ap.add_argument('--test', choices=['passive', 'modes', 'bench', 'stream', 'flood', 'wraptest'],
                    required=True)
    ap.add_argument('--mode', choices=['ascii', 'binary', 'count'], default='binary')
    ap.add_argument('--dlc', type=int, choices=[0, 8], default=8)
    ap.add_argument('--ms', type=int, default=5000)
    ap.add_argument('--seconds', type=float, default=10.0)
    a = ap.parse_args()
    if a.from_raw:
        sys.exit(revalidate(a))
    if not a.port or not a.output_prefix:
        ap.error('--port and --output-prefix are required for a live capture')
    sys.exit(run(a))


if __name__ == '__main__':
    main()
