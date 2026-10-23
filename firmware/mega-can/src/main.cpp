#include <Arduino.h>
#include <SPI.h>
#include <mcp2515.h>
#include <string.h>

#if OPENMOTO_MCP_CLOCK_MHZ == 8
constexpr CAN_CLOCK kClock = MCP_8MHZ;
#elif OPENMOTO_MCP_CLOCK_MHZ == 16
constexpr CAN_CLOCK kClock = MCP_16MHZ;
#elif OPENMOTO_MCP_CLOCK_MHZ == 20
constexpr CAN_CLOCK kClock = MCP_20MHZ;
#else
#error "Select a confirmed MCP2515 crystal frequency"
#endif

constexpr uint8_t kCs = 10;
MCP2515 controller(kCs);
uint32_t sequence = 0;
uint32_t passed = 0;

void fail(const __FlashStringHelper *stage, int code) {
    Serial.print(F("FAIL stage=")); Serial.print(stage);
    Serial.print(F(" code=")); Serial.println(code);
    // Stop permanently. Never fall back to normal mode.
    while (true) { delay(1000); }
}

void requireOk(MCP2515::ERROR result, const __FlashStringHelper *stage) {
    if (result != MCP2515::ERROR_OK) fail(stage, result);
}

void setup() {
    pinMode(53, OUTPUT); // Mega hardware SS must remain an output.
    digitalWrite(53, HIGH);
    pinMode(kCs, OUTPUT);
    digitalWrite(kCs, HIGH);
    pinMode(2, INPUT_PULLUP); // INT wired for later use; polling in Phase 1.
    Serial.begin(115200);
    SPI.begin();
    delay(500);
    Serial.println(F("OpenMoto INTERNAL LOOPBACK ONLY"));
    Serial.print(F("MCP crystal MHz=")); Serial.println(OPENMOTO_MCP_CLOCK_MHZ);
    requireOk(controller.reset(), F("reset"));
    requireOk(controller.setBitrate(CAN_500KBPS, kClock), F("bitrate"));
    requireOk(controller.setLoopbackMode(), F("loopback"));
    Serial.println(F("READY loopback nominal_bitrate=500000"));
}

void loop() {
    can_frame tx = {};
    tx.can_id = 0x123;
    tx.can_dlc = 8;
    tx.data[0] = 0x4F; tx.data[1] = 0x4D;
    for (uint8_t i = 0; i < 4; ++i) tx.data[2 + i] = (sequence >> (8 * i)) & 0xFF;
    tx.data[6] = 0xA5; tx.data[7] = 0x5A;
    requireOk(controller.sendMessage(&tx), F("send"));
    can_frame rx = {};
    const uint32_t start = millis();
    MCP2515::ERROR result;
    do {
        result = controller.readMessage(&rx);
        if (result == MCP2515::ERROR_OK) break;
        if (result != MCP2515::ERROR_NOMSG) fail(F("receive"), result);
        if (millis() - start >= 250) fail(F("timeout"), result);
        delay(1);
    } while (true);
    if (rx.can_id != tx.can_id || rx.can_dlc != tx.can_dlc ||
        memcmp(rx.data, tx.data, tx.can_dlc) != 0) fail(F("frame_mismatch"), -1);
    const uint8_t errors = controller.getErrorFlags();
    if (errors != 0) fail(F("error_flags"), errors);
    ++passed;
    Serial.print(F("PASS seq=")); Serial.print(sequence);
    Serial.print(F(" id=0x123 dlc=8 matched=8 total=")); Serial.println(passed);
    ++sequence;
    delay(1000);
}
