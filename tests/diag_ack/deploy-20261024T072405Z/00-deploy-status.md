# Phase 6 diag_ack deployment status

Timestamp (UTC): 20261024T072405Z
Hardware Mac: MacBook-Pro.local (user sergio, /Users/sergio/src/openmoto)
Arduino: Mega 2560, VID:PID 2341:0042, serial 34530303238351110202
Port: /dev/cu.usbmodem21101
Firmware: diag_ack, HEX sha256 5244f05874093b840a5e00baaaa0d34fd546e0c686af25f09db5de0774bf2453
Flash use: 11372 bytes (4.5%); RAM 1911 bytes (23.3%)

## Results
- Build diag_ack: SUCCESS (01-build.log)
- Firmware audit: PASS (02-fw-audit.log)
- Behaviour tests: PASS (03-tests.log)
- Phase 3B/4/5 HEX unchanged: confirmed (04-preserve-check.log)

## FLASH: NOT PERFORMED — BLOCKED
Reason: operator reported OBD connected to the BMW. Instruction 5 and
docs/diag-ack-bringup.md require the OBD DISCONNECTED before flashing, and
blocker B4 requires power-on reset/SPI-init to occur BEFORE connecting to the
bike. Flashing resets the Mega and floats the SPI/CS lines during the bootloader;
doing so while wired to the vehicle bus is the exact hazard to avoid.
Waiting for operator to disconnect OBD and confirm.

## B1-B4: NOT MEASURED (no bench electrical measurements taken this session)
