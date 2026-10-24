"""Static audit of the built p3b_sniffer firmware: no CAN transmit / non-silent mode path.

Builds `p3b_sniffer` with the project wrapper, then inspects the linked ELF:
  1. No MCP2515 transmit or non-silent-mode methods and no bench traffic code
     (sendMessage, setNormalMode, setNormalOneShotMode, setLoopbackMode,
     setSleepMode, can2Queue, benchRun, floodRun, streamRun, wrapTest).
  2. Every call/jump to MCP2515::setMode loads r22 (the mode) with 0x80 (CONFIG)
     or 0x60 (LISTEN_ONLY) - never 0x00 (NORMAL), 0x40 (LOOPBACK), 0x20 (SLEEP).
  3. CANCTRL (0x0F) is only modified inside MCP2515::setMode.
  4. The SPI data register (SPDR, I/O 0x2E) is written only by SPIClass::transfer.
  5. The firmware banner string still contains the bench-only warning.
This proves properties of the software image only. It says nothing about the
electrical interface (see docs/vehicle-readiness-r18.md).

Run from the repository root:  python3 tests/check_p3b_sniffer_passive.py
Negative control (must FAIL):   python3 tests/check_p3b_sniffer_passive.py p3b_bench_spi4_921k_pace20k
"""
from pathlib import Path
import re
import subprocess
import sys

ROOT = Path(__file__).resolve().parents[1]
TC = ROOT / '.tools/platformio/packages/toolchain-atmelavr/bin'
ENV = sys.argv[1] if len(sys.argv) > 1 else 'p3b_sniffer'  # other envs: negative control
ELF = ROOT / f'firmware/mega-can/.pio/build/{ENV}/firmware.elf'
FORBIDDEN = ['MCP2515::sendMessage', 'MCP2515::setNormalMode', 'MCP2515::setNormalOneShotMode',
             'MCP2515::setLoopbackMode', 'MCP2515::setSleepMode', 'can2Queue', 'benchRun',
             'floodRun', 'streamRun', 'wrapTest', 'can2RequestAbort']
ALLOWED_MODES = {0x80: 'CONFIG', 0x60: 'LISTEN_ONLY'}


def main():
    subprocess.run([str(ROOT / 'scripts/pio'), 'run', '-d', 'firmware/mega-can', '-e', ENV],
                   cwd=ROOT, check=True, stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)
    syms = subprocess.check_output([str(TC / 'avr-nm'), '-C', str(ELF)], text=True)
    dis = subprocess.check_output([str(TC / 'avr-objdump'), '-d', '-C', str(ELF)], text=True).splitlines()
    problems = []
    for name in FORBIDDEN:
        if name in syms:
            problems.append(f'forbidden symbol linked: {name}')
    # Map each instruction line to its enclosing function.
    func, owner = None, []
    for line in dis:
        m = re.match(r'^[0-9a-f]+ <(.+)>:$', line)
        if m:
            func = m.group(1)
        owner.append(func)
    set_mode_calls = 0
    for i, line in enumerate(dis):
        if re.search(r'\b(call|jmp|rcall|rjmp)\b.*<MCP2515::setMode\(MCP2515::CANCTRL_REQOP_MODE\)>$', line):
            if owner[i] and owner[i].startswith('MCP2515::setMode'):
                continue  # internal loop branch
            set_mode_calls += 1
            prev = [l for l in dis[max(0, i - 8):i] if re.search(r'\bldi\s+r22,', l)]
            vals = [int(re.search(r'ldi\s+r22,\s*0x([0-9a-f]+)', l).group(1), 16) for l in prev]
            if not vals or vals[-1] not in ALLOWED_MODES:
                problems.append(f'setMode call in <{owner[i]}> with mode {vals[-1:]} (allowed 0x80/0x60)')
        if re.search(r'\bout\s+0x2e,', line) and owner[i] != 'SPIClass::transfer(unsigned char)':
            problems.append(f'SPDR written outside SPIClass::transfer in <{owner[i]}>')
        if re.search(r'\bcall\b.*<MCP2515::modifyRegister', line):
            prev = [l for l in dis[max(0, i - 8):i] if re.search(r'\bldi\s+r22,', l)]
            if prev and re.search(r'ldi\s+r22,\s*0x0F\b', prev[-1], re.I) and \
                    not (owner[i] or '').startswith('MCP2515::setMode'):
                problems.append(f'CANCTRL modified outside setMode in <{owner[i]}>')
    if set_mode_calls == 0:
        problems.append('no setMode call sites found (analysis broken?)')
    image = ELF.read_bytes()
    if b'ISOLATED BENCH ONLY' not in image:
        problems.append('bench-only warning string missing from image')
    hexfile = ELF.with_suffix('.hex')
    import hashlib
    digest = hashlib.sha256(hexfile.read_bytes()).hexdigest()
    print(f'{ENV} HEX sha256={digest}')
    print(f'setMode call sites checked: {set_mode_calls}')
    if problems:
        for p in problems:
            print('FAIL:', p)
        sys.exit(1)
    print('P3B SNIFFER PASSIVE AUDIT PASS: no transmit/non-silent mode path in the linked image '
          '(software only; not an electrical safety statement)')


if __name__ == '__main__':
    main()
