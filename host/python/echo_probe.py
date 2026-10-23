"""Diagnostic: does host->device data appear in the device->host stream?

During a FLOOD (continuous device output, commands not processed until it
ends) the host sends a marker line every 50 ms. The firmware never prints the
marker except inside '# ERR unknown command: <marker>' replies after the flood.
Any other occurrence in the received bytes is host data leaking into the
device->host direction (USB-serial bridge corruption).
Usage: echo_probe.py --port P --baud B --output RAWFILE [--flood "FLOOD 8 5000"]
"""
import argparse
import json
import re
import time

MARK = b'ZQZQZQ'


def main():
    import serial
    ap = argparse.ArgumentParser()
    ap.add_argument('--port', required=True)
    ap.add_argument('--baud', type=int, required=True)
    ap.add_argument('--output', required=True)
    ap.add_argument('--flood', default='FLOOD 8 5000')
    ap.add_argument('--mode', default='ASCII')
    a = ap.parse_args()
    raw, sent = bytearray(), 0
    with open(a.output, 'xb') as f, serial.Serial(a.port, a.baud, timeout=0.01) as p:
        p.dtr = False; time.sleep(0.1); p.reset_input_buffer(); p.dtr = True

        def pump(seconds):
            end = time.monotonic() + seconds
            while time.monotonic() < end:
                chunk = p.read(p.in_waiting or 1)
                if chunk:
                    raw.extend(chunk); f.write(chunk)
        while b'READY ' not in raw:
            pump(0.1)
        if a.mode != 'ASCII':
            p.write(a.mode.encode() + b'\n'); pump(1.0)
        if a.flood == 'STATSPAM':
            # Commands arrive while long S lines (> TX buffer) are being written.
            p.write(b'START\n'); pump(0.3)
            t_end = time.monotonic() + 10.0
            while time.monotonic() < t_end:
                p.write(b'STATS\n'); sent += 1
                pump(0.04)
            pump(2.0)
            data = bytes(raw)
            lines = data.split(b'\n')
            s_lines = [l for l in lines if l.startswith(b'S ms=')]
            bad = [l for l in s_lines if not re.fullmatch(rb'S ms=\d+( [a-z_0-9]+=[0-9A-Fx.]+)+ capture=(ON|OFF)\r', l)]
            print(json.dumps({'stats_sent': sent, 's_lines': len(s_lines), 'malformed_s_lines': len(bad),
                              'examples': [b.decode('ascii', 'replace') for b in bad[:4]],
                              'raw_bytes': len(data)}, indent=1))
            return
        p.write(a.flood.encode() + b'\n')
        pump(0.5)
        t_end = time.monotonic() + 4.0
        while time.monotonic() < t_end:
            p.write(MARK + b'\n'); sent += 1
            pump(0.05)
        pump(6.0)
    data = bytes(raw)
    replies = len(re.findall(rb'# ERR unknown command: ' + MARK, data))
    total = data.count(MARK)
    # Occurrences not inside a firmware ERR reply (ASCII) or a TEXT record carrying it.
    leaked = total - replies
    res = {'markers_sent': sent, 'marker_occurrences': total, 'firmware_err_replies': replies,
           'leaked_into_device_stream': leaked, 'raw_bytes': len(data)}
    ctx = [data[max(0, m.start() - 24):m.end() + 4].decode('ascii', 'replace')
           for m in re.finditer(MARK, data)][:5]
    res['contexts'] = ctx
    print(json.dumps(res, indent=1))


if __name__ == '__main__':
    main()
