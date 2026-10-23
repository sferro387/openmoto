"""Capture a real, isolated Phase 2 run. Opening serial/DTR resets the Mega."""
import argparse
from pathlib import Path
import re
import time



def validate_capture(text):
    lines = text.splitlines()
    if not lines or lines[0] != 'OpenMoto Phase 2' or lines[-1] != 'PHASE 2: PASS':
        raise ValueError('Missing Phase 2 startup or final PASS')
    if any('FAIL' in line for line in lines):
        raise ValueError('Firmware reported failure')
    for node in (1, 2):
        for check in ('SPI', 'initialized 500k/8MHz', 'NORMAL'):
            if f'MCP2515 #{node} {check}: PASS' not in lines:
                raise ValueError(f'Missing node {node} {check} result')
        if f'MCP2515 #{node} stopped: CONFIG' not in lines:
            raise ValueError('Controllers did not stop cleanly')
    headings = [
        'Test B CAN1 -> CAN2 requested=100',
        'Test C CAN2 -> CAN1 requested=100',
        'Test D CAN1 -> CAN2 requested=1000',
        'Test D CAN2 -> CAN1 requested=1000',
    ]
    starts = [i for i, line in enumerate(lines) if re.match(r'Test [BCD] CAN', line)]
    if [lines[i] for i in starts] != headings:
        raise ValueError('Missing, repeated, or unordered test directions')
    for index, start in enumerate(starts):
        block = lines[start:starts[index + 1] if index < 3 else len(lines)]
        expected = 100 if index < 2 else 1000
        for field in ('Frames requested', 'Frames queued', 'Frames transmitted (TX complete)',
                      'Frames received', 'Frames verified'):
            if f'{field}: {expected}' not in block:
                raise ValueError(f'Incomplete {field} in test {index + 1}')
        for field in ('Timeouts', 'Missing frames', 'Payload mismatches', 'Duplicate frames',
                      'Unexpected frames', 'Sequence errors', 'SPI/library/TX API errors',
                      'CAN controller error observations'):
            if f'{field}: 0' not in block:
                raise ValueError(f'Nonzero or missing {field}')
        if block.count('TEST: PASS') != 1:
            raise ValueError('Missing per-direction PASS')


def main():
    import serial
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--port', required=True)
    parser.add_argument('--output', type=Path, required=True)
    args = parser.parse_args()
    # Fail before touching hardware if this would overwrite earlier evidence.
    with args.output.open('x') as log:
        with serial.Serial(args.port, 115200, timeout=1) as port:
            port.dtr = False
            time.sleep(0.1)
            port.reset_input_buffer()
            port.dtr = True
            deadline = time.monotonic() + 90
            lines = []
            while time.monotonic() < deadline:
                line = port.readline().decode('ascii', errors='replace').strip()
                if not line:
                    continue
                print(line, flush=True)
                log.write(line + '\n')
                log.flush()
                lines.append(line)
                if line.startswith('PHASE 2:'):
                    validate_capture('\n'.join(lines))
                    print('ACCEPTED: actual firmware reported complete Phase 2 PASS', flush=True)
                    return
            raise TimeoutError('No complete Phase 2 result within 90 seconds; inspect saved log')


if __name__ == '__main__':
    main()
