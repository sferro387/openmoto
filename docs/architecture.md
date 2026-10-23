# Architecture

Phase 1 path: Mac USB serial -> Mega Arduino firmware -> hardware SPI -> MCP2515
internal transmit/receive buffers. TJA1050 and physical CAN bus are outside the
loopback path. Only the upstream autowp MCP2515 library and framework SPI are used.

Firmware checks reset, bitrate configuration, and loopback entry; sends one
standard 0x123 eight-byte frame per second with a changing sequence; checks ID,
DLC, all payload bytes, receive timeout, and controller error flags. Any failure
prints FAIL and halts until reset. There is no normal-mode fallback or serial
command that enables bus transmission.

A passing test demonstrates basic MCU execution, serial output, and SPI/controller
loopback operation. It does not prove physical CAN signaling, transceiver health,
interrupt wiring, exact oscillator speed, vehicle bitrate, OBD pinout, diagnostic
protocols, or service reset safety.

Later phases may separate CAN transport, ISO-TP, vehicle-specific diagnostics,
verified service operations, and BLE/mobile transport. Require independent
vehicle-specific evidence and tests before implementing service writes. The
ATmega2560 proof of concept is not the commercial dongle hardware selection.
