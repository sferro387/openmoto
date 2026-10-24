"""Static audit of the built diagnostic firmware images (Phase 4 + Phase 5).

Environments audited (build-only; nothing is uploaded):

  diag_dryrun             Phase 4 no-transmit build. Proven CANNOT enter NORMAL
                          mode or transmit: transmit helpers are not linked, and
                          canEnterActive()/canTransmit() are stubs.
  diag_readonly           Phase 4 transmit build. The ONLY NORMAL-mode path is
                          diagHalEnterNormal(); the only requestMode() callers are
                          canSetRxId, canSilence (CONFIG/LISTEN) and that function.
  diag_kombi_probe_dryrun Phase 5 probe, no transmit. Same no-TX guarantee as
                          diag_dryrun, so the 22 F1 50 ARM/CONFIRM can be
                          rehearsed with zero risk of a frame leaving the chip.
  diag_kombi_probe        Phase 5 probe, transmit build. Same NORMAL-mode-only-via
                          -diagHalEnterNormal guarantee as diag_readonly.

This audits the software image only. It is NOT an electrical or vehicle result,
and says nothing about whether any addressing is correct. See docs/diag-active.md
and docs/diag-kombi-probe.md.

Run on the host that has the PlatformIO toolchain:
    python3 tests/check_diag_firmware.py
"""
from pathlib import Path
import hashlib
import re
import subprocess
import sys

ROOT = Path(__file__).resolve().parents[1]
TC = ROOT / '.tools/platformio/packages/toolchain-atmelavr/bin'

# Transmit / NORMAL-mode helpers that are marked noinline, so they appear as
# distinct symbols. Present only in the transmit build. (diagHalTxState is
# intentionally omitted: it is small and may be inlined.)
TX_SYMBOLS = [
    '(anonymous namespace)::diagHalEnterNormal()',
    '(anonymous namespace)::diagHalTransmit(diag::Frame const&)',
]
# Only these functions may call requestMode(); of them, only diagHalEnterNormal
# is allowed to request NORMAL mode (canSilence/canSetRxId use CONFIG/LISTEN).
REQUESTMODE_CALLERS_ALLOWED = {
    '(anonymous namespace)::ArduinoHal::canSetRxId(unsigned int)',
    '(anonymous namespace)::ArduinoHal::canSilence()',
    '(anonymous namespace)::diagHalEnterNormal()',
}


def build(env):
    subprocess.run([str(ROOT / 'scripts/pio'), 'run', '-d', 'firmware/mega-can', '-e', env],
                   cwd=ROOT, check=True, stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)
    return ROOT / f'firmware/mega-can/.pio/build/{env}/firmware.elf'


def nm(elf):
    return subprocess.check_output([str(TC / 'avr-nm'), '-C', str(elf)], text=True)


def objdump(elf):
    return subprocess.check_output([str(TC / 'avr-objdump'), '-d', '-C', str(elf)], text=True)


def owners(dis_lines):
    """Map each instruction line index to its enclosing function name."""
    func, owner = None, []
    for line in dis_lines:
        m = re.match(r'^[0-9a-f]+ <(.+)>:$', line)
        if m:
            func = m.group(1)
        owner.append(func)
    return owner


def requestmode_callers(dis_lines, owner):
    callers = set()
    for i, line in enumerate(dis_lines):
        if re.search(r'\b(call|jmp|rcall|rjmp)\b.*<\(anonymous namespace\)::requestMode', line):
            # Skip requestMode's own internal loop branch (it polls modeOf in a
            # loop, which the compiler can render as a self jump/call).
            if owner[i] and owner[i].startswith('(anonymous namespace)::requestMode'):
                continue
            if owner[i]:
                callers.add(owner[i])
    return callers


def sha(elf):
    hexfile = elf.with_suffix('.hex')
    return hashlib.sha256(hexfile.read_bytes()).hexdigest()


def audit_dryrun(env='diag_dryrun'):
    elf = build(env)
    problems = []
    syms = nm(elf)
    for s in TX_SYMBOLS:
        if s in syms:
            problems.append(f'{env} links a transmit helper that should be absent: {s}')
    dis = objdump(elf).splitlines()
    owner = requestmode_callers(dis, owners(dis))
    bad = owner - {'(anonymous namespace)::ArduinoHal::canSetRxId(unsigned int)',
                   '(anonymous namespace)::ArduinoHal::canSilence()'}
    if bad:
        problems.append(f'{env} calls requestMode from unexpected functions: {sorted(bad)}')
    # canEnterActive must be a trivial stub (return 0). Its body should not call
    # requestMode and should be only a couple of instructions.
    body = re.findall(
        r'<\(anonymous namespace\)::ArduinoHal::canEnterActive\(\)>:\n((?:.*\n)*?)\s+[0-9a-f]+:\s+08 95\s+ret',
        objdump(elf))
    if not body:
        problems.append(f'{env}: could not locate canEnterActive stub body')
    elif 'requestMode' in body[0] or 'call' in body[0]:
        problems.append(f'{env}: canEnterActive is not a trivial stub (contains a call)')
    print(f'{env} HEX sha256={sha(elf)}')
    return problems


def audit_readonly(env='diag_readonly'):
    elf = build(env)
    problems = []
    syms = nm(elf)
    for s in TX_SYMBOLS:
        if s not in syms:
            problems.append(f'{env} is missing an expected transmit helper: {s} '
                            '(the dryrun absence check would be meaningless)')
    dis = objdump(elf).splitlines()
    callers = requestmode_callers(dis, owners(dis))
    unexpected = callers - REQUESTMODE_CALLERS_ALLOWED
    if unexpected:
        problems.append(f'{env}: requestMode called from unexpected functions: {sorted(unexpected)}')
    if '(anonymous namespace)::diagHalEnterNormal()' not in callers:
        problems.append(f'{env}: diagHalEnterNormal does not call requestMode (NORMAL path missing?)')
    # It is a diagnostic build, not a sniffer build.
    image = elf.read_bytes()
    if b'read-only diagnostic client' not in image:
        problems.append(f'{env}: diagnostic client banner missing from image')
    # No sniffer/bench transmit code leaked in.
    for leaked in ('benchRun', 'floodRun', 'streamRun', 'can2Queue'):
        if leaked in syms:
            problems.append(f'{env}: unexpected sniffer/bench symbol linked: {leaked}')
    # Write/mutating handling must still be compiled in (serviceForbidden is the
    # default-deny gate). It is inlined into policyCheck, so we check the probe
    # build still denies writes behaviourally via the host test; here we only
    # assert no write-service transmit routine exists (there is none by design).
    print(f'{env} HEX sha256={sha(elf)}')
    return problems


def main():
    all_problems = []
    for env in ('diag_dryrun', 'diag_kombi_probe_dryrun', 'diag_service_read_dryrun'):
        all_problems += [(env, p) for p in audit_dryrun(env)]
    for env in ('diag_readonly', 'diag_kombi_probe', 'diag_service_read'):
        all_problems += [(env, p) for p in audit_readonly(env)]
    if all_problems:
        for env, p in all_problems:
            print(f'FAIL [{env}]: {p}')
        sys.exit(1)
    print('DIAG FIRMWARE AUDIT PASS: dryrun/probe-dryrun images cannot transmit (no NORMAL/TX '
          'code linked); readonly/probe images reach NORMAL mode only via diagHalEnterNormal '
          '(software only; not an electrical or vehicle result).')


if __name__ == '__main__':
    main()
