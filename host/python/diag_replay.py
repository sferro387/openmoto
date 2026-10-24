#!/usr/bin/env python3
"""OpenMoto offline diagnostic-trace replay tool.

Import a verified diagnostic trace (JSON, ELM327/STN command log, or CAN/ISO-TP
frame log) and replay it OFFLINE against an in-process mock ECU built from the
trace's own recorded responses. This never opens a serial port and never touches
a motorcycle; it is for studying and regression-testing recovered sequences.

Read-only guard: a trace step that mutates ECU state (e.g. a 2E service write)
is reported BLOCKED and the replay stops, unless --allow-write-dryrun is given,
in which case writes are logged as explicit dry runs (still never transmitted).

Examples:
  diag_replay.py --json openmoto_diag/traces/kombi-read-r18.json
  diag_replay.py --elm tests/diag_traces/sample-elm-kombi.log
  diag_replay.py --can tests/diag_traces/sample-can-kombi.log --target 60 --tx-ext 60 --rx-ext F1
  diag_replay.py --json openmoto_diag/traces/kombi-service-reset-r18.json --allow-write-dryrun
"""
import argparse
import sys
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parent))
from openmoto_diag import trace as tracemod   # noqa: E402
from openmoto_diag import replay as replaymod  # noqa: E402


def _h(s):
    return int(s, 16)


def build_trace(args):
    if args.json:
        return tracemod.load_json_trace(args.json)
    if args.elm:
        return tracemod.parse_elm_log(Path(args.elm).read_text(), name=Path(args.elm).stem,
                                      evidence=tracemod.EV_INFERRED)
    if args.can:
        return tracemod.parse_can_log(
            Path(args.can).read_text(),
            tester_id=_h(args.tester), target_id=_h(args.target),
            tx_ext=_h(args.tx_ext) if args.tx_ext else None,
            rx_ext=_h(args.rx_ext) if args.rx_ext else None,
            name=Path(args.can).stem)
    return None


def main(argv=None):
    ap = argparse.ArgumentParser(description='OpenMoto offline diagnostic trace replay')
    src = ap.add_mutually_exclusive_group(required=True)
    src.add_argument('--json', help='JSON trace file')
    src.add_argument('--elm', help='ELM327/STN command log')
    src.add_argument('--can', help='CAN/ISO-TP frame log (candump-style)')
    ap.add_argument('--tester', default='6F1', help='tester CAN id (hex, default 6F1)')
    ap.add_argument('--target', default='60', help='ECU address byte (hex, default 60 = KOMBI)')
    ap.add_argument('--tx-ext', help='tester->ECU address extension byte (hex)')
    ap.add_argument('--rx-ext', help='ECU->tester address extension byte (hex)')
    ap.add_argument('--allow-write-dryrun', action='store_true',
                    help='log write steps as explicit dry runs instead of blocking (still never sent)')
    ap.add_argument('--to-json', help='convert the imported trace to JSON and write it here, then exit')
    args = ap.parse_args(argv)

    trace = build_trace(args)
    if trace is None:
        ap.error('no trace source provided')

    if args.to_json:
        Path(args.to_json).write_text(tracemod.dump_json_trace(trace))
        print(f'wrote {args.to_json} ({len(trace.steps)} steps)')
        return 0

    report = replaymod.replay(trace, allow_write_dryrun=args.allow_write_dryrun)
    for r in report.results:
        line = f'[{r.index:02d}] {r.status:12s}'
        if r.kind == 'at':
            line += f' {r.note}'
        else:
            line += f' req={r.request or "-"} exp={r.expected or "-"} got={r.got or "-"} [{r.evidence}]'
            if r.note:
                line += f'  # {r.note}'
        print(line)
    print('---')
    print(report.summary())
    if report.blocked:
        print(f'BLOCKED steps (read-only guard): {report.blocked}')
    return 0 if report.ok else 1


if __name__ == '__main__':
    sys.exit(main())
