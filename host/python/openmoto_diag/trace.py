"""Diagnostic trace model and importers for offline replay.

A *trace* is an ordered list of steps, each a request (bytes the tester sends)
and, optionally, the expected response (bytes the ECU returned) plus timing and
session metadata. Traces come from three evidence-leveled sources:

  * ELM327/STN command logs  -- `parse_elm_log`
  * raw CAN / ISO-TP frame logs -- `parse_can_log`
  * hand-authored JSON transcripts -- `load_json_trace`

This module ONLY parses and models traces. It never transmits. The replay engine
(replay.py) is what drives a trace against the offline mock ECU, and it enforces
the read-only policy: a step whose request mutates ECU state (e.g. a `2E` service
write) is marked non-replayable and the engine refuses to "send" it.

Provenance is first-class. Every step carries an `evidence` label so a trace
built from verified MotoScan findings is never confused with a guessed sequence.
"""
from dataclasses import dataclass, field
import json
import re
from typing import List, Optional

# Evidence levels, mirroring the APK-analysis report vocabulary.
EV_APK = 'APK'            # confirmed from decompiled MotoScan bytecode
EV_PUBLIC = 'PUBLIC'      # documented public behaviour (ELM datasheet, ISO)
EV_INFERRED = 'INFERRED'  # hypothesis / design choice
EV_UNKNOWN = 'UNKNOWN'    # placeholder, must not be transmitted


@dataclass
class Step:
    """One request/response exchange in a trace."""
    request: bytes                       # UDS service bytes (no ISO-TP PCI)
    expected: Optional[bytes] = None     # expected UDS response bytes, if known
    kind: str = 'uds'                    # 'uds' | 'at' | 'raw_can'
    at_command: Optional[str] = None     # for ELM AT/ST setup steps
    delay_ms: int = 0                    # inter-step delay the source used
    evidence: str = EV_INFERRED
    note: str = ''

    def is_write(self) -> bool:
        """True if this request mutates ECU state (service id 0x2E, 0x2F, 0x31,
        resets, security, etc.). Replay refuses to 'send' these."""
        if self.kind != 'uds' or not self.request:
            return False
        return self.request[0] in _MUTATING_SIDS


@dataclass
class Trace:
    name: str
    steps: List[Step] = field(default_factory=list)
    target_hex: Optional[int] = None     # ECU address byte, e.g. 0x60 for KOMBI
    tester_hex: int = 0x6F1
    addressing: str = 'extended'         # 'normal' | 'extended'
    source: str = ''                     # where the trace came from
    evidence: str = EV_INFERRED

    def writes(self) -> List[Step]:
        return [s for s in self.steps if s.is_write()]


# Mutating UDS service ids (must stay consistent with diag_core.h serviceForbidden).
_MUTATING_SIDS = frozenset({
    0x10, 0x11, 0x14, 0x27, 0x28, 0x2A, 0x2C, 0x2E, 0x2F, 0x31, 0x34, 0x35,
    0x36, 0x37, 0x38, 0x3D, 0x3B, 0x83, 0x84, 0x85, 0x86, 0x87,
})


def _hexbytes(s: str) -> bytes:
    cleaned = re.sub(r'[^0-9A-Fa-f]', '', s)
    if len(cleaned) % 2:
        raise ValueError(f'odd-length hex: {s!r}')
    return bytes.fromhex(cleaned)


# --------------------------------------------------------------------------- #
# ELM327 / STN command-log importer
# --------------------------------------------------------------------------- #
# Accepts a simple line log where tester lines are prefixed '>' (sent) and the
# adapter reply follows, e.g.:
#     > ATSPB
#     OK
#     > 22F150
#     62 F1 50 01 02 03
# AT/ST lines become setup steps; hex lines become UDS steps. ELM echoes and the
# ">" prompt are ignored. This is a *transport* importer; the UDS payload of a
# data line is taken verbatim and classified by its first byte.
_SENT_RE = re.compile(r'^\s*>\s*(.*\S)\s*$')


def parse_elm_log(text: str, name='elm', evidence=EV_INFERRED) -> Trace:
    trace = Trace(name=name, source='elm_log', evidence=evidence)
    lines = text.splitlines()
    i = 0
    pending = None  # the step awaiting its response
    while i < len(lines):
        line = lines[i].strip()
        i += 1
        if not line:
            continue
        m = _SENT_RE.match(line)
        if not m:
            # A response line for the pending step.
            if pending is not None and pending.kind == 'uds':
                try:
                    pending.expected = _hexbytes(line)
                except ValueError:
                    pass  # non-hex adapter chatter (OK, SEARCHING, etc.)
            continue
        sent = m.group(1)
        if pending is not None:
            trace.steps.append(pending)
        upper = sent.upper().replace(' ', '')
        if upper.startswith('AT') or upper.startswith('ST'):
            pending = Step(request=b'', kind='at', at_command=sent.strip(),
                           evidence=evidence, note='adapter setup')
        else:
            pending = Step(request=_hexbytes(sent), kind='uds', evidence=evidence)
    if pending is not None:
        trace.steps.append(pending)
    return trace


# --------------------------------------------------------------------------- #
# CAN / ISO-TP frame-log importer
# --------------------------------------------------------------------------- #
# Accepts candump-style lines: "<id> <len> <b0> <b1> ..." or "<id>#<hexdata>".
# Frames addressed to the tester id are requests; frames from the target id are
# responses. Multi-frame ISO-TP (first/consecutive) is reassembled. Extended
# addressing (first data byte = address extension) is handled when `rx_ext`/
# `tx_ext` are provided.
_CANDUMP_RE = re.compile(r'^\s*(?:\(.*?\)\s*\w+\s+)?([0-9A-Fa-f]{3})\s*[#:]?\s*(.*)$')


def _frame_bytes(rest: str) -> bytes:
    """Return the data-byte payload following the CAN id.

    Supports 'ID#AABBCC' (no length token) and 'ID AA BB CC' space-separated
    hex. We do NOT try to guess a leading length token: candump '(ts) can0 ID
    [len] data' style is handled by passing already-split data, and the common
    'ID#data' and 'ID b0 b1 ...' forms carry no separate length field."""
    rest = rest.strip()
    if not rest:
        return b''
    toks = rest.replace('#', ' ').split()
    return _hexbytes(''.join(toks))


def _isotp_reassemble(frames: List[bytes], ext: Optional[int]) -> bytes:
    """Reassemble a list of CAN data payloads (same direction) into the UDS
    message. Handles single (0x0_), first (0x1_) + consecutive (0x2_) frames."""
    out = bytearray()
    total = None
    for fr in frames:
        if ext is not None:
            if not fr or fr[0] != ext:
                continue
            fr = fr[1:]
        if not fr:
            continue
        pci = fr[0] >> 4
        if pci == 0:                         # single frame
            n = fr[0] & 0x0F
            return bytes(fr[1:1 + n])
        if pci == 1:                         # first frame
            total = ((fr[0] & 0x0F) << 8) | fr[1]
            out += fr[2:]
        elif pci == 2:                       # consecutive frame
            out += fr[1:]
        # flow-control (0x3_) frames carry no payload
    if total is not None:
        return bytes(out[:total])
    return bytes(out)


def parse_can_log(text: str, tester_id=0x6F1, target_id=0x60,
                  tx_ext=None, rx_ext=None, name='can', evidence=EV_INFERRED) -> Trace:
    """Build a request/response trace from a raw CAN frame log.

    target_id is the ECU *address byte* (e.g. 0x60). In BMW extended addressing
    the actual response CAN id is 0x600 | target_id and frames carry the tester
    id 0xF1 as an address-extension byte; pass tx_ext/rx_ext to honour that."""
    resp_id = 0x600 | target_id if target_id <= 0xFF else target_id
    trace = Trace(name=name, source='can_log', target_hex=target_id,
                  tester_hex=tester_id, addressing='extended' if (tx_ext or rx_ext) else 'normal',
                  evidence=evidence)
    tx_frames, rx_frames = [], []

    def flush():
        if tx_frames:
            req = _isotp_reassemble(tx_frames, tx_ext)
            step = Step(request=req, kind='uds', evidence=evidence)
            if rx_frames:
                step.expected = _isotp_reassemble(rx_frames, rx_ext)
            trace.steps.append(step)
        tx_frames.clear()
        rx_frames.clear()

    for raw in text.splitlines():
        m = _CANDUMP_RE.match(raw)
        if not m:
            continue
        cid = int(m.group(1), 16)
        data = _frame_bytes(m.group(2))
        if cid == tester_id:
            if rx_frames:     # a new request after a completed response: flush pair
                flush()
            tx_frames.append(data)
        elif cid == resp_id:
            rx_frames.append(data)
    flush()
    return trace


# --------------------------------------------------------------------------- #
# JSON trace (authored / exported)
# --------------------------------------------------------------------------- #
def load_json_trace(path) -> Trace:
    with open(path, 'r') as fh:
        d = json.load(fh)
    steps = []
    for s in d.get('steps', []):
        steps.append(Step(
            request=_hexbytes(s.get('request', '')) if s.get('request') else b'',
            expected=_hexbytes(s['expected']) if s.get('expected') else None,
            kind=s.get('kind', 'uds'),
            at_command=s.get('at_command'),
            delay_ms=int(s.get('delay_ms', 0)),
            evidence=s.get('evidence', EV_INFERRED),
            note=s.get('note', ''),
        ))
    return Trace(
        name=d.get('name', 'json'),
        steps=steps,
        target_hex=d.get('target_hex'),
        tester_hex=d.get('tester_hex', 0x6F1),
        addressing=d.get('addressing', 'extended'),
        source=d.get('source', 'json'),
        evidence=d.get('evidence', EV_INFERRED),
    )


def dump_json_trace(trace: Trace) -> str:
    def hx(b):
        return b.hex().upper() if b else ''
    return json.dumps({
        'name': trace.name,
        'target_hex': trace.target_hex,
        'tester_hex': trace.tester_hex,
        'addressing': trace.addressing,
        'source': trace.source,
        'evidence': trace.evidence,
        'steps': [
            {k: v for k, v in {
                'request': hx(s.request),
                'expected': hx(s.expected) if s.expected else None,
                'kind': s.kind,
                'at_command': s.at_command,
                'delay_ms': s.delay_ms,
                'evidence': s.evidence,
                'note': s.note,
            }.items() if v not in (None, '', 0)}
            for s in trace.steps
        ],
    }, indent=2)
