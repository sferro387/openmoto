"""Correlate binary decode errors with host read timing (diagnostic).

Usage: timeline_check.py <capture prefix> [...]
Reads P.errors.jsonl and P.timeline.json written by capture_phase3b.py. For each
transport error, reports the host-side gap between the USB read chunk that
contained the error and the previous chunk, compared with the distribution of
all inter-chunk gaps. Large host gaps at error positions would point to host
buffering; errors at ordinary gaps point to the device/USB-bridge side.
"""
import bisect
import json
import sys
from pathlib import Path


def main():
    for prefix in sys.argv[1:]:
        p = Path(prefix)
        tl = json.loads(p.with_suffix('.timeline.json').read_text())
        errs = [json.loads(l) for l in p.with_suffix('.errors.jsonl').read_text().splitlines() if l]
        ends = [n for _, n in tl]
        gaps = sorted((tl[i][0] - tl[i - 1][0]) * 1000 for i in range(1, len(tl)))
        pct = lambda q: gaps[min(len(gaps) - 1, int(q * len(gaps)))] if gaps else None
        print(f'{p.name}: chunks={len(tl)} bytes={ends[-1] if ends else 0} '
              f'gap_ms p50={pct(0.5):.2f} p99={pct(0.99):.2f} p999={pct(0.999):.2f} max={gaps[-1]:.2f}')
        seen = set()
        for e in errs:
            if e.get('kind') not in ('crc', 'bad_length', 'bad_version', 'truncated'):
                continue
            i = bisect.bisect_left(ends, e['offset'] + 1)
            if i in seen or i >= len(tl):
                continue
            seen.add(i)
            gap = (tl[i][0] - tl[i - 1][0]) * 1000 if i else float('nan')
            chunk = tl[i][1] - (tl[i - 1][1] if i else 0)
            rank = bisect.bisect_left(gaps, gap) / max(1, len(gaps))
            print(f'  error {e["kind"]} @ {e["offset"]}: chunk {i} size={chunk} gap_before={gap:.2f} ms '
                  f'(percentile {100 * rank:.1f})')


if __name__ == '__main__':
    main()
