"""Compile actual Phase 2 source against host-only fault injection stubs."""
from pathlib import Path
import subprocess
import tempfile
import importlib.util

root = Path(__file__).resolve().parents[1]
scenarios = [
    'healthy', 'absent2', 'shared_cs', 'wrong_bitrate', 'normal_fail',
    'tx_api', 'missing', 'duplicate', 'wrong_id', 'extended', 'rtr',
    'dlc', 'payload', 'sequence', 'self_rx', 'eflg', 'tec', 'rec',
    'merrf', 'tx_pending', 'stop_fail',
]
with tempfile.TemporaryDirectory(prefix='openmoto-phase2-') as tmp:
    binary = str(Path(tmp) / 'phase2-mock')
    subprocess.run([
        'clang++', '-std=c++17', '-Wall', '-Wextra', '-Werror',
        '-DOPENMOTO_MCP_CLOCK_MHZ=8', '-I' + str(root / 'tests/phase2_mock'),
        str(root / 'tests/phase2_mock/runner.cpp'), '-o', binary,
    ], check=True)
    for scenario in scenarios:
        subprocess.run([binary, scenario], check=True, timeout=5)

    # Validate capture acceptance against the actual mocked firmware output.
    # None of this output is saved as a physical CAN result.
    spec = importlib.util.spec_from_file_location('capture_phase2', root / 'host/python/capture_phase2.py')
    capture = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(capture)
    text = subprocess.check_output([binary, 'healthy', '--emit'], text=True)
    capture.validate_capture(text)
    corruptions = [
        text.replace('PHASE 2: PASS', 'PHASE 2: FAIL'),
        text.replace('OpenMoto Phase 2', 'OpenMoto INTERNAL LOOPBACK ONLY'),
        text.replace('MCP2515 #2 SPI: PASS', 'MCP2515 #2 SPI: FAIL'),
        text.replace('Frames verified: 1000', 'Frames verified: 999'),
        text.replace('Duplicate frames: 0', 'Duplicate frames: 1'),
        text.replace('TEST: PASS', 'TEST: INCOMPLETE', 1),
        text.replace('MCP2515 #1 stopped: CONFIG', 'Stop missing'),
        text[:text.index('Test D CAN2')],
    ]
    for bad in corruptions:
        try:
            capture.validate_capture(bad)
        except ValueError:
            continue
        raise AssertionError('Capture validator accepted corrupted/incomplete evidence')
    print('HOST CAPTURE VALIDATION PASS: clean mock accepted; 8 corrupt/truncated logs rejected')
