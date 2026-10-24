"""OpenMoto Phase 4 read-only diagnostic CLI (diag_cli.py entry point).

Two transports:
  --sim PATH     drive the offline simulator (tests/diag_host/diag_sim). Default
                 for development and the only transport used by automated tests.
  --port DEV     drive a real Mega over serial. This opens the port (resetting
                 the board) and will transmit on whatever CAN bus is wired. It
                 is HARD-GATED: it refuses to run unless --i-have-read-prereqs is
                 passed AND a verified addressing profile is supplied AND the
                 operator confirms interactively. Even then, every single request
                 requires an explicit per-request confirmation.

Read-only by construction: only allowlisted, non-mutating requests can be armed,
and the host re-checks each request against the same policy as the firmware.
"""
import argparse
import sys
from pathlib import Path

from . import protocol
from .profile import load_profile, ProfileError
from .transport import SimTransport, SerialTransport

PKG_DIR = Path(__file__).resolve().parent
REPO_ROOT = PKG_DIR.parents[2]  # openmoto_diag -> python -> host -> repo root
DEFAULT_SIM = REPO_ROOT / 'tests/diag_host/.build/diag_sim'

TERMINAL_PREFIXES = ('READY', 'RESULT ', 'STATUS ', 'OK ', 'ERR ', 'ARMED ',
                     'DISARMED ', 'EVT ', 'POLICY ', 'ALERT ')


def _drain(transport, timeout=10.0):
    return transport.read_lines(timeout=timeout)


def _find(lines, *prefixes):
    for ln in lines:
        if any(ln.startswith(p) for p in prefixes):
            return ln
    return None


def list_requests():
    print('Allowlisted read-only requests (name -> bytes : description):')
    for r in protocol.ALLOWED_REQUESTS:
        print(f'  {r.name:24s} {r.request.hex().upper():8s} {r.description}')


def run_session(transport, profile, requests, target, assume_yes, log, kombi_probe=False):
    """Execute one or more armed+confirmed read-only requests over the transport."""
    def emit(line):
        transport.write_line(line)
        out = _drain(transport)
        for ln in out:
            print(f'< {ln}')
            if log:
                log.write(ln + '\n')
        return out

    print(f'# target={target} addressing via profile {profile.vehicle!r}')
    emit(profile.addr_command(target))
    emit(profile.pad_command())

    for token in requests:
        try:
            req = protocol.build_request_bytes(token)
        except ValueError as exc:
            print(f'! {exc}', file=sys.stderr)
            return 2
        decision = protocol.check_request(req, kombi_probe=kombi_probe)
        if not decision.allowed:
            print(f'! host policy refuses {token!r}: {decision.detail}', file=sys.stderr)
            return 2

        # ARM
        print(f'> ARM {req.hex().upper()}   ({decision.detail})')
        armed_out = emit('ARM ' + req.hex().upper())
        armed_line = _find(armed_out, 'ARMED ')
        if not armed_line:
            print('! request was not armed (see output above); skipping', file=sys.stderr)
            continue
        armed = protocol.parse_armed_line(armed_line)
        if armed is None:
            print('! could not parse ARMED line', file=sys.stderr)
            return 3

        # Per-request confirmation: the whole point of the ARM/CONFIRM gate.
        print(f'  Firmware will transmit CAN id {armed["tx_id"]:#05x} data {armed["data"]} '
              f'(tx_build={armed["tx_build"]}).')
        print(f'  Confirmation code from firmware: {armed["code"]}')
        if assume_yes:
            code = armed['code']
            print(f'  --yes: auto-confirming {code}')
        else:
            try:
                reply = input(f'  Type the code {armed["code"]} to transmit, or anything else to cancel: ').strip().upper()
            except EOFError:
                reply = ''
            code = reply
        emit('CONFIRM ' + code)

    return 0


def main(argv=None):
    ap = argparse.ArgumentParser(description='OpenMoto read-only diagnostic client')
    ap.add_argument('--list', action='store_true', help='list allowlisted requests and exit')
    ap.add_argument('--sim', nargs='?', const=str(DEFAULT_SIM),
                    help='use the offline simulator binary (default path if given with no value)')
    ap.add_argument('--port', help='serial port of the Mega (REAL transmission; hard-gated)')
    ap.add_argument('--baud', type=int, default=115200)
    ap.add_argument('--profile', help='addressing profile JSON (must be verified)')
    ap.add_argument('--target', help='target name within the profile')
    ap.add_argument('--request', action='append', default=[],
                    help='request name or raw hex; repeatable')
    ap.add_argument('--yes', action='store_true',
                    help='auto-confirm each request (simulator/testing only)')
    ap.add_argument('--i-have-read-prereqs', action='store_true',
                    help='assert docs/diag-active.md prerequisites are met (required for --port)')
    ap.add_argument('--kombi-probe', action='store_true',
                    help='enable the single Phase 5 KOMBI probe DID (22 F1 50); mirrors the '
                         'firmware OPENMOTO_DIAG_KOMBI_PROBE build flag. Still read-only.')
    ap.add_argument('--log', help='append the transcript to this file')
    args = ap.parse_args(argv)

    if args.list:
        list_requests()
        return 0

    if not args.sim and not args.port:
        ap.error('choose a transport: --sim [PATH] or --port DEV')
    if args.sim and args.port:
        ap.error('--sim and --port are mutually exclusive')
    if not args.profile or not args.target:
        ap.error('--profile and --target are required to run a session')

    try:
        profile = load_profile(args.profile)
        profile.require(args.target)
    except (ProfileError, OSError) as exc:
        print(f'! profile error: {exc}', file=sys.stderr)
        return 2

    if not args.request:
        ap.error('provide at least one --request (see --list)')

    # Pre-validate every request against the host policy before any session or
    # transport is opened, so a forbidden request fails fast with nothing sent.
    for token in args.request:
        try:
            req = protocol.build_request_bytes(token)
        except ValueError as exc:
            print(f'! {exc}', file=sys.stderr)
            return 2
        decision = protocol.check_request(req, kombi_probe=args.kombi_probe)
        if not decision.allowed:
            print(f'! host policy refuses {token!r}: {decision.detail}', file=sys.stderr)
            return 2

    # ---- real-hardware guard ------------------------------------------------
    if args.port:
        if not args.i_have_read_prereqs:
            print('! REFUSING real transmission: pass --i-have-read-prereqs only after the '
                  'docs/diag-active.md prerequisites (B1-B4) are met.', file=sys.stderr)
            return 4
        if args.yes:
            print('! --yes is not allowed with --port: each real request must be confirmed '
                  'interactively.', file=sys.stderr)
            return 4
        print('=' * 70)
        print('REAL CAN TRANSMISSION to', args.port)
        print('Opening the port resets the Mega and will transmit on the wired bus.')
        print('Profile:', profile.vehicle, '| bitrate', profile.bitrate_kbps, 'kbps',
              '| mode', profile.addressing_mode)
        print('=' * 70)
        try:
            ack = input("Type 'TRANSMIT' to proceed: ").strip()
        except EOFError:
            ack = ''
        if ack != 'TRANSMIT':
            print('aborted.', file=sys.stderr)
            return 4
        transport = SerialTransport(args.port, args.baud)
    else:
        sim_path = Path(args.sim)
        if not sim_path.exists():
            print(f'! simulator binary not found: {sim_path}\n'
                  f'  build it with: clang++ -std=c++17 -DOPENMOTO_DIAG_ALLOW_TX=1 '
                  f'tests/diag_host/diag_sim.cpp -o {sim_path}', file=sys.stderr)
            return 2
        transport = SimTransport(sim_path)

    log = open(args.log, 'a') if args.log else None
    try:
        return run_session(transport, profile, args.request, args.target, args.yes, log,
                           kombi_probe=args.kombi_probe)
    finally:
        transport.close()
        if log:
            log.close()


if __name__ == '__main__':
    sys.exit(main())
