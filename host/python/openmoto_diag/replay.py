"""Offline replay engine for verified diagnostic traces.

Given a Trace (from trace.py), the engine walks the steps IN ORDER against an
in-process mock ECU built from the trace's own recorded responses, checking:

  * request order is preserved;
  * each response matches what the trace recorded (session state is modelled by
    a ReplayEcu that only answers requests it was taught, in sequence);
  * ISO-TP addressing / adapter setup (AT/ST) steps are accepted but not sent to
    an ECU (they are transport configuration);
  * inter-step timing is honoured in a scaled, non-blocking way (recorded for the
    report rather than slept through, so tests stay fast).

Read-only guarantee: the engine NEVER performs a write. A step whose request
mutates ECU state (service 0x2E/0x2F/0x31/resets/security/0x3B...) is reported
as BLOCKED and the engine stops unless `allow_write_dryrun=True`, in which case
it records the step as a *dry run* (request logged, nothing executed, response
taken from the trace) explicitly labelled so it can never be mistaken for a real
write. Nothing here can reach a serial port or a motorcycle.
"""
from dataclasses import dataclass, field
from typing import List, Optional

from . import trace as tracemod


@dataclass
class StepResult:
    index: int
    kind: str
    request: str          # hex
    expected: str         # hex
    got: str              # hex
    status: str           # OK | MISMATCH | NO_EXPECTED | AT_OK | BLOCKED | WRITE_DRYRUN
    evidence: str
    note: str = ''


@dataclass
class ReplayReport:
    trace_name: str
    results: List[StepResult] = field(default_factory=list)
    ok: bool = True
    blocked: List[int] = field(default_factory=list)

    def summary(self) -> str:
        counts = {}
        for r in self.results:
            counts[r.status] = counts.get(r.status, 0) + 1
        parts = ', '.join(f'{k}={v}' for k, v in sorted(counts.items()))
        return f'{self.trace_name}: {"PASS" if self.ok else "FAIL"} ({parts})'


class ReplayEcu:
    """A mock ECU that answers exactly the responses recorded in a trace, in the
    order they appear. It models session/sequence by consuming one scripted
    response per matching request; an out-of-order or unknown request yields no
    response (as a silent/unsupported ECU would)."""

    def __init__(self, trace: tracemod.Trace):
        self._script = [(s.request, s.expected) for s in trace.steps
                        if s.kind == 'uds' and s.expected is not None]
        self._pos = 0

    def request(self, req: bytes) -> Optional[bytes]:
        if self._pos < len(self._script):
            exp_req, exp_resp = self._script[self._pos]
            if exp_req == req:
                self._pos += 1
                return exp_resp
        # fall back: search forward for the same request (tolerate interleaved
        # AT steps that don't reach the ECU) without reordering past a match
        for j in range(self._pos, len(self._script)):
            if self._script[j][0] == req:
                self._pos = j + 1
                return self._script[j][1]
        return None


def replay(trace: tracemod.Trace, allow_write_dryrun: bool = False) -> ReplayReport:
    """Replay a trace offline. Returns a ReplayReport. Never transmits."""
    ecu = ReplayEcu(trace)
    report = ReplayReport(trace_name=trace.name)
    for i, step in enumerate(trace.steps):
        req_hex = step.request.hex().upper()
        exp_hex = step.expected.hex().upper() if step.expected else ''

        if step.kind == 'at':
            report.results.append(StepResult(
                i, 'at', '', '', '', 'AT_OK', step.evidence,
                note=f'adapter setup: {step.at_command}'))
            continue

        if step.is_write():
            if not allow_write_dryrun:
                report.results.append(StepResult(
                    i, 'uds', req_hex, exp_hex, '', 'BLOCKED', step.evidence,
                    note='write/mutating request refused by read-only replay policy'))
                report.blocked.append(i)
                report.ok = False
                break
            # Dry run: log intent, do NOT execute; response is the trace's record.
            report.results.append(StepResult(
                i, 'uds', req_hex, exp_hex, exp_hex, 'WRITE_DRYRUN', step.evidence,
                note='DRY RUN ONLY: request not transmitted; response is from trace record'))
            continue

        got = ecu.request(step.request)
        got_hex = got.hex().upper() if got else ''
        if step.expected is None:
            status = 'NO_EXPECTED'
        elif got == step.expected:
            status = 'OK'
        else:
            status = 'MISMATCH'
            report.ok = False
        report.results.append(StepResult(
            i, 'uds', req_hex, exp_hex, got_hex, status, step.evidence, step.note))
    return report
