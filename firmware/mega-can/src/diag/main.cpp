// OpenMoto Phase 4: READ-ONLY diagnostic client, Arduino Mega 2560 glue.
//
// Logic lives in diag_core.h (also compiled on the host against a mock ECU).
// This file only provides the hardware layer: MCP2515 #1 (CS D10) on CAN1 and
// MCP2515 #2 (CS D9) held in CONFIG. All CAN mode changes go through
// requestMode(); the only NORMAL-mode request and the only transmit routine are
// compiled in solely when OPENMOTO_DIAG_ALLOW_TX=1 (env diag_readonly). The
// env diag_dryrun has NO transmit code (verified by tests/check_diag_firmware.py).
//
// DO NOT CONNECT TO A MOTORCYCLE until docs/diag-active.md prerequisites are met.
#include <Arduino.h>
#include <SPI.h>
#include <mcp2515.h>
#include "diag_core.h"

#ifndef OPENMOTO_SPI_HZ
#define OPENMOTO_SPI_HZ 1000000
#endif

namespace {

constexpr uint8_t CS1 = 10, CS2 = 9, INT1_PIN = 2, INT2_PIN = 3;
constexpr uint32_t SPI_HZ = OPENMOTO_SPI_HZ;
constexpr uint32_t BAUD = 115200;

constexpr uint8_t R_CANSTAT = 0x0E, R_CANCTRL = 0x0F;
constexpr uint8_t R_RXF0 = 0x00, R_RXF1 = 0x04, R_RXF2 = 0x08, R_RXF3 = 0x10, R_RXF4 = 0x14, R_RXF5 = 0x18;
constexpr uint8_t R_RXM0 = 0x20, R_RXM1 = 0x24;
constexpr uint8_t R_CNF3 = 0x28, R_CNF2 = 0x29, R_CNF1 = 0x2A;
constexpr uint8_t R_TXB0CTRL = 0x30, R_TXB1CTRL = 0x40, R_TXB2CTRL = 0x50;
constexpr uint8_t R_RXB0CTRL = 0x60, R_RXB1CTRL = 0x70;
constexpr uint8_t I_WRITE = 0x02, I_READ = 0x03, I_BITMOD = 0x05, I_READ_STATUS = 0xA0;
constexpr uint8_t I_READ_RX0 = 0x90, I_READ_RX1 = 0x94;
constexpr uint8_t MODE_MASK = 0xE0, MODE_NORMAL = 0x00, MODE_LISTEN = 0x60, MODE_CONFIG = 0x80;
constexpr uint8_t TXREQ = 0x08, TXERR = 0x10, MLOA = 0x20, ABTF = 0x40;
constexpr uint8_t CANCTRL_OSM = 0x08, CANCTRL_ABAT = 0x10;
constexpr uint8_t CNF1_500K = 0x00, CNF2_500K = 0x90, CNF3_500K = 0x82;  // 500 kbps @ 8 MHz

MCP2515 can1(CS1, SPI_HZ, &SPI), can2(CS2, SPI_HZ, &SPI);

// ---- SPI (same direct-port scheme as Phase 3B; mapping verified at startup) ----
inline void csWrite(uint8_t cs, bool high) {
    volatile uint8_t *port = cs == CS1 ? &PORTB : &PORTH;
    const uint8_t mask = cs == CS1 ? _BV(PB4) : _BV(PH6);
    const uint8_t sreg = SREG;
    cli();
    if (high) *port |= mask; else *port &= uint8_t(~mask);
    SREG = sreg;
}
bool csMappingOk() {
    return portOutputRegister(digitalPinToPort(CS1)) == &PORTB && digitalPinToBitMask(CS1) == _BV(PB4) &&
           portOutputRegister(digitalPinToPort(CS2)) == &PORTH && digitalPinToBitMask(CS2) == _BV(PH6);
}
inline void csLow(uint8_t cs) { SPI.beginTransaction(SPISettings(SPI_HZ, MSBFIRST, SPI_MODE0)); csWrite(cs, false); }
inline void csHigh(uint8_t cs) { csWrite(cs, true); SPI.endTransaction(); }
uint8_t readReg(uint8_t cs, uint8_t a) {
    csLow(cs); SPI.transfer(I_READ); SPI.transfer(a);
    const uint8_t v = SPI.transfer(0);
    csHigh(cs);
    return v;
}
void writeReg(uint8_t cs, uint8_t a, uint8_t v) {
    csLow(cs); SPI.transfer(I_WRITE); SPI.transfer(a); SPI.transfer(v); csHigh(cs);
}
void bitModify(uint8_t cs, uint8_t a, uint8_t mask, uint8_t v) {
    csLow(cs); SPI.transfer(I_BITMOD); SPI.transfer(a); SPI.transfer(mask); SPI.transfer(v); csHigh(cs);
}
uint8_t readStatus(uint8_t cs) {
    csLow(cs); SPI.transfer(I_READ_STATUS);
    const uint8_t v = SPI.transfer(0);
    csHigh(cs);
    return v;
}
uint8_t modeOf(uint8_t cs) { return readReg(cs, R_CANSTAT) & MODE_MASK; }

// The ONLY function that writes CANCTRL.REQOP on CAN1. Audited call sites pass
// MODE_CONFIG / MODE_LISTEN (and MODE_NORMAL only from diagHalEnterNormal).
__attribute__((noinline)) bool requestMode(uint8_t mode) {
    bitModify(CS1, R_CANCTRL, MODE_MASK, mode);
    const uint32_t t0 = millis();
    while (millis() - t0 < 50) {
        if (modeOf(CS1) == mode) return true;
    }
    return false;
}

// Acceptance mask/filter registers (CONFIG mode only): accept exactly one 11-bit ID.
void writeIdReg(uint8_t base, uint16_t id) {
    writeReg(CS1, base, uint8_t(id >> 3));
    writeReg(CS1, base + 1, uint8_t((id & 7) << 5));
    writeReg(CS1, base + 2, 0);
    writeReg(CS1, base + 3, 0);
}
bool idRegIs(uint8_t base, uint16_t id) {
    return readReg(CS1, base) == uint8_t(id >> 3) && readReg(CS1, base + 1) == uint8_t((id & 7) << 5) &&
           readReg(CS1, base + 2) == 0 && readReg(CS1, base + 3) == 0;
}

#if OPENMOTO_DIAG_ALLOW_TX
int8_t firstBuf = -1;  // TX buffer used by the request frame
__attribute__((noinline)) bool diagHalEnterNormal() {
    if (!requestMode(MODE_CONFIG)) return false;
    bitModify(CS1, R_CANCTRL, CANCTRL_OSM, CANCTRL_OSM);  // one-shot: never retransmit
    if (!(readReg(CS1, R_CANCTRL) & CANCTRL_OSM)) return false;
    firstBuf = -1;
    return requestMode(MODE_NORMAL);
}
__attribute__((noinline)) bool diagHalTransmit(const diag::Frame &f) {
    if (f.id == 0 || f.id > 0x7FF || f.dlc > 8) return false;
    if (modeOf(CS1) != MODE_NORMAL) return false;
    int8_t buf = -1;
    if (!(readReg(CS1, R_TXB0CTRL) & TXREQ)) buf = 0;
    else if (!(readReg(CS1, R_TXB1CTRL) & TXREQ)) buf = 1;
    if (buf < 0) return false;
    uint8_t b[13];
    b[0] = uint8_t(f.id >> 3); b[1] = uint8_t((f.id & 7) << 5); b[2] = 0; b[3] = 0; b[4] = f.dlc;
    for (uint8_t i = 0; i < f.dlc; ++i) b[5 + i] = f.data[i];
    csLow(CS1); SPI.transfer(uint8_t(0x40 + 2 * buf));
    for (uint8_t i = 0; i < 5 + f.dlc; ++i) SPI.transfer(b[i]);
    csHigh(CS1);
    csLow(CS1); SPI.transfer(uint8_t(0x81 + buf)); csHigh(CS1);  // RTS, CS raised at once
    if (firstBuf < 0) firstBuf = buf;
    return true;
}
diag::TxState diagHalTxState() {
    if (firstBuf < 0) return diag::TXS_NONE;
    const uint8_t reg = firstBuf == 0 ? R_TXB0CTRL : R_TXB1CTRL;
    uint8_t c = readReg(CS1, reg);
    if (c & TXREQ) return diag::TXS_PENDING;
    if (!(c & (ABTF | MLOA | TXERR))) c = readReg(CS1, reg);  // flags can lag TXREQ clearing
    return (c & (ABTF | MLOA | TXERR)) ? diag::TXS_FAILED : diag::TXS_SENT;
}
#endif

class ArduinoHal : public diag::Hal {
public:
    uint32_t nowMs() override { return millis(); }
    int readChar() override { return Serial.read(); }
    void putChar(char c) override { Serial.write(uint8_t(c)); }

    bool canSilence() override {
        bitModify(CS1, R_CANCTRL, CANCTRL_ABAT, CANCTRL_ABAT);  // abort anything pending
        bool ok = requestMode(MODE_CONFIG);
        bitModify(CS1, R_CANCTRL, CANCTRL_ABAT | CANCTRL_OSM, 0);
        ok = ok && !(readReg(CS1, R_TXB0CTRL) & TXREQ) && !(readReg(CS1, R_TXB1CTRL) & TXREQ) &&
             !(readReg(CS1, R_TXB2CTRL) & TXREQ);
        ok = ok && requestMode(MODE_LISTEN);
        return ok && modeOf(CS1) == MODE_LISTEN && modeOf(CS2) == MODE_CONFIG;
    }
    bool canSetRxId(uint16_t id) override {
        if (!requestMode(MODE_CONFIG)) { requestMode(MODE_LISTEN); return false; }
        writeIdReg(R_RXM0, 0x7FF); writeIdReg(R_RXM1, 0x7FF);
        const uint8_t f[6] = {R_RXF0, R_RXF1, R_RXF2, R_RXF3, R_RXF4, R_RXF5};
        for (uint8_t i = 0; i < 6; ++i) writeIdReg(f[i], id);
        writeReg(CS1, R_RXB0CTRL, 0x04);  // RXM=filters on, rollover to RXB1
        writeReg(CS1, R_RXB1CTRL, 0x00);
        bool ok = idRegIs(R_RXM0, 0x7FF) && idRegIs(R_RXM1, 0x7FF);
        for (uint8_t i = 0; i < 6; ++i) ok = ok && idRegIs(f[i], id);
        return requestMode(MODE_LISTEN) && ok;
    }
#if OPENMOTO_DIAG_ALLOW_TX
    bool canEnterActive() override { return diagHalEnterNormal(); }
    bool canTransmit(const diag::Frame &f) override { return diagHalTransmit(f); }
    diag::TxState canTxState() override { return diagHalTxState(); }
#else
    bool canEnterActive() override { return false; }
    bool canTransmit(const diag::Frame &) override { return false; }
    diag::TxState canTxState() override { return diag::TXS_NONE; }
#endif
    bool canReceive(diag::Frame &f) override {
        for (uint8_t tries = 0; tries < 4; ++tries) {
            const uint8_t st = readStatus(CS1);
            const uint8_t instr = (st & 0x01) ? I_READ_RX0 : (st & 0x02) ? I_READ_RX1 : 0;
            if (!instr) return false;
            uint8_t b[13];
            csLow(CS1); SPI.transfer(instr);
            for (uint8_t i = 0; i < 13; ++i) b[i] = SPI.transfer(0);
            csHigh(CS1);  // RXnIF cleared by the buffer read
            const bool ext = b[1] & 0x08, rtr = b[1] & 0x10;
            if (ext || rtr || (b[4] & 0x0F) > 8) continue;  // consumed, not a std data frame
            f.id = uint16_t(b[0]) << 3 | b[1] >> 5;
            f.dlc = b[4] & 0x0F;
            for (uint8_t i = 0; i < 8; ++i) f.data[i] = i < f.dlc ? b[5 + i] : 0;
            return true;
        }
        return false;
    }
};

ArduinoHal hal;
diag::Shell shell(hal);

bool bitrateOk() {
    return can1.setBitrate(CAN_500KBPS, MCP_8MHZ) == MCP2515::ERROR_OK && readReg(CS1, R_CNF1) == CNF1_500K &&
           readReg(CS1, R_CNF2) == CNF2_500K && readReg(CS1, R_CNF3) == CNF3_500K;
}
// Startup self-check: SPI readback with cross-selection patterns, both chips in CONFIG.
bool initialize() {
    if (!csMappingOk()) { Serial.println(F("CS port mapping: FAIL")); return false; }
    const bool c1 = can1.reset() == MCP2515::ERROR_OK && can1.setConfigMode() == MCP2515::ERROR_OK && modeOf(CS1) == MODE_CONFIG;
    const bool c2 = can2.reset() == MCP2515::ERROR_OK && can2.setConfigMode() == MCP2515::ERROR_OK && modeOf(CS2) == MODE_CONFIG;
    bool s1 = c1, s2 = c2;
    for (uint8_t round = 0; round < 2; ++round) {
        const uint8_t a = round == 0 ? 0x15 : 0x2A, b = round == 0 ? 0x2A : 0x15;
        if (c1) writeReg(CS1, R_CNF1, a);
        if (c2) writeReg(CS2, R_CNF1, b);
        s1 = s1 && readReg(CS1, R_CNF1) == a;
        s2 = s2 && readReg(CS2, R_CNF1) == b;
    }
    Serial.println(s1 ? F("MCP2515 #1 SPI: PASS") : F("MCP2515 #1 SPI: FAIL"));
    Serial.println(s2 ? F("MCP2515 #2 SPI: PASS") : F("MCP2515 #2 SPI: FAIL"));
    if (!s1 || !s2) return false;
    if (!bitrateOk()) { Serial.println(F("MCP2515 #1 bitrate 500k/8MHz: FAIL")); return false; }
    writeReg(CS2, R_CNF1, 0x00);
    return modeOf(CS2) == MODE_CONFIG;
}

}  // namespace

void setup() {
    digitalWrite(53, HIGH); pinMode(53, OUTPUT);
    digitalWrite(CS1, HIGH); digitalWrite(CS2, HIGH);
    pinMode(CS1, OUTPUT); pinMode(CS2, OUTPUT);
    pinMode(INT1_PIN, INPUT_PULLUP); pinMode(INT2_PIN, INPUT_PULLUP);
    Serial.begin(BAUD); SPI.begin(); delay(500);
    if (!initialize()) {
        can1.setConfigMode(); can2.setConfigMode();
        Serial.println(F("INIT: FAIL; controllers set to CONFIG"));
        while (true) delay(1000);
    }
    // Default acceptance until ADDR sets the real response ID: only ID 0x7FF.
    // A failed filter readback halts silently (both controllers in CONFIG).
    if (!hal.canSetRxId(0x7FF)) {
        can1.setConfigMode(); can2.setConfigMode();
        Serial.println(F("INIT: FAIL rx filter readback; controllers set to CONFIG"));
        while (true) delay(1000);
    }
    shell.begin();
}

void loop() { shell.poll(); }
