# Setup and bring-up

## Inspection and decisions (2026-10-19)

Workspace: /Users/sergio/Downloads/openmoto, newly created, Git initialized.
Existing bmw-diagnostic-client, bmw-apk-comparison, MotoScan, and Telegram Lite
directories were left unchanged. No applicable ancestor AGENTS.md was found.

Observed macOS 27.0.1 (26A434), arm64; Homebrew 7.0.9; Python 3.9.6 at
/usr/bin/python3; Git 2.54.0 (Apple Git-157); Command Line Tools at
/Library/Developer/CommandLineTools. Neither pio/platformio on PATH nor the default
~/.platformio/penv/bin/pio was found. Serial devices were only
/dev/cu.Bluetooth-Incoming-Port and /dev/cu.debug-console. No board detected.

Reuse working Python and Git rather than modify system installations. Host venv:
host/python/.venv. Separate tooling venv: .tools/pio-venv. Both were created with
python3 -m venv. No sudo, GUI, drivers, shell profile edits, or Homebrew changes.

Official installer download failed with DNS resolution errors for
raw.githubusercontent.com; git ls-remote likewise failed for github.com.
Use the officially documented pip method in the isolated tooling environment.
Attempted PlatformIO 6.1.18 installation failed: no matching distribution visible.
An explicit PyPI retry also failed (see current session output). At that point PlatformIO was
not installed; see the latest progress record below for current status.

Selected stable pins: PlatformIO Core 6.1.18, atmelavr 5.1.0, upstream
arduino-mcp2515 v1.3.1 at commit 49e0d7d6755bab09cc3e68997bfdb75264c8daf7.
Compiler, Arduino framework and SCons package versions are also pinned.
No unnecessary application libraries. Project-local PlatformIO core_dir avoids
writing tool packages into the user's home. No default oscillator assumption.

## Reproduce installation

Run from the repository root:

```sh
.tools/pio-venv/bin/python -m pip install --index-url https://pypi.org/simple platformio==6.1.18
./scripts/pio --version
source .tools/pio-venv/bin/activate
pio --version
```

The ./scripts/pio launcher sets a repository-local PLATFORMIO_CORE_DIR before
Core starts. Use it for all commands. Activation also makes bare pio available,
but bare pio can use home-directory state before reading platformio.ini.
For a fresh clone, first run `python3 -m venv .tools/pio-venv` and
`python3 -m venv host/python/.venv`. Host activation is optional via
`source host/python/.venv/bin/activate`. Install the pinned serial capture dependency:
`host/python/.venv/bin/python -m pip install -r host/python/requirements.txt`.

## Build before powering hardware

All environments can be compile-checked with `./scripts/pio run -d firmware/mega-can`.
Before upload, inspect the crystal and choose exactly the matching environment. Example ONLY
for confirmed 8 MHz:

```sh
./scripts/pio run -d firmware/mega-can -e loopback_8mhz
```

Use loopback_16mhz or loopback_20mhz for those markings. A successful build is
required before upload. The configured 500 kbit/s is only a loopback test setting,
not a claim about any target motorcycle.

## Power, upload, verify

1. Complete the unpowered wiring checklist in hardware.md and explicitly confirm
   it with the engineer BEFORE connecting USB or other power.
2. After confirmation, connect USB and list devices:
   `./scripts/pio device list`. Identify the newly appearing USB serial
   port; never select Bluetooth or debug-console. If absent, inspect cable/USB
   enumeration before considering any driver installation.
3. Upload with the selected environment and identified port, for example:
   `./scripts/pio run -d firmware/mega-can -e loopback_8mhz -t upload --upload-port /dev/cu.REPLACE`.
4. Monitor: `./scripts/pio device monitor -p /dev/cu.REPLACE -b 115200`.
   Close other serial clients. If startup output was missed, reset the Mega.
5. Require READY and at least 20 consecutive PASS lines with increasing sequence
   and no FAIL. Save actual output and package inventory in tests/ when observed.
   Exit monitor with Ctrl-C. Upload success alone is not a loopback pass.

Expected output (illustrative, NOT an observed result):

```text
OpenMoto INTERNAL LOOPBACK ONLY
MCP crystal MHz=8
READY loopback nominal_bitrate=500000
PASS seq=0 id=0x123 dlc=8 matched=8 total=1
PASS seq=1 id=0x123 dlc=8 matched=8 total=2
```

For FAIL reset/bitrate/loopback: remove power and check VCC/GND, CS, and SPI wiring.
For timeout or mismatch: check the module, wiring, and selected crystal setting.
For garbage serial: confirm 115200. Never switch to normal CAN mode to debug this
phase. Loopback cannot establish the actual crystal frequency.

## Official references

- https://docs.platformio.org/en/latest/core/installation/methods/pypi.html
- https://docs.platformio.org/en/latest/core/installation/methods/installer-script.html
- https://github.com/autowp/arduino-mcp2515/tree/v1.3.1
- https://github.com/platformio/platform-atmelavr/tree/v5.1.0

## Retry on 2026-10-22

Retried the pinned PlatformIO pip install using explicit official PyPI URL. It
failed again; curl could not resolve either pypi.org or github.com, and scutil
reported no DNS configuration available in this execution environment. No system
network settings were changed. The session still has restricted shell networking.

A new /dev/cu.usbmodem21101 device is visible. Its identity is not confirmed. It
was not opened, reset, monitored, or flashed; wiring and crystal confirmation are
still pending. PlatformIO installation, compilation, upload, and physical loopback
verification remain incomplete.

If the agent terminal remains restricted, run the installation command above in
a normal macOS Terminal from /Users/sergio/Downloads/openmoto, then report the
result so work can resume. No sudo or global package installation is needed.

## Network restored and tool installation (2026-10-22)

PyPI HTTPS now succeeds. Installed PlatformIO 6.1.18 in the project-local tooling
venv through official PyPI; CLI works and pip check reports no broken requirements.
The separately installed /Users/sergio/platformio-venv contains Core 6.2.0; it was
left unchanged. requirements-tooling.lock.txt captures the local Python packages;
use `python -m pip install -r requirements-tooling.lock.txt` inside the tooling
venv to reproduce that snapshot on compatible Python/macOS.

Added scripts/pio to set the cache path early: platformio.ini alone did not
prevent Core startup from trying ~/.platformio. All tool packages remain local.
Python 3.9.6 uses LibreSSL 2.8.3, producing urllib3 v2's NotOpenSSLWarning.
Network downloads worked despite the warning; this is a known environment
limitation, not suppressed. A future supported OpenSSL-based Python can replace
the venv without changing system Python.

Downloaded atmelavr 5.1.0, toolchain-atmelavr 1.70300.191015 (GCC 7.3.0),
framework-arduino-avr 5.2.0, tool-scons 4.40801.0, and autowp MCP2515 1.3.1.
Resolved v1.3.1 with git ls-remote and pinned its full commit in platformio.ini.
First build of all three environments failed with “Bad CPU type in executable”;
file identifies avr-g++ as Mach-O x86_64. Rosetta installation authorized by user;
Apple's license prompt is awaiting explicit license acceptance. No sudo used.

Read-only serial enumeration reports /dev/cu.usbmodem21101 with VID:PID 2341:0042,
serial 34530303238351110202. Port not opened or flashed. Crystal and unpowered
wiring confirmation remain pending.

## Hardware confirmed; Rosetta license review pending

User reports crystal marking 8000 (8 MHz), confirms all seven documented wires
were manually checked, and confirms CAN-H/L, OBD cable, and motorcycle are
disconnected. Use loopback_8mhz for the actual board.

User explicitly instructed the engineer not to enter A at Apple's license
prompt. Rosetta installation is paused for the user to review and accept the
license themselves. After the user confirms installation, resume compilation
with ./scripts/pio and pinned dependencies. After build success, identify the
USB serial port and request explicit confirmation BEFORE uploading firmware.

## Successful 8 MHz compilation after Rosetta installation

User confirmed completing Rosetta installation themselves. Built successfully
using `./scripts/pio run -d firmware/mega-can -e loopback_8mhz` with pinned
dependencies. Flash: 5932 / 253952 bytes; static RAM: 239 / 8192 bytes.
HEX SHA-256: 1f4bb0f521066036af00cf34ec3e4ece92ccaf7de29146d6328cbe2fb0cbbff6.

Fresh USB enumeration identifies /dev/cu.usbmodem21101, VID:PID 2341:0042, serial
34530303238351110202. No upload performed. Explicit upload confirmation required.
Python LibreSSL compatibility warning remains; build exited successfully.

## Phase 1 accepted: upload and hardware loopback

User explicitly authorized upload. First attempt was blocked by sandbox serial
permissions. After approved serial access, the wrapper uploaded to
/dev/cu.usbmodem21101 with the wiring protocol. avrdude reported ATmega2560
signature 0x1e9801, wrote 5932 bytes, and verified all 5932 bytes successfully.
Upload completed with exit status 0. Added the observed uploader version
platformio/tool-avrdude@1.60300.200527 to explicit package pins.

Captured real serial output at 115200 using host/python/capture_loopback.py.
Opening the port and toggling DTR reset the Mega. The script required READY,
20 consecutive matching PASS records, and no FAIL; it exited 0. Observed crystal
setting was 8 MHz; frame sequences 0–19, ID 0x123, DLC 8, all eight bytes matched.
Raw evidence: tests/loopback-8mhz-verified-serial.log. Acceptance summary and
artifact hashes: tests/phase1-result.json.

The capture script uses pyserial 3.5 (already in the tooling venv; also installed
in the separate host venv via its pinned requirements.txt). The initial capture
used the tooling Python. To capture again using the host environment, choose a
NEW output filename (the script refuses to overwrite existing evidence):

```sh
host/python/.venv/bin/python host/python/capture_loopback.py --port /dev/cu.usbmodem21101 --output tests/loopback-repeat.log
```

This command intentionally resets the board. Hardware access in a restricted
agent session may need execution approval. No physical CAN, TJA1050 transceiver,
interrupt wiring, OBD/vehicle protocol, or service reset has been validated.
The firmware remains loopback-only and continues running after capture closes.
Disconnect USB before changing any wiring.
