// OpenMoto Phase 6: ACK-ONLY CAN bring-up, Arduino Mega 2560 glue.
//
// Decision logic lives in ack_core.h (also compiled on the host against a
// simulated controller). This file only provides the hardware layer for CAN1
// (MCP2515 #1, CS D10). CAN2 (#2, CS D9) is held in CONFIG and is physically
// disconnected.
//
// THERE IS NO TRANSMIT PATH IN THIS FIRMWARE. The HAL exposes no "load TX
// buffer" or "request to send" operation. The MCP2515 is driven only between
// CONFIG, LISTEN_ONLY and NORMAL, and in NORMAL it is verified that all three
// TXBnCTRL.TXREQ bits are 0 and the TXnRTS pins are configured as inputs (so an
// external edge cannot trigger a transmission). In NORMAL mode the controller
// still generates CAN ACK bits and error frames in hardware -- that is active
// bus participation, documented in docs/diag-ack-bringup.md.
//
// DO NOT CONNECT TO A MOTORCYCLE until that document's prerequisites are met,
// and never enable ACK-only automatically at boot: the firmware starts in
// LISTEN_ONLY and waits for ARM + CONFIRM.
#include <Arduino.h>
#include <SPI.h>
#include <mcp2515.h>
#include "ack_core.h"

#ifndef OPENMOTO_SPI_HZ
#define OPENMOTO_SPI_HZ 1000000
#endif

namespace {

constexpr uint8_t CS1 = 10, CS2 = 9, INT1_PIN = 2, INT2_PIN = 3;
constexpr uint32_t SPI_HZ = OPENMOTO_SPI_HZ;
constexpr uint32_t BAUD = 115200;

constexpr uint8_t R_CANSTAT = 0x0E, R_CANCTRL = 0x0F;
constexpr uint8_t R_CNF3 = 0x28, R_CNF2 = 0x29, R_CNF1 = 0x2A;
constexpr uint8_t R_TEC = 0x1C, R_REC = 0x1D, R_EFLG = 0x2D;
constexpr uint8_t R_TXB0CTRL = 0x30, R_TXB1CTRL = 0x40, R_TXB2CTRL = 0x50;
constexpr uint8_t R_TXRTSCTRL = 0x0D;
constexpr uint8_t R_RXB0CTRL = 0x60, R_RXB1CTRL = 0x70;
constexpr uint8_t I_WRITE = 0x02, I_READ = 0x03, I_BITMOD = 0x05, I_READ_STATUS = 0xA0;
constexpr uint8_t I_READ_RX0 = 0x90, I_READ_RX1 = 0x94;
constexpr uint8_t MODE_MASK = 0xE0, MODE_NORMAL = 0x00, MODE_LISTEN = 0x60, MODE_CONFIG = 0x80;
constexpr uint8_t TXREQ = 0x08;
constexpr uint8_t EFLG_RX0OVR = 0x40, EFLG_RX1OVR = 0x80, EFLG_TXBO = 0x20,
                  EFLG_TXEP = 0x10, EFLG_RXEP = 0x08;
constexpr uint8_t CNF1_500K = 0x00, CNF2_500K = 0x90, CNF3_500K = 0x82;  // 500 kbps @ 8 MHz

MCP2515 can1(CS1, SPI_HZ, &SPI), can2(CS2, SPI_HZ, &SPI);

// ---- SPI (same direct-port scheme as Phase 3B/4; mapping verified at startup)
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

// The ONLY function that writes CANCTRL.REQOP. Callers pass CONFIG/LISTEN/NORMAL.
bool requestMode(uint8_t mode) {
    bitModify(CS1, R_CANCTRL, MODE_MASK, mode);
    const uint32_t t0 = millis();
    while (millis() - t0 < 50) if (modeOf(CS1) == mode) return true;
    return false;
}

// True iff every transmit buffer has TXREQ clear (nothing queued to send).
bool allTxIdle() {
    return !(readReg(CS1, R_TXB0CTRL) & TXREQ) &&
           !(readReg(CS1, R_TXB1CTRL) & TXREQ) &&
           !(readReg(CS1, R_TXB2CTRL) & TXREQ);
}

// Force the TXnRTS pins to be digital inputs (B2RTS/B1RTS/B0RTS mode bits = 0),
// so an external edge can never request a transmission. Writable in CONFIG.
void disableRtsPins() { writeReg(CS1, R_TXRTSCTRL, 0x00); }

// A tiny SPI integrity self-check: write/read a scratch pattern to a TX data
// register (NOT a control register, so this can never arm a transmission), in
// CONFIG only. Returns true if readback matches.
bool spiSelfCheck() {
    if (modeOf(CS1) != MODE_CONFIG) return true;  // only meaningful in CONFIG
    const uint8_t R_TXB0D0 = 0x36;
    writeReg(CS1, R_TXB0D0, 0x5A);
    const bool ok1 = readReg(CS1, R_TXB0D0) == 0x5A;
    writeReg(CS1, R_TXB0D0, 0xA5);
    const bool ok2 = readReg(CS1, R_TXB0D0) == 0xA5;
    writeReg(CS1, R_TXB0D0, 0x00);
    return ok1 && ok2;
}

// RX frame counter maintained by draining the receive buffers each poll.
uint32_t g_rxCount = 0;
bool g_spiErrorLatched = false;

void drainRx() {
    for (uint8_t tries = 0; tries < 8; ++tries) {
        const uint8_t st = readStatus(CS1);
        const uint8_t instr = (st & 0x01) ? I_READ_RX0 : (st & 0x02) ? I_READ_RX1 : 0;
        if (!instr) return;
        uint8_t b[13];
        csLow(CS1); SPI.transfer(instr);
        for (uint8_t i = 0; i < 13; ++i) b[i] = SPI.transfer(0);
        csHigh(CS1);  // RXnIF cleared by the buffer read
        ++g_rxCount;
    }
}

class ArduinoAckHal : public ackbringup::AckHal {
public:
    uint32_t nowMs() override { return millis(); }
    int readChar() override { return Serial.read(); }
    void putChar(char c) override { Serial.write(uint8_t(c)); }

    bool enterConfig() override {
        bool ok = requestMode(MODE_CONFIG);
        if (ok) { disableRtsPins(); }
        return ok && modeOf(CS1) == MODE_CONFIG;
    }
    bool enterListenOnly() override {
        // Go through CONFIG so we can (re)assert RTS-pin disable and TXREQ clear.
        if (!requestMode(MODE_CONFIG)) return false;
        disableRtsPins();
        if (!allTxIdle()) return false;
        return requestMode(MODE_LISTEN) && modeOf(CS1) == MODE_LISTEN && modeOf(CS2) == MODE_CONFIG;
    }
    bool enterAckOnly() override {
        // NORMAL mode with no TX ever loaded == ACK-only. Verify TX idle and RTS
        // pins disabled in CONFIG first, then enter NORMAL.
        if (!requestMode(MODE_CONFIG)) return false;
        disableRtsPins();
        if (readReg(CS1, R_TXRTSCTRL) & 0x07) return false;  // RTS pin mode bits must be 0
        if (!allTxIdle()) return false;
        if (!requestMode(MODE_NORMAL)) return false;
        return modeOf(CS1) == MODE_NORMAL && allTxIdle();
    }
    bool noTxPending() override { return allTxIdle(); }

    void readStats(ackbringup::BusStats &s) override {
        drainRx();
        const uint8_t eflg = readReg(CS1, R_EFLG);
        const uint8_t tec = readReg(CS1, R_TEC);
        const uint8_t rec = readReg(CS1, R_REC);
        s.rxCount = g_rxCount;
        s.tec = tec;
        s.rec = rec;
        s.eflg = eflg;
        s.errorPassive = (eflg & (EFLG_TXEP | EFLG_RXEP)) != 0 || tec >= 128 || rec >= 128;
        s.busOff = (eflg & EFLG_TXBO) != 0;
        s.mode_normal = modeOf(CS1) == MODE_NORMAL;
        if (!g_spiErrorLatched && !spiSelfCheck()) g_spiErrorLatched = true;
        s.spiError = g_spiErrorLatched;
        // RX overflow is not fatal but worth surfacing; clear the sticky bits.
        if (eflg & (EFLG_RX0OVR | EFLG_RX1OVR)) bitModify(CS1, R_EFLG, EFLG_RX0OVR | EFLG_RX1OVR, 0);
    }
};

ArduinoAckHal hal;
ackbringup::AckShell shell(hal);

bool bitrateOk() {
    return can1.setBitrate(CAN_500KBPS, MCP_8MHZ) == MCP2515::ERROR_OK && readReg(CS1, R_CNF1) == CNF1_500K &&
           readReg(CS1, R_CNF2) == CNF2_500K && readReg(CS1, R_CNF3) == CNF3_500K;
}

bool initialize() {
    if (!csMappingOk()) { Serial.println(F("CS port mapping: FAIL")); return false; }
    const bool c1 = can1.reset() == MCP2515::ERROR_OK && can1.setConfigMode() == MCP2515::ERROR_OK && modeOf(CS1) == MODE_CONFIG;
    const bool c2 = can2.reset() == MCP2515::ERROR_OK && can2.setConfigMode() == MCP2515::ERROR_OK && modeOf(CS2) == MODE_CONFIG;
    Serial.println(c1 ? F("MCP2515 #1 CONFIG: PASS") : F("MCP2515 #1 CONFIG: FAIL"));
    Serial.println(c2 ? F("MCP2515 #2 CONFIG: PASS") : F("MCP2515 #2 CONFIG: FAIL"));
    if (!c1 || !c2) return false;
    disableRtsPins();
    if (!spiSelfCheck()) { Serial.println(F("MCP2515 #1 SPI self-check: FAIL")); return false; }
    Serial.println(F("MCP2515 #1 SPI self-check: PASS"));
    if (!bitrateOk()) { Serial.println(F("MCP2515 #1 bitrate 500k/8MHz: FAIL")); return false; }
    Serial.println(F("MCP2515 #1 bitrate 500k/8MHz: PASS"));
    // Accept-all filters so the passive window sees every frame.
    writeReg(CS1, R_RXB0CTRL, 0x60);  // RXM=11: receive any (filters off)
    writeReg(CS1, R_RXB1CTRL, 0x60);
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
    // Start SILENT. Never auto-enter ACK-only: begin() puts CAN1 in LISTEN_ONLY
    // and waits for ARM + CONFIRM.
    shell.begin();
}

void loop() { shell.poll(); }
