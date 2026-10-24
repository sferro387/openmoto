#!/usr/bin/env python3
"""Entry point for the OpenMoto read-only diagnostic CLI.

Run `python3 host/python/diag_cli.py --list` to see allowlisted requests, or
`--help` for usage. Real transmission (--port) is hard-gated; see
openmoto_diag/cli.py and docs/diag-active.md.
"""
import sys
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parent))
from openmoto_diag.cli import main  # noqa: E402

if __name__ == '__main__':
    sys.exit(main())
