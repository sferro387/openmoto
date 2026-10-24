"""OpenMoto Phase 4 host-side read-only diagnostic client.

This package drives the firmware diagnostic shell (ARM / CONFIRM protocol) over
a serial line, OR an offline simulator for tests. It enforces, host-side, the
same read-only policy the firmware enforces, so a mistake on either side is
caught. Nothing in this package is permitted to transmit on a real vehicle
until docs/diag-active.md prerequisites are met and a verified addressing
profile exists; see cli.py for the guard.
"""
from .protocol import (
    ALLOWED_REQUESTS, PolicyResult, check_request, parse_armed_line,
    parse_result_line, build_request_bytes,
)
from .profile import AddressingProfile, ProfileError, load_profile

__all__ = [
    'ALLOWED_REQUESTS', 'PolicyResult', 'check_request', 'parse_armed_line',
    'parse_result_line', 'build_request_bytes', 'AddressingProfile',
    'ProfileError', 'load_profile',
]
