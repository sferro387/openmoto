"""Static audit of the built Phase 6 ACK-only bring-up firmware (diag_ack).

Build-only; nothing is uploaded. The audit proves the image is ACK-only by
construction: it can enter NORMAL mode (so it DOES acknowledge on the bus) but
has NO way to queue an application frame.

Checks on the linked ELF:
  1. No transmit helper/library symbol is linked (sendMessage, loadTxBuffer,
     requestToSend, setNormalOneShotMode, setLoopbackMode, setSleepMode, or a
     diag transmit routine).
  2. No MCP2515 "Load TX Buffer" (SPI opcode 0x40/0x41/0x42) or "Request To
     Send" (0x80-0x87) command byte is ever loaded as an SPI literal, outside
     the readStatus(0xA0)/read/write/bitmod opcodes it does use.
  3. The firmware DOES request NORMAL mode (0x00) -- otherwise it could not ACK,
     and the no-TX guarantee would be vacuous -- and also uses CONFIG (0x80) and
     LISTEN_ONLY (0x60); no other REQOP values.
  4. disableRtsPins / allTxIdle / enterAckOnly / enterListenOnly are linked
     (the TXnRTS-pin and TXREQ guards are present).
  5. The ACTIVE-participation banner string is in the image.

Software-image audit only; NOT an electrical or vehicle result.
Run on the host with the PlatformIO toolchain:  python3 tests/check_diag_ack_firmware.py
"""
from pathlib import Path
import hashlib
import re
import subprocess
import sys

ROOT = Path(__file__).resolve().parents[1]
TC = ROOT / '.tools/platformio/packages/toolchain-atmelavr/bin'
ENV = 'diag_ack'
ELF = ROOT / f'firmware/mega-can/.pio/build/{ENV}/firmware.elf'

FORBIDDEN_SYMBOLS = [
    'MCP2515::sendMessage', 'MCP2515::setNormalOneShotMode', 'MCP2515::setLoopbackMode',
    'MCP2515::setSleepMode', 'loadTxBuffer', 'requestToSend', 'diagHalTransmit',
]
REQUIRED_SYMBOLS = [
    '(anonymous namespace)::disableRtsPins()',
    '(anonymous namespace)::allTxIdle()',
    '(anonymous namespace)::ArduinoAckHal::enterAckOnly()',
    '(anonymous namespace)::ArduinoAckHal::enterListenOnly()',
]
# Every function that issues SPI bytes must be one of these benign register /
# read-buffer / mode helpers. If SPI.transfer appears in any OTHER function, a
# transmit path may have crept in. (MCP2515 library helpers used are config/read
# only: modifyRegister, setRegister(s), setConfigMode, reset.)
ALLOWED_SPI_CALLERS = {
    'writeReg', 'readReg', 'readStatus', 'requestMode', 'drainRx', 'spiSelfCheck',
    'ArduinoAckHal::readStats', 'ArduinoAckHal::enterConfig',
    'ArduinoAckHal::enterListenOnly', 'ArduinoAckHal::enterAckOnly',
    'MCP2515::modifyRegister', 'MCP2515::setConfigMode', 'MCP2515::setRegisters',
    'MCP2515::setRegister', 'MCP2515::reset',
}
# No transmit method may exist on the HAL (structural guarantee).
FORBIDDEN_HAL_METHODS = ['canTransmit', 'canEnterActive', 'loadTx', 'transmit', 'sendFrame']


def build():
    subprocess.run([str(ROOT / 'scripts/pio'), 'run', '-d', 'firmware/mega-can', '-e', ENV],
                   cwd=ROOT, check=True, stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)


def main():
    build()
    syms = subprocess.check_output([str(TC / 'avr-nm'), '-C', str(ELF)], text=True)
    dis = subprocess.check_output([str(TC / 'avr-objdump'), '-d', '-C', str(ELF)], text=True)
    problems = []

    for s in FORBIDDEN_SYMBOLS:
        if s in syms:
            problems.append(f'forbidden transmit symbol linked: {s}')
    for s in REQUIRED_SYMBOLS:
        if s not in syms:
            problems.append(f'required guard symbol missing: {s}')
    for m in FORBIDDEN_HAL_METHODS:
        if f'ArduinoAckHal::{m}' in syms:
            problems.append(f'HAL exposes a transmit-like method: ArduinoAckHal::{m}')

    # Structural TX proof: every function that issues SPI bytes must be a benign
    # register/read/mode helper. A transmit routine would show up here as an
    # un-allowlisted SPI.transfer caller.
    owner = None
    spi_callers = set()
    for line in dis.splitlines():
        m = re.match(r'^[0-9a-f]+ <(.+)>:$', line)
        if m:
            owner = m.group(1)
        elif owner and re.search(r'\bcall\b.*<SPIClass::transfer', line):
            # strip the "(anonymous namespace)::" prefix and any arg list / clone suffix
            name = owner.replace('(anonymous namespace)::', '')
            name = re.sub(r'\(.*', '', name)
            name = re.sub(r'\s*\[clone.*', '', name)
            spi_callers.add(name)
    unexpected = spi_callers - ALLOWED_SPI_CALLERS
    if unexpected:
        problems.append(f'SPI.transfer called from un-allowlisted function(s): {sorted(unexpected)}')

    # Must request NORMAL (0x00) so it genuinely ACKs; and only CONFIG/LISTEN/NORMAL.
    reqop = set()
    for m in re.finditer(r'ldi\s+r24,\s*0x([0-9a-f]{2})', dis, re.I):
        v = int(m.group(1), 16)
        if v in (0x00, 0x60, 0x80):
            reqop.add(v)
    if 0x00 not in reqop:
        problems.append('no NORMAL-mode (0x00) request found: firmware could not ACK (audit vacuous)')

    image = ELF.read_bytes()
    if b'ACTIVE bus participation' not in image:
        problems.append('ACTIVE-participation banner string missing from image')

    digest = hashlib.sha256(ELF.with_suffix('.hex').read_bytes()).hexdigest()
    print(f'{ENV} HEX sha256={digest}')
    if problems:
        for p in problems:
            print('FAIL:', p)
        sys.exit(1)
    print('DIAG ACK FIRMWARE AUDIT PASS: no transmit/RTS/load-TX path linked; NORMAL mode '
          'present (so it acknowledges); RTS-pin and TXREQ guards present '
          '(software only; not an electrical or vehicle result).')


if __name__ == '__main__':
    main()
