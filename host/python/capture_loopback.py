"""Capture real Phase 1 serial evidence; opening/toggling DTR resets the Mega."""
import argparse
import re
import time
from pathlib import Path

import serial

parser = argparse.ArgumentParser(description=__doc__)
parser.add_argument('--port', required=True)
parser.add_argument('--output', type=Path, required=True)
args = parser.parse_args()
args.output.parent.mkdir(parents=True, exist_ok=True)
pattern = re.compile(r'PASS seq=(\d+) id=0x123 dlc=8 matched=8 total=(\d+)')
count = 0
previous = None
ready = False
with args.output.open('x') as log, serial.Serial(args.port, 115200, timeout=1) as port:
    port.dtr = False
    time.sleep(0.1)
    port.reset_input_buffer()
    port.dtr = True
    deadline = time.monotonic() + 40
    while time.monotonic() < deadline:
        line = port.readline().decode('ascii', errors='replace').strip()
        if not line:
            continue
        print(line, flush=True)
        log.write(line + '\n')
        log.flush()
        if 'FAIL' in line:
            raise SystemExit('Hardware loopback FAIL; see captured output')
        if line == 'READY loopback nominal_bitrate=500000':
            ready = True
            count = 0
            previous = None
        match = pattern.fullmatch(line)
        if match and ready:
            seq, total = map(int, match.groups())
            if total != seq + 1 or (previous is not None and seq != previous + 1):
                raise SystemExit('Nonconsecutive loopback sequence')
            previous = seq
            count += 1
            if count >= 20:
                print('ACCEPTED: READY and 20 consecutive matching loopback frames', flush=True)
                break
    else:
        raise SystemExit('Timed out before READY and 20 consecutive PASS frames')
