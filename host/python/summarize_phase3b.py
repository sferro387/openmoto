"""Aggregate Phase 3B benchmark runs into one JSON summary and a markdown table.

Usage: summarize_phase3b.py <run-dir> [<run-dir> ...] --json OUT.json [--md OUT.md]
Each run directory (tests/phase3b/<label>-<UTC>/) holds upload.log (with the HEX
SHA-256) and one <test>.summary.json per capture written by capture_phase3b.py.
Nothing is filtered: every capture found is listed, including failures.
"""
import argparse
import hashlib
import json
from pathlib import Path


def sha256(path):
    return hashlib.sha256(Path(path).read_bytes()).hexdigest()


def row(run, s):
    fw = s.get('firmware', {}) if isinstance(s.get('firmware'), dict) else {}
    g = lambda k, d='': fw.get(k, s.get(k, d))
    test = s.get('test')
    r = {
        'run': run, 'test': test, 'mode': s.get('mode'), 'dlc': s.get('dlc') if test in ('stream', 'flood', 'wraptest') else '',
        'spi_hz': s.get('config', {}).get('spi_hz'), 'baud': s.get('config', {}).get('baud'),
        'verdict': s.get('verdict'), 'problems': s.get('problems', []),
        'decode_errors': s.get('decode_errors'), 'record_seq_gaps': s.get('record_seq_gaps'),
        'host_bytes_per_s': s.get('host_bytes_per_s'),
        'raw_bytes': s.get('raw_bytes'), 'binary_bytes': s.get('binary_bytes'),
        'ascii_parse_errors': s.get('ascii_parse_errors'),
    }
    if test in ('stream', 'wraptest'):
        r.update({k: g(k) for k in ('sent', 'unique', 'lost', 'sent_per_s', 'unique_per_s', 'hw_overrun',
                                     'queue_drop', 'printed', 'int_missed', 'spi_errors', 'queue_peak',
                                     'min_free_ram', 'merr', 'max_flush_us', 'max_gap_us', 'max_service_us')})
        r['firmware_verdict'] = s.get('firmware_verdict')
        r['host_missing'] = s.get('host_missing')
    elif test == 'flood':
        r.update({k: g(k) for k in ('rx', 'rx_per_s', 'hw_overrun', 'queue_drop', 'printed', 'int_missed',
                                     'spi_errors', 'queue_peak', 'CAN2_TEC', 'CAN2_EFLG', 'max_flush_us',
                                     'max_gap_us', 'max_service_us', 'min_free_ram')})
    elif test == 'bench':
        st = s.get('stats', {})
        r.update({'matched': g('matched'), 'copies_total': g('copies_total'), 'hw_overrun': st.get('hw_overrun'),
                  'queue_drop': st.get('queue_drop'), 'int_missed': st.get('int_missed'),
                  'spi_errors': st.get('spi_errors'), 'firmware_verdict': s.get('firmware_verdict')})
    return r


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument('dirs', nargs='+')
    ap.add_argument('--json', required=True)
    ap.add_argument('--md')
    a = ap.parse_args()
    runs, rows = [], []
    for d in map(Path, a.dirs):
        up = (d / 'upload.log').read_text() if (d / 'upload.log').exists() else ''
        hexline = next((l for l in up.splitlines()
                        if l.endswith('firmware.hex') and len(l.split()[0]) == 64), '')
        files = sorted(d.glob('*.summary.json'))
        runs.append({'dir': str(d), 'hex_sha256': hexline.split()[0] if hexline else None,
                     'upload_ok': 'upload_exit=0' in up,
                     # committed evidence only (*.frames.jsonl is regenerable via --from-raw)
                     'evidence_sha256': {str(f): sha256(f) for f in sorted(d.iterdir())
                                         if f.is_file() and not f.name.endswith('.frames.jsonl')}})
        for f in files:
            rows.append(row(d.name, json.loads(f.read_text())))
    transport = {}
    for r in rows:
        key = (r['spi_hz'], r['baud'])
        t = transport.setdefault(f'spi{r["spi_hz"]}_baud{r["baud"]}', {'raw_bytes': 0, 'binary_bytes': 0,
                                                                        'decode_errors': 0, 'record_seq_gaps': 0,
                                                                        'ascii_parse_errors': 0, 'captures': 0})
        t['captures'] += 1
        for k in ('raw_bytes', 'binary_bytes', 'decode_errors', 'record_seq_gaps', 'ascii_parse_errors'):
            t[k] += int(r.get(k) or 0)
    Path(a.json).write_text(json.dumps({'runs': runs, 'transport_totals': transport, 'results': rows},
                                       indent=2) + '\n')
    if a.md:
        cols = ['run', 'test', 'mode', 'dlc', 'sent_per_s', 'unique_per_s', 'rx_per_s', 'lost', 'hw_overrun',
                'queue_drop', 'decode_errors', 'record_seq_gaps', 'int_missed', 'spi_errors', 'queue_peak',
                'host_bytes_per_s', 'verdict', 'firmware_verdict']
        lines = ['| ' + ' | '.join(cols) + ' |', '|' + '---|' * len(cols)]
        for r in rows:
            lines.append('| ' + ' | '.join(str(r.get(c, '')) if r.get(c) is not None else '' for c in cols) + ' |')
        Path(a.md).write_text('\n'.join(lines) + '\n')
    print(f'{len(rows)} captures from {len(runs)} runs')


if __name__ == '__main__':
    main()
