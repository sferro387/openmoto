"""Read-only UDS request policy and firmware shell transcript parsing.

The firmware (firmware/mega-can/src/diag/diag_core.h) is the authority on what
is allowed; this module mirrors the SAME allowlist so the host refuses an
unsafe request before it is ever armed, and so the CLI can describe each
request to the operator. If the two disagree, the firmware still denies the
request, but the mismatch is a bug and tests check they agree.

Everything here is non-mutating by construction. No write, reset, security,
routine, coding, or adaptation service is representable as an allowed request.
"""
from dataclasses import dataclass
import re

# ISO 14229-1 identification DIDs only (must match ALLOWED_DIDS in diag_core.h).
ALLOWED_DIDS = (0xF186, 0xF187, 0xF188, 0xF189, 0xF18A, 0xF18B, 0xF18C,
                0xF190, 0xF191, 0xF192, 0xF193, 0xF194, 0xF195, 0xF197)

# Phase 5: BMW KOMBI identification DID (manufacturer-specific, outside the ISO
# range). Allowed ONLY when the KOMBI probe is explicitly enabled, mirroring the
# firmware's OPENMOTO_DIAG_KOMBI_PROBE build flag. It is still a pure read (0x22).
KOMBI_PROBE_DID = 0xF150

# Phase 7: four KOMBI service-inspection DIDs (manufacturer-specific). Allowed
# ONLY when service-read is explicitly enabled, mirroring the firmware
# OPENMOTO_DIAG_SERVICE_READ build flag. All are pure reads (service 0x22).
SERVICE_READ_DIDS = (0xE119, 0xE12B, 0xE12C, 0xE12D)
SERVICE_DID_NAMES = {
    0xE119: 'odometer-related', 0xE12B: 'clock-related',
    0xE12C: 'service-date-related', 0xE12D: 'service-distance-related',
}

# Services that mutate state / security / memory / comms. Denied by name; also
# denied by default. Must match serviceForbidden() in diag_core.h.
FORBIDDEN_SERVICES = frozenset({
    0x10, 0x11, 0x14, 0x27, 0x28, 0x2A, 0x2C, 0x2E, 0x2F, 0x31, 0x34, 0x35,
    0x36, 0x37, 0x38, 0x3D, 0x83, 0x84, 0x85, 0x86, 0x87,
})

DID_NAMES = {
    0xF186: 'ActiveDiagnosticSession', 0xF187: 'VehicleManufacturerSparePartNumber',
    0xF188: 'VehicleManufacturerECUSoftwareNumber', 0xF189: 'VehicleManufacturerECUSoftwareVersion',
    0xF18A: 'SystemSupplierIdentifier', 0xF18B: 'ECUManufacturingDate',
    0xF18C: 'ECUSerialNumber', 0xF190: 'VIN', 0xF191: 'VehicleManufacturerECUHardwareNumber',
    0xF192: 'SystemSupplierECUHardwareNumber', 0xF193: 'SystemSupplierECUHardwareVersionNumber',
    0xF194: 'SystemSupplierECUSoftwareNumber', 0xF195: 'SystemSupplierECUSoftwareVersionNumber',
    0xF197: 'SystemNameOrEngineType',
}


@dataclass(frozen=True)
class RequestSpec:
    """A named, allowlisted read-only request the operator may choose."""
    name: str
    request: bytes
    description: str


def _did_requests():
    out = []
    for did in ALLOWED_DIDS:
        out.append(RequestSpec(
            name=f'read_did_{did:04X}',
            request=bytes([0x22, did >> 8, did & 0xFF]),
            description=f'ReadDataByIdentifier {did:04X} ({DID_NAMES.get(did, "?")})',
        ))
    return out


ALLOWED_REQUESTS = tuple([
    RequestSpec('tester_present', bytes([0x3E, 0x00]), 'TesterPresent (no suppressPositiveResponse)'),
    RequestSpec('dtc_count', bytes([0x19, 0x01, 0xFF]),
                'ReadDTCInformation reportNumberOfDTCByStatusMask (mask=FF)'),
    RequestSpec('dtc_list', bytes([0x19, 0x02, 0xFF]),
                'ReadDTCInformation reportDTCByStatusMask (mask=FF)'),
] + _did_requests())

# Phase 5 probe request (only usable when the KOMBI probe is enabled).
KOMBI_PROBE_REQUEST = RequestSpec(
    'kombi_probe', bytes([0x22, 0xF1, 0x50]),
    'ReadDataByIdentifier F150 (BMW KOMBI identification probe) [Phase 5, requires kombi_probe]')

# Phase 7 service-read requests (only usable when service-read is enabled).
SERVICE_READ_REQUESTS = tuple(
    RequestSpec(
        f'service_{did:04X}', bytes([0x22, did >> 8, did & 0xFF]),
        f'ReadDataByIdentifier {did:04X} ({SERVICE_DID_NAMES[did]}) '
        f'[Phase 7, requires service_read]')
    for did in SERVICE_READ_DIDS
)

REQUESTS_BY_NAME = {r.name: r for r in ALLOWED_REQUESTS}
REQUESTS_BY_NAME[KOMBI_PROBE_REQUEST.name] = KOMBI_PROBE_REQUEST
for _r in SERVICE_READ_REQUESTS:
    REQUESTS_BY_NAME[_r.name] = _r


@dataclass(frozen=True)
class PolicyResult:
    allowed: bool
    kind: str
    detail: str


def check_request(req: bytes, kombi_probe: bool = False,
                  service_read: bool = False) -> PolicyResult:
    """Mirror diag_core.h policyCheck(). Returns whether this exact request is allowed.

    kombi_probe mirrors the firmware OPENMOTO_DIAG_KOMBI_PROBE flag: when True,
    the single manufacturer-specific read 22 F1 50 (DID 0xF150) is additionally
    allowed. service_read mirrors OPENMOTO_DIAG_SERVICE_READ: when True, the four
    service DIDs E119/E12B/E12C/E12D are additionally allowed. Both only ever
    permit service 0x22 pure reads; no write/reset/security path is opened."""
    if len(req) == 0:
        return PolicyResult(False, 'deny', 'empty-request')
    sid = req[0]
    if sid in FORBIDDEN_SERVICES:
        return PolicyResult(False, 'deny', 'service-forbidden(mutating-or-security)')
    if sid == 0x3E:
        if len(req) != 2:
            return PolicyResult(False, 'deny', 'length-not-allowed')
        return (PolicyResult(True, 'TesterPresent', 'TesterPresent') if req[1] == 0x00
                else PolicyResult(False, 'deny', 'subfunction-not-allowlisted'))
    if sid == 0x22:
        if len(req) != 3:
            return PolicyResult(False, 'deny', 'length-not-allowed')
        did = req[1] << 8 | req[2]
        allowed = (did in ALLOWED_DIDS
                   or (kombi_probe and did == KOMBI_PROBE_DID)
                   or (service_read and did in SERVICE_READ_DIDS))
        return (PolicyResult(True, 'ReadDataByIdentifier', f'ReadDID {did:04X}')
                if allowed else PolicyResult(False, 'deny', 'DID-not-allowlisted'))
    if sid == 0x19:
        if len(req) != 3:
            return PolicyResult(False, 'deny', 'length-not-allowed')
        if req[1] == 0x01:
            return PolicyResult(True, 'ReadDTCInformation', 'reportNumberOfDTCByStatusMask')
        if req[1] == 0x02:
            return PolicyResult(True, 'ReadDTCInformation', 'reportDTCByStatusMask')
        return PolicyResult(False, 'deny', 'subfunction-not-allowlisted')
    return PolicyResult(False, 'deny', 'service-not-allowlisted')


def build_request_bytes(token: str) -> bytes:
    """Resolve a CLI token to request bytes: a named request or raw hex."""
    if token in REQUESTS_BY_NAME:
        return REQUESTS_BY_NAME[token].request
    cleaned = token.replace(' ', '').replace(':', '')
    if len(cleaned) == 0 or len(cleaned) % 2 != 0:
        raise ValueError(f'not a known request name or even-length hex: {token!r}')
    try:
        return bytes.fromhex(cleaned)
    except ValueError as exc:
        raise ValueError(f'invalid hex request {token!r}: {exc}') from exc


_ARMED_RE = re.compile(
    r'ARMED code=(?P<code>[0-9A-Fa-f]{4}) req=(?P<req>[0-9A-Fa-f]+) '
    r'desc=(?P<desc>\S+) tx_id=(?P<tx_id>[0-9A-Fa-f]{3}) dlc=(?P<dlc>\d+) '
    r'data=(?P<data>[0-9A-Fa-f]+) max_tx_frames=(?P<max_tx>\d+) '
    r'expires_ms=(?P<ttl>\d+) tx_build=(?P<tx_build>\d)')


def parse_armed_line(line: str):
    """Parse an ARMED line into a dict, or None if it is not one."""
    m = _ARMED_RE.search(line)
    if not m:
        return None
    d = m.groupdict()
    return {
        'code': d['code'].upper(),
        'req': d['req'].upper(),
        'desc': d['desc'],
        'tx_id': int(d['tx_id'], 16),
        'dlc': int(d['dlc']),
        'data': d['data'].upper(),
        'max_tx_frames': int(d['max_tx']),
        'expires_ms': int(d['ttl']),
        'tx_build': int(d['tx_build']),
    }


def parse_result_line(line: str):
    """Parse a RESULT line into a dict of key=value tokens, or None."""
    line = line.strip()
    if not line.startswith('RESULT '):
        return None
    out = {}
    for tok in line[len('RESULT '):].split():
        if '=' in tok:
            k, v = tok.split('=', 1)
            out[k] = v
    return out
