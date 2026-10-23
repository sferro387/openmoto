# Phase 1 hardware

Use ONE 5 V MCP2515 + TJA1050 module and the Mega 2560. Disconnect USB and any
other power before wiring. Leave OBD, motorcycle, CAN H/L, and second module
completely disconnected. Supply the module from Mega 5V; never from OBD power.

| MCP2515 | Mega 2560 |
|---|---|
| VCC | 5V |
| GND | GND |
| SCK | D52 |
| SI / MOSI | D51 |
| SO / MISO | D50 |
| CS | D10 |
| INT | D2 |

D53 is configured OUTPUT/HIGH to preserve SPI master mode. INT is wired but
firmware polls reception; this test does not validate interrupt signaling.

Inspect the metal crystal on the MCP2515 module with power disconnected. Record
its full marking and select the corresponding 8, 16, or 20 MHz build environment.
Do not confuse the Mega's oscillator with the module's crystal. If unclear, stop
and identify it. Internal loopback does not independently verify crystal speed.

Before applying power, explicitly confirm every wire, VCC/GND polarity, absence
of shorts, and that CAN H/L and OBD are disconnected. Await that confirmation
before connecting USB. No termination resistor or second CAN node is needed for
internal loopback. Leave termination hardware alone for this phase.

Crystal marking: `8000`, reported by user as 8 MHz. Select `loopback_8mhz`.
Wiring confirmation: user manually checked every connection listed above on the
ELEGOO Mega 2560 R3 and one MCP2515 + TJA1050 module. CAN-H and CAN-L are
disconnected; OBD cable and motorcycle are not connected.

User requires explicit confirmation before firmware upload, after successful
compilation and USB serial port identification.
