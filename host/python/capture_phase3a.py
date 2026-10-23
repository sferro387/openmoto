"""Capture a Phase 3A CAN sniffer session. Opening serial/DTR resets the Mega.

Default: passive capture (sniffer_8mhz or sniffer_bench_8mhz) for --seconds.
--bench:  requires sniffer_bench_8mhz; sends BENCH and validates that CAN1
          (LISTEN_ONLY) saw every deterministic CAN2 test frame, in order.
Duplicate copies are expected on the two-node bench (CAN1 never ACKs) and are
reported, not treated as failures. See docs/phase3a.md.
"""
import argparse
from pathlib import Path
import re
import time

FRAME_RE = re.compile(
    r'^F (\d+) ([0-9A-F]{3}|[0-9A-F]{8}) ([SE]) (\d{1,2})(?: ([0-9A-F]+))?( R)?$')

# Must match SPECS / BENCH_ROUNDS / benchFrame() in firmware/mega-can/src/phase3a/main.cpp.
BENCH_SPECS = [
    (0x123, False, False, 8), (0x18DAF110, True, False, 8), (0x7FF, False, False, 0),
    (0x1FFFFFFF, True, False, 3), (0x321, False, True, 4), (0x0ABCDEF, True, True, 0),
    (0x000, False, False, 1), (0x456, False, False, 8),
]
BENCH_ROUNDS = 25

INIT_LINES = [
    'MCP2515 #1 SPI: PASS', 'MCP2515 #2 SPI: PASS',
    'MCP2515 #1 initialized 500k/8MHz: PASS',
    'MCP2515 #1 accept-all std+ext filters: PASS',
    'MCP2515 #1 LISTEN_ONLY: PASS', 'MCP2515 #2 CONFIG (silent): PASS',
]


def parse_frame(line):
    """Return (ms, id, ext, rtr, dlc, payload) or raise ValueError."""
    m = FRAME_RE.match(line)
    if not m:
        raise ValueError(f'Malformed frame line: {line!r}')
    ms, id_hex, kind, dlc, data, rtr = m.groups()
    ext, rtr, dlc = kind == 'E', rtr is not None, int(dlc)
    if len(id_hex) != (8 if ext else 3):
        raise ValueError(f'ID width does not match frame type: {line!r}')
    can_id = int(id_hex, 16)
    if can_id > (0x1FFFFFFF if ext else 0x7FF) or dlc > 15:
        raise ValueError(f'ID/DLC out of range: {line!r}')
    expected_len = 0 if rtr else min(dlc, 8)
    payload = bytes.fromhex(data) if data else b''
    if len(payload) != expected_len:
        raise ValueError(f'Payload length does not match DLC/RTR: {line!r}')
    return int(ms), can_id, ext, rtr, dlc, payload


def expected_bench_frames():
    frames, seq = [], 0
    for rnd in range(BENCH_ROUNDS):
        for index, (can_id, ext, rtr, dlc) in enumerate(BENCH_SPECS):
            pattern = bytes([0x4F, 0x4D, rnd, index, seq & 0xFF, (seq >> 8) & 0xFF, 0xA5, 0x5A])
            frames.append((can_id, ext, rtr, dlc, b'' if rtr else pattern[:min(dlc, 8)]))
            seq += 1
    return frames


def key_values(line):
    return dict(tok.split('=', 1) for tok in line.split()[1:] if '=' in tok)


def validate_init(lines, bench):
    if not lines or lines[0] != 'OpenMoto Phase 3A CAN sniffer':
        raise ValueError('Missing Phase 3A startup banner')
    required = INIT_LINES + (['MCP2515 #2 initialized 500k/8MHz: PASS'] if bench else [])
    for item in required:
        if item not in lines:
            raise ValueError(f'Missing init result: {item}')
    if not any(line.startswith('READY ') for line in lines):
        raise ValueError('Firmware never reported READY')
    if any('ALERT' in line or line.startswith('INIT: FAIL') for line in lines):
        raise ValueError('Firmware reported an alert or init failure')


def last_stats(lines):
    stats = [line for line in lines if line.startswith('S ')]
    if not stats:
        raise ValueError('No statistics line captured')
    return key_values(stats[-1])


def validate_passive(text):
    lines = text.splitlines()
    validate_init(lines, bench=False)
    frames = [parse_frame(line) for line in lines if line.startswith('F ')]
    stats = last_stats(lines)
    if int(stats['mode_faults']) != 0:
        raise ValueError('CAN1 left LISTEN_ONLY')
    if int(stats['tec_peak']) or int(stats['rec_peak']):
        raise ValueError('Nonzero CAN1 error counter in LISTEN_ONLY')
    if int(stats['serial_drop']) == 0 and len(frames) != int(stats['printed']):
        raise ValueError('Printed frame count does not match captured lines')
    return {'frames_printed': len(frames), **stats}


def validate_bench(text, runs=1):
    lines = text.splitlines()
    validate_init(lines, bench=True)
    if 'Variant: bench (CAN2 test traffic only on BENCH command)' not in lines:
        raise ValueError('Not the sniffer_bench_8mhz firmware')
    starts = [i for i, line in enumerate(lines) if line.startswith('# BENCH start')]
    verdicts = [i for i, line in enumerate(lines) if line.startswith('BENCH: ')]
    if len(starts) != runs or len(verdicts) != runs:
        raise ValueError(f'Expected exactly {runs} BENCH start/verdict pairs')
    results = []
    for run, (start, verdict) in enumerate(zip(starts, verdicts)):
        end = starts[run + 1] if run + 1 < runs else len(lines)
        if not start < verdict < end:
            raise ValueError('BENCH start/verdict out of order')
        results.append(_validate_bench_run(lines, start, verdict, end))
    if runs > 1 and any(r['unique_sequence'] != results[0]['unique_sequence'] for r in results):
        raise ValueError('Repeated BENCH runs printed different unique sequences')
    for r in results:
        del r['unique_sequence']
    return results


def _validate_bench_run(lines, start, verdict, end):
    if lines[verdict] != 'BENCH: PASS':
        raise ValueError('Firmware reported BENCH: FAIL')
    window = lines[start + 1:verdict]
    verdicts = [verdict]
    frames = [parse_frame(line) for line in window if line.startswith('F ')]
    times = [f[0] for f in frames]
    if times != sorted(times):
        raise ValueError('Frame timestamps went backwards')
    # Collapse consecutive identical copies (no-ACK retransmissions).
    unique = []
    for frame in frames:
        key = frame[1:]
        if not unique or unique[-1] != key:
            unique.append(key)
    expected = expected_bench_frames()
    summary = key_values(next(line for line in window if line.startswith('# BENCH frames_expected=')))
    stats = last_stats(lines[verdicts[0]:end])
    if int(summary['matched']) != len(expected) or int(summary['missing']) or int(summary['unexpected']):
        raise ValueError('Firmware bench summary is incomplete')
    if int(stats['mode_faults']) or int(stats['tec_peak']) or int(stats['rec_peak']):
        raise ValueError('CAN1 left LISTEN_ONLY or reported nonzero error counters')
    # Unconditional: a lost copy or unprinted line makes the evidence incomplete.
    for key in ('serial_drop', 'hw_overrun', 'int_missed'):
        if int(stats[key]) != 0:
            raise ValueError(f'Bench {key}={stats[key]} (must be 0)')
    if unique != expected:
        raise ValueError('Printed unique frames differ from the deterministic bench sequence')
    if len(frames) != int(stats['printed']) or int(stats['printed']) != int(stats['rx']):
        raise ValueError('Printed frame count does not match captured lines / received count')
    report = {}
    for line in lines[start:verdict]:
        if line.startswith('# BENCH copies_total=') or line.startswith('# BENCH CAN2 ') or \
                line.startswith('# BENCH CAN1 ') or line.startswith('# BENCH timing '):
            prefix = 'can2_' if 'CAN2 TEC' in line else 'can1_' if 'CAN1 mode' in line else ''
            report.update({prefix + k: v for k, v in key_values(line).items()})
    if int(report.get('copies_total', -1)) != len(frames):
        raise ValueError('Firmware copies_total does not match printed bench frames')
    return {'frames_printed': len(frames), 'unique_frames': len(unique), 'unique_sequence': unique,
            **summary, **report, **{'stats_' + k: v for k, v in stats.items()}}


# Serial command test (T2): (command, predicate on response line, description).
COMMAND_TESTS = [
    ('HELP', lambda l: l.startswith('# Frame: F <ms>'), 'help text'),
    ('STATS', lambda l: l.startswith('S ') and l.endswith('capture=OFF'), 'stats while stopped'),
    ('START', lambda l: l == '# CAPTURE ON', 'capture on'),
    ('STATS', lambda l: l.startswith('S ') and l.endswith('capture=ON'), 'stats while capturing'),
    ('RESET', lambda l: l == '# COUNTERS RESET', 'reset'),
    ('STATS', lambda l: l.startswith('S ') and ' rx=0 ' in l and ' merr=0 ' in l, 'zeroed counters'),
    ('stop', lambda l: l == '# CAPTURE OFF', 'case-insensitive stop'),
    ('FOO', lambda l: l == '# ERR unknown command: FOO', 'unknown command'),
    ('X' * 20, lambda l: l == '# ERR command too long', 'overlong command'),
]


FLOOD_FRAME = (0x123, False, False, 8, bytes([0x4F, 0x4D, 0xF1, 0, 0, 0, 0xA5, 0x5A]))


def validate_flood(text):
    """FLOOD is a measurement; validate integrity, not throughput."""
    lines = text.splitlines()
    starts = [i for i, l in enumerate(lines) if l.startswith('# FLOOD start')]
    done = [i for i, l in enumerate(lines) if l == 'FLOOD: DONE']
    if len(starts) != 1 or len(done) != 1 or done[0] < starts[0]:
        raise ValueError('Expected one complete FLOOD measurement')
    window = lines[starts[0] + 1:done[0]]
    for line in window:
        if line.startswith('F ') and parse_frame(line)[1:] != FLOOD_FRAME:
            raise ValueError(f'Unexpected frame during FLOOD: {line!r}')
    phases = [key_values(l) for l in window if l.startswith('# FLOOD frame=')]
    if [(p.get('frame'), p.get('print')) for p in phases] != \
            [('123_DLC8', 'ON'), ('123_DLC8', 'OFF'), ('7FF_DLC0', 'OFF')]:
        raise ValueError('Missing FLOOD phase results')
    for p in phases:
        if p['CAN1_mode'] != 'LISTEN_ONLY' or p['CAN2_stopped'] != 'CONFIG':
            raise ValueError('FLOOD safety/stop check failed')
    printed = sum(1 for l in window if l.startswith('F '))
    if printed != int(phases[0]['printed']) or any(int(p['printed']) for p in phases[1:]):
        raise ValueError('FLOOD printed count does not match captured lines')
    return {f'flood_{p["frame"]}_print_{p["print"].lower()}_{k}': v for p in phases
            for k, v in p.items() if k not in ('frame', 'print')}


class LineReader:
    def __init__(self, port, log):
        self.port, self.log, self.buf, self.lines = port, log, b'', []

    def poll(self):
        chunk = self.port.read(self.port.in_waiting or 1)
        self.buf += chunk
        new = []
        while b'\n' in self.buf:
            raw, self.buf = self.buf.split(b'\n', 1)
            line = raw.decode('ascii', errors='replace').rstrip('\r')
            if not line:
                continue
            self.log.write(line + '\n')
            self.log.flush()
            self.lines.append(line)
            new.append(line)
        return new

    def wait_for(self, predicate, seconds, echo=True):
        deadline = time.monotonic() + seconds
        while time.monotonic() < deadline:
            for line in self.poll():
                if echo and not line.startswith('F '):
                    print(line, flush=True)
                if predicate(line):
                    return line
        raise TimeoutError('Timed out waiting for firmware output; inspect saved log')


def main():
    import serial
    parser = argparse.ArgumentParser(description=__doc__,
                                     formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument('--port', required=True)
    parser.add_argument('--output', type=Path, required=True)
    parser.add_argument('--bench', action='store_true')
    parser.add_argument('--bench-runs', type=int, default=1, help='BENCH runs in one session')
    parser.add_argument('--flood', action='store_true', help='after BENCH, run FLOOD measurement')
    parser.add_argument('--commands', action='store_true', help='serial command test (T2)')
    parser.add_argument('--seconds', type=float, default=30.0, help='passive capture duration')
    parser.add_argument('--baud', type=int, default=115200)
    args = parser.parse_args()
    # Fail before touching hardware if this would overwrite earlier evidence.
    with args.output.open('x') as log:
        with serial.Serial(args.port, args.baud, timeout=0.2) as port:
            port.dtr = False
            time.sleep(0.1)
            port.reset_input_buffer()
            port.dtr = True
            reader = LineReader(port, log)
            reader.wait_for(lambda l: l.startswith('READY ') or l.startswith('INIT: FAIL'), 15)
            if reader.lines[-1].startswith('INIT: FAIL'):
                raise SystemExit('Firmware initialization failed; inspect saved log')
            if args.bench:
                for _ in range(args.bench_runs):
                    port.write(b'BENCH\n')
                    reader.wait_for(lambda l: l.startswith('BENCH: '), 120)
                    reader.wait_for(lambda l: l.startswith('S '), 10)
                runs = validate_bench('\n'.join(reader.lines), args.bench_runs)
                result = {f'run{i + 1}_{k}': v for i, r in enumerate(runs) for k, v in r.items()}
                if args.flood:
                    port.write(b'FLOOD\n')
                    reader.wait_for(lambda l: l == 'FLOOD: DONE', 60)
                    result.update(validate_flood('\n'.join(reader.lines)))
            elif args.flood:
                validate_init(reader.lines, bench=True)
                port.write(b'FLOOD\n')
                reader.wait_for(lambda l: l == 'FLOOD: DONE', 60)
                result = validate_flood('\n'.join(reader.lines))
            elif args.commands:
                validate_init(reader.lines, bench=False)
                result = {}
                for cmd, predicate, description in COMMAND_TESTS:
                    port.write(cmd.encode('ascii') + b'\n')
                    try:
                        reader.wait_for(predicate, 5)
                    except TimeoutError:
                        raise ValueError(f'Command test failed: {cmd!r} ({description})')
                    result[f'cmd_{description.replace(" ", "_")}'] = 'PASS'
            else:
                port.write(b'START\n')
                reader.wait_for(lambda l: l == '# CAPTURE ON', 5)
                end = time.monotonic() + args.seconds
                while time.monotonic() < end:
                    for line in reader.poll():
                        if not line.startswith('F '):
                            print(line, flush=True)
                port.write(b'STATS\n')
                reader.wait_for(lambda l: l.startswith('S '), 10)
                port.write(b'STOP\n')
                reader.wait_for(lambda l: l == '# CAPTURE OFF', 10)
                result = validate_passive('\n'.join(reader.lines))
    for key in sorted(result):
        print(f'RESULT {key}={result[key]}')
    mode = 'bench PASS validated' if args.bench else 'flood measurement validated' if args.flood else \
        'command test validated' if args.commands else 'passive capture validated'
    print('ACCEPTED: ' + mode)


if __name__ == '__main__':
    main()
