"""Offline encoder/decoder for BMW R18 KOMBI service-write payloads.

Reverse-engineered from MotoScan 1.110 `sl0.C(dl0, int, Date, Date)`
(motoscan-smali/sl0.smali:435-1008). This module ONLY builds/parses byte
payloads for study and review. It performs NO I/O and transmits nothing; the
firmware write path stays disabled (Phase 8 constraint). See
docs/diag-kombi-service-reset.md for the evidence and the APK-confirmed vs
assumed split.

Field layouts (CONFIRMED [APK] from sl0.C aput-byte order):
  E12B clock           : 2E E1 2B [hour][min][sec][day][month+1][yearHi][yearLo]
  E12C next svc date   : 2E E1 2C [day][month+1][yearHi][yearLo]
  E12D service distance: 2E E1 2D [valHi][valLo]            (BE16, raw int, no scale)

Notes:
  * Java Calendar MONTH is 0-based; sl0.C adds 1, so the on-wire month is 1-12.
  * Year is 16-bit big-endian (full year, e.g. 0x07EA = 2026).
  * E12D is the raw integer the operator sets (MotoScan default "6000", parsed
    then double-to-int with NO multiply/divide) -> written verbatim as BE16.
    Whether the cluster treats it as remaining distance vs an absolute threshold
    is NOT decided by sl0.C; that is an ECU-side semantic -> [ASSUMPTION].
  * sl0.C performs NO diagnostic-session change (0x10) and NO security access
    (0x27); the three writes go straight to the transport with a 1500 ms gap.
"""
from dataclasses import dataclass


SVC_WRITE = 0x2E
DID_CLOCK = 0xE12B
DID_DATE = 0xE12C
DID_DIST = 0xE12D


def _be16(v: int) -> bytes:
    if not (0 <= v <= 0xFFFF):
        raise ValueError(f'value {v} out of 16-bit range')
    return bytes([(v >> 8) & 0xFF, v & 0xFF])


@dataclass(frozen=True)
class Clock:
    hour: int
    minute: int
    second: int
    day: int
    month: int   # 1-12 (on-wire, already +1 from Java 0-based)
    year: int    # full year, e.g. 2026


@dataclass(frozen=True)
class ServiceDate:
    day: int
    month: int   # 1-12
    year: int


# ---- encoders (build the exact 2E payload MotoScan would send) ----------------
def encode_clock(c: Clock) -> bytes:
    for name, v, lo, hi in [('hour', c.hour, 0, 23), ('minute', c.minute, 0, 59),
                            ('second', c.second, 0, 59), ('day', c.day, 1, 31),
                            ('month', c.month, 1, 12)]:
        if not (lo <= v <= hi):
            raise ValueError(f'{name}={v} out of range [{lo},{hi}]')
    return bytes([SVC_WRITE, DID_CLOCK >> 8, DID_CLOCK & 0xFF,
                  c.hour, c.minute, c.second, c.day, c.month]) + _be16(c.year)


def encode_date(d: ServiceDate) -> bytes:
    if not (1 <= d.day <= 31) or not (1 <= d.month <= 12):
        raise ValueError('date day/month out of range')
    return bytes([SVC_WRITE, DID_DATE >> 8, DID_DATE & 0xFF,
                  d.day, d.month]) + _be16(d.year)


def encode_distance(value: int) -> bytes:
    """E12D payload. `value` is the raw integer (MotoScan default 6000), written
    as BE16 with NO scaling. Units/semantics are an ECU-side assumption."""
    return bytes([SVC_WRITE, DID_DIST >> 8, DID_DIST & 0xFF]) + _be16(value)


# ---- decoders (parse either a 2E write payload or a 62 read response) ----------
def _strip_header(payload: bytes, sid_expect: int, did: int) -> bytes:
    if len(payload) < 3:
        raise ValueError('payload too short')
    sid, hi, lo = payload[0], payload[1], payload[2]
    if (hi << 8 | lo) != did:
        raise ValueError(f'DID {hi<<8|lo:04X} != expected {did:04X}')
    # Accept the write SID (0x2E) or the positive-read SID (0x62).
    if sid not in (SVC_WRITE, 0x62):
        raise ValueError(f'unexpected SID {sid:02X}')
    return payload[3:]


def decode_clock(payload: bytes) -> Clock:
    d = _strip_header(payload, SVC_WRITE, DID_CLOCK)
    if len(d) != 7:
        raise ValueError(f'clock data must be 7 bytes, got {len(d)}')
    return Clock(hour=d[0], minute=d[1], second=d[2], day=d[3], month=d[4],
                 year=(d[5] << 8) | d[6])


def decode_date(payload: bytes) -> ServiceDate:
    d = _strip_header(payload, SVC_WRITE, DID_DATE)
    if len(d) != 4:
        raise ValueError(f'date data must be 4 bytes, got {len(d)}')
    return ServiceDate(day=d[0], month=d[1], year=(d[2] << 8) | d[3])


def decode_distance(payload: bytes) -> int:
    d = _strip_header(payload, SVC_WRITE, DID_DIST)
    if len(d) != 2:
        raise ValueError(f'distance data must be 2 bytes, got {len(d)}')
    return (d[0] << 8) | d[1]


# ---- MotoScan's next-service calculation [APK] --------------------------------
# w1.smali:~2397 constructs next date = now + 31_536_000_000 ms (exactly 365
# days) and the clock = new Date(now). Distance default is the string "6000".
NEXT_SERVICE_DATE_OFFSET_MS = 0x757B12C00   # 31_536_000_000 ms == 365 days
DEFAULT_SERVICE_DISTANCE = 6000             # MotoScan UI default (raw, unscaled)
