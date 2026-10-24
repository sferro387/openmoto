"""Addressing profile loader with a hard 'no unverified addressing' guard.

A profile names the CAN addressing for one or more diagnostic targets. The
loader REFUSES to produce a usable profile unless it is explicitly marked
verified and every required field is present. This is the mechanism that keeps
guessed IDs out of any transmit path: the template ships with verified=false
and null addresses, so it cannot be used until a human fills in documented
values. See host/python/openmoto_diag/profiles/r18-UNVERIFIED.template.json.
"""
from dataclasses import dataclass
from typing import Optional
import json


class ProfileError(Exception):
    pass


@dataclass(frozen=True)
class Target:
    name: str
    description: str
    tx_id: int
    rx_id: int
    tx_ext: Optional[int]
    rx_ext: Optional[int]
    evidence: str


@dataclass(frozen=True)
class AddressingProfile:
    vehicle: str
    bitrate_kbps: int
    addressing_mode: str          # 'normal' or 'extended'
    padding_byte: Optional[int]   # None => no padding; else 0..255
    targets: dict                 # name -> Target

    def addr_command(self, target_name: str) -> str:
        """Build the firmware ADDR command for a target."""
        t = self.require(target_name)
        if self.addressing_mode == 'extended':
            return f'ADDR {t.tx_id:X} {t.rx_id:X} {t.tx_ext:X} {t.rx_ext:X}'
        return f'ADDR {t.tx_id:X} {t.rx_id:X}'

    def pad_command(self) -> str:
        return 'PAD NONE' if self.padding_byte is None else f'PAD {self.padding_byte:02X}'

    def require(self, target_name: str) -> Target:
        if target_name not in self.targets:
            raise ProfileError(f'target {target_name!r} not in profile '
                               f'(have: {", ".join(sorted(self.targets)) or "none"})')
        return self.targets[target_name]


def _req_int(d, key, lo, hi, ctx):
    v = d.get(key)
    if not isinstance(v, int) or isinstance(v, bool) or not (lo <= v <= hi):
        raise ProfileError(f'{ctx}: {key!r} must be an integer in [{lo:#x}, {hi:#x}], got {v!r}')
    return v


def load_profile(path) -> AddressingProfile:
    """Load and strictly validate an addressing profile. Raises ProfileError if
    the profile is unverified, incomplete, or contains an obviously unsafe id."""
    with open(path, 'r') as fh:
        data = json.load(fh)

    if data.get('schema') != 'openmoto-diag-addressing/1':
        raise ProfileError('not an openmoto-diag-addressing/1 profile')
    if data.get('verified') is not True:
        raise ProfileError(
            "profile is not verified (verified != true). Addressing must be "
            "confirmed from a documented source or measurement and the file "
            "marked verified=true before it can be used. Refusing to proceed.")

    bitrate = _req_int(data, 'bitrate_kbps', 1, 1000, 'profile')
    mode = data.get('addressing_mode')
    if mode not in ('normal', 'extended'):
        raise ProfileError("addressing_mode must be 'normal' or 'extended'")

    pad = data.get('padding_byte', None)
    if pad is not None:
        pad = _req_int(data, 'padding_byte', 0x00, 0xFF, 'profile')

    raw_targets = data.get('targets') or {}
    targets = {}
    for name, td in raw_targets.items():
        if name.startswith('_'):
            continue  # template example / comment entries
        ctx = f'target {name!r}'
        tx = _req_int(td, 'tx_id', 0x001, 0x7FF, ctx)
        rx = _req_int(td, 'rx_id', 0x001, 0x7FF, ctx)
        # Reject obviously unsafe / nonsensical addressing up front.
        if tx == rx:
            raise ProfileError(f'{ctx}: tx_id == rx_id')
        if 0x7DF in (tx,):
            raise ProfileError(f'{ctx}: tx_id 0x7DF is the functional broadcast; '
                               'read-only point-to-point addressing is required')
        tx_ext = rx_ext = None
        if mode == 'extended':
            tx_ext = _req_int(td, 'tx_ext', 0x00, 0xFF, ctx)
            rx_ext = _req_int(td, 'rx_ext', 0x00, 0xFF, ctx)
        evidence = td.get('evidence')
        if not isinstance(evidence, str) or not evidence.strip():
            raise ProfileError(f'{ctx}: a non-empty "evidence" string is required '
                               '(cite the source/measurement for these ids)')
        targets[name] = Target(name, td.get('description') or '', tx, rx, tx_ext, rx_ext, evidence)

    if not targets:
        raise ProfileError('profile has no usable targets')

    return AddressingProfile(
        vehicle=str(data.get('vehicle', 'unknown')),
        bitrate_kbps=bitrate,
        addressing_mode=mode,
        padding_byte=pad,
        targets=targets,
    )
