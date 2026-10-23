// OpenMoto Phase 3A: CAN1 LISTEN_ONLY sniffer.
//
// CAN1 (CS D10, INT D2) never transmits: this file contains no transmit path
// for CAN1, and its mode is verified as LISTEN_ONLY before capture and
// periodically afterwards. CAN2 (CS D9) is held silent in CONFIG, except in the
// separately built bench variant (OPENMOTO_SNIFFER_BENCH=1), where the BENCH
// command uses CAN2 in NORMAL mode as a deterministic test-traffic source.
// See docs/phase3a.md for the output format, bandwidth limits and test plan.
#include <Arduino.h>
#include <SPI.h>
#include <mcp2515.h>
#include <ctype.h>
#include <string.h>

#if OPENMOTO_MCP_CLOCK_MHZ != 8
#error "Phase 3A requires the confirmed 8 MHz module crystals"
#endif
#ifndef OPENMOTO_SNIFFER_BENCH
#define OPENMOTO_SNIFFER_BENCH 0
#endif
#ifndef OPENMOTO_SNIFFER_BAUD
#define OPENMOTO_SNIFFER_BAUD 115200
#endif
#if SERIAL_TX_BUFFER_SIZE < 128
#error "Phase 3A expects -DSERIAL_TX_BUFFER_SIZE=256 (see platformio.ini)"
#endif

namespace {
constexpr uint8_t CS1 = 10, CS2 = 9, INT1_PIN = 2, INT2_PIN = 3;
constexpr uint32_t SPI_HZ = 1000000;          // Same conservative breadboard SPI as Phase 2.
constexpr uint32_t HEALTH_MS = 100, STATS_MS = 5000;

// MCP2515 registers, instructions and bits (DS21801). Raw access is limited to
// RX buffer reads, flag handling, readback checks and the bench-only CAN2 TX.
constexpr uint8_t R_CANSTAT = 0x0E, R_CANCTRL = 0x0F, R_TEC = 0x1C, R_REC = 0x1D;
constexpr uint8_t R_RXM0SIDH = 0x20, R_CNF3 = 0x28, R_CNF2 = 0x29, R_CNF1 = 0x2A;
constexpr uint8_t R_CANINTE = 0x2B, R_CANINTF = 0x2C, R_EFLG = 0x2D;
constexpr uint8_t R_TXB0CTRL = 0x30, R_TXB1CTRL = 0x40, R_TXB2CTRL = 0x50;
constexpr uint8_t R_RXB0CTRL = 0x60, R_RXB1CTRL = 0x70;
constexpr uint8_t I_WRITE = 0x02, I_READ = 0x03, I_BITMOD = 0x05;
constexpr uint8_t I_READ_RX0 = 0x90, I_READ_RX1 = 0x94;
constexpr uint8_t MODE_MASK = 0xE0, MODE_NORMAL = 0x00, MODE_LISTEN = 0x60, MODE_CONFIG = 0x80;
constexpr uint8_t IF_RX0 = 0x01, IF_RX1 = 0x02, IF_ERR = 0x20, IF_MERR = 0x80;
constexpr uint8_t IF_SERVICED = IF_RX0 | IF_RX1 | IF_ERR | IF_MERR; // == CANINTE after reset()
constexpr uint8_t EF_RX0OVR = 0x40, EF_RX1OVR = 0x80;
constexpr uint8_t TXREQ = 0x08;

MCP2515 can1(CS1, SPI_HZ, &SPI), can2(CS2, SPI_HZ, &SPI);

// Chip select by direct port write (Mega 2560: D10 = PB4, D9 = PH6), verified
// against the Arduino pin tables at startup. digitalWrite() costs ~3.5 us per
// call, which matters on the per-frame receive path. PORTH is not bit-
// addressable, so the read-modify-write is done with interrupts disabled.
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
void csLow(uint8_t cs) {
    SPI.beginTransaction(SPISettings(SPI_HZ, MSBFIRST, SPI_MODE0));
    csWrite(cs, false);
}
void csHigh(uint8_t cs) {
    csWrite(cs, true);
    SPI.endTransaction();
}
uint8_t readStatus(uint8_t cs) {  // READ STATUS: bit0 RX0IF, bit1 RX1IF
    csLow(cs);
    SPI.transfer(0xA0);
    const uint8_t value = SPI.transfer(0);
    csHigh(cs);
    return value;
}
uint8_t readReg(uint8_t cs, uint8_t address) {
    csLow(cs);
    SPI.transfer(I_READ); SPI.transfer(address);
    const uint8_t value = SPI.transfer(0);
    csHigh(cs);
    return value;
}
void writeReg(uint8_t cs, uint8_t address, uint8_t value) {
    csLow(cs);
    SPI.transfer(I_WRITE); SPI.transfer(address); SPI.transfer(value);
    csHigh(cs);
}
void bitModify(uint8_t cs, uint8_t address, uint8_t mask, uint8_t value) {
    csLow(cs);
    SPI.transfer(I_BITMOD); SPI.transfer(address); SPI.transfer(mask); SPI.transfer(value);
    csHigh(cs);
}
uint8_t mode(uint8_t cs) { return readReg(cs, R_CANSTAT) & MODE_MASK; }

struct Frame {
    uint32_t ms, id;
    uint8_t dlc;           // Raw DLC field (0..15); payload is min(dlc, 8) bytes.
    bool ext, rtr;
    uint8_t data[8];
};
uint8_t payloadLen(const Frame &f) { return f.rtr ? 0 : (f.dlc > 8 ? 8 : f.dlc); }

struct Counters {
    uint32_t rx, std, ext, rtr, dlcOver8;
    uint32_t printed, serialDrop;  // serialDrop: received and counted; output queue full, not printed.
    uint32_t hwOverrun;            // RXnOVR events; each means >=1 frame lost in the controller.
    uint32_t merr, errif;          // MERRF / ERRIF interrupt events.
    uint32_t intMissed, modeFaults;
    uint8_t eflgOr, peakTec, peakRec;
};
Counters c;
volatile uint32_t intEdges = 0;
bool snifferEnabled = false, capturing = false, testRunning = false;
#if OPENMOTO_SNIFFER_BENCH
bool benchRunning = false;  // BENCH frame verification active
#endif
uint8_t msgDepth = 0;  // >0 while a multi-part status message is being written.
uint32_t lastHealth = 0, lastStats = 0, lastStatsRx = 0;

void serviceCan1();
bool bitrateOk(MCP2515 &can, uint8_t cs);
// Status text must not interleave with frame lines and must not stall CAN
// reception. It waits for serial TX space while continuing to drain CAN1;
// frames read meanwhile stay in the output queue until the message ends.
struct ReliableOut : public Print {
    size_t write(uint8_t b) override {
        while (Serial.availableForWrite() <= 0) serviceCan1();
        return Serial.write(b);
    }
    using Print::write;
};
ReliableOut out;
struct Msg {
    Msg() { ++msgDepth; }
    ~Msg() { --msgDepth; }
};

void onCan1Int() { ++intEdges; } // ISR: no SPI here; main loop owns the bus.
uint32_t readIntEdges() {
    noInterrupts(); const uint32_t v = intEdges; interrupts(); return v;
}

// ---- Frame output -------------------------------------------------------
// Division-free decimal: 32-bit division is a slow software routine on AVR
// (~40 us per digit); measured printing throughput was limited by it.
char *putDec(char *p, uint32_t v) {
    static const uint32_t POW10[] = {1000000000UL, 100000000UL, 10000000UL, 1000000UL,
                                     100000UL, 10000UL, 1000UL, 100UL, 10UL};
    bool started = false;
    for (uint8_t i = 0; i < sizeof(POW10) / sizeof(POW10[0]); ++i) {
        uint8_t digit = 0;
        while (v >= POW10[i]) { v -= POW10[i]; ++digit; }
        if (digit || started) { *p++ = char('0' + digit); started = true; }
    }
    *p++ = char('0' + v);
    return p;
}
char *putHex(char *p, uint32_t v, uint8_t digits) {
    static const char hex[] = "0123456789ABCDEF";
    for (int8_t shift = int8_t((digits - 1) * 4); shift >= 0; shift -= 4) *p++ = hex[(v >> shift) & 0xF];
    return p;
}
// "F <ms> <id> <S|E> <dlc>[ <payload hex>][ R]\n"; max 43 bytes.
constexpr uint8_t MAX_LINE = 43;
uint8_t formatFrame(char *line, const Frame &f) {
    char *p = line;
    *p++ = 'F'; *p++ = ' ';
    p = putDec(p, f.ms); *p++ = ' ';
    p = putHex(p, f.id, f.ext ? 8 : 3); *p++ = ' ';
    *p++ = f.ext ? 'E' : 'S'; *p++ = ' ';
    p = putDec(p, f.dlc);
    const uint8_t n = payloadLen(f);
    if (n) { *p++ = ' '; for (uint8_t i = 0; i < n; ++i) p = putHex(p, f.data[i], 2); }
    if (f.rtr) { *p++ = ' '; *p++ = 'R'; }
    *p++ = '\n';
    return uint8_t(p - line);
}
bool printFrames = true;  // FLOOD count-only phase disables printing.
// Output queue: the receive path only copies frames here (no formatting or
// serial work), so CAN1's two hardware buffers are emptied as fast as SPI
// allows. Lines are formatted later by flushOutput() from the main loop.
// Measured on hardware: formatting in the receive path let the RX buffers
// overrun on back-to-back short frames (see docs/phase3a.md).
constexpr uint8_t QUEUE_SIZE = 64;  // power of two
Frame queue[QUEUE_SIZE];
uint8_t qHead = 0, qTail = 0, qCount = 0, qPeak = 0;
void emitFrame(const Frame &f) {
    if (!printFrames) return;
    if (qCount == QUEUE_SIZE) { ++c.serialDrop; return; }  // Output backlog full: counted drop.
    queue[qHead] = f;
    qHead = uint8_t((qHead + 1) & (QUEUE_SIZE - 1));
    if (++qCount > qPeak) qPeak = qCount;
}
// Print at most one queued line, only if it fits without blocking. Never runs
// inside a status message, so frame and status lines cannot interleave.
void flushOutput() {
    if (!qCount || msgDepth || Serial.availableForWrite() < int(MAX_LINE)) return;
    char line[MAX_LINE + 1];
    const uint8_t n = formatFrame(line, queue[qTail]);
    qTail = uint8_t((qTail + 1) & (QUEUE_SIZE - 1));
    --qCount;
    Serial.write(reinterpret_cast<const uint8_t *>(line), n);
    ++c.printed;
}
#if OPENMOTO_SNIFFER_BENCH
bool outputIdle() { return !qCount && Serial.availableForWrite() >= SERIAL_TX_BUFFER_SIZE - 1; }
void serviceAll() { serviceCan1(); flushOutput(); }
// Wait (servicing CAN1 and output) until queued lines are printed, bounded.
void drainOutput(uint32_t maxMs) {
    const uint32_t start = millis();
    while (!outputIdle() && millis() - start < maxMs) serviceAll();
}
#endif

// ---- Bench (CAN2 test traffic) -----------------------------------------
#if OPENMOTO_SNIFFER_BENCH
struct BenchSpec { uint32_t id; bool ext, rtr; uint8_t dlc; };
const BenchSpec SPECS[] = {
    {0x123, false, false, 8}, {0x18DAF110, true, false, 8}, {0x7FF, false, false, 0},
    {0x1FFFFFFF, true, false, 3}, {0x321, false, true, 4}, {0x0ABCDEF, true, true, 0},
    {0x000, false, false, 1}, {0x456, false, false, 8},
};
constexpr uint8_t SPEC_COUNT = sizeof(SPECS) / sizeof(SPECS[0]);
constexpr uint8_t BENCH_ROUNDS = 25;
constexpr uint16_t BENCH_TOTAL = uint16_t(BENCH_ROUNDS) * SPEC_COUNT;
constexpr uint32_t BENCH_TIMEOUT_MS = 100, BENCH_QUIET_MS = 5, BENCH_DRAIN_MS = 1000;

struct Bench {
    Frame expected;
    uint16_t copies;
    bool abortRequested;
    uint32_t queuedUs, firstUs, secondUs;  // micros() at RTS / first / second verified copy
    uint32_t matched, missing, unexpected, timeouts, copiesTotal, maxCopies;
    uint32_t hist[3];  // frames seen 1x, 2x, >=3x
    uint32_t firstLatencyUs;                // frame 0: RTS -> first copy (CAN2 starts at TEC 0)
    uint32_t latMinUs, latMaxUs;            // frames 1..N-1: RTS -> first copy
    uint32_t recopyMinUs, recopyMaxUs, recopyFrames;  // first -> second copy
    uint8_t can2TecAfterFirst;
    uint8_t can2Tec, can2Rec, can2PeakTec, can2PeakRec, can2EflgOr, can2IntfOr;
    bool safetyStop;
};
Bench bench;

Frame benchFrame(uint8_t round, uint8_t index, uint16_t seq) {
    const BenchSpec &s = SPECS[index];
    Frame f = {};
    f.id = s.id; f.ext = s.ext; f.rtr = s.rtr; f.dlc = s.dlc;
    const uint8_t pattern[8] = {0x4F, 0x4D, round, index, uint8_t(seq), uint8_t(seq >> 8), 0xA5, 0x5A};
    if (!f.rtr) memcpy(f.data, pattern, payloadLen(f));
    return f;
}
void can2RequestAbort() { bitModify(CS2, R_CANCTRL, 0x10, 0x10); }  // ABAT
void benchCheck(const Frame &f) {
    const Frame &e = bench.expected;
    const uint8_t n = payloadLen(f);
    if (!(f.id == e.id && f.ext == e.ext && f.rtr == e.rtr && f.dlc == e.dlc &&
          memcmp(f.data, e.data, n) == 0)) { ++bench.unexpected; return; }
    const uint32_t now = micros();
    if (++bench.copies == 1) {
        bench.firstUs = now;
    } else if (bench.copies == 2) {
        bench.secondUs = now;
    }
}
// Transmit path exists ONLY for CAN2 (CS2 is hard-coded).
void can2Queue(const Frame &f) {
    uint8_t b[13];
    if (f.ext) {
        const uint32_t sid = f.id >> 18;
        b[0] = uint8_t(sid >> 3);
        b[1] = uint8_t(((sid & 7) << 5) | 0x08 | ((f.id >> 16) & 3));
        b[2] = uint8_t(f.id >> 8); b[3] = uint8_t(f.id);
    } else {
        b[0] = uint8_t(f.id >> 3); b[1] = uint8_t((f.id & 7) << 5); b[2] = 0; b[3] = 0;
    }
    b[4] = uint8_t(f.dlc | (f.rtr ? 0x40 : 0));
    const uint8_t n = payloadLen(f);
    memcpy(b + 5, f.data, n);
    csLow(CS2); SPI.transfer(0x40);                           // LOAD TX BUFFER, TXB0SIDH
    for (uint8_t i = 0; i < 5 + n; ++i) SPI.transfer(b[i]);
    csHigh(CS2);
    csLow(CS2); SPI.transfer(0x81); csHigh(CS2);              // RTS TXB0
}
// Complete an abort: request ABAT (idempotent), wait for TXREQ to clear while
// still draining CAN1, then clear ABAT (it must be cleared before the next RTS).
bool can2Abort() {
    can2RequestAbort();
    const uint32_t start = millis();
    while ((readReg(CS2, R_TXB0CTRL) & TXREQ) && millis() - start < 10) serviceCan1();
    const bool idle = !(readReg(CS2, R_TXB0CTRL) & TXREQ);
    bitModify(CS2, R_CANCTRL, 0x10, 0);
    return idle;
}
void sampleCan2() {
    bench.can2Tec = readReg(CS2, R_TEC); bench.can2Rec = readReg(CS2, R_REC);
    if (bench.can2Tec > bench.can2PeakTec) bench.can2PeakTec = bench.can2Tec;
    if (bench.can2Rec > bench.can2PeakRec) bench.can2PeakRec = bench.can2Rec;
    bench.can2EflgOr |= readReg(CS2, R_EFLG);
    bench.can2IntfOr |= readReg(CS2, R_CANINTF);
}
#endif

// ---- CAN1 receive path --------------------------------------------------
void readFrame(uint8_t buffer, Frame &f) {
    uint8_t h[5];
    csLow(CS1);
    SPI.transfer(buffer ? I_READ_RX1 : I_READ_RX0);
    for (uint8_t i = 0; i < 5; ++i) h[i] = SPI.transfer(0);   // SIDH SIDL EID8 EID0 DLC
    f.ext = h[1] & 0x08;
    const uint32_t sid = (uint32_t(h[0]) << 3) | (h[1] >> 5);
    if (f.ext) {
        f.id = (sid << 18) | (uint32_t(h[1] & 0x03) << 16) | (uint32_t(h[2]) << 8) | h[3];
        f.rtr = h[4] & 0x40;                                  // RXBnDLC.RTR (extended)
    } else {
        f.id = sid;
        f.rtr = h[1] & 0x10;                                  // RXBnSIDL.SRR (standard)
    }
    f.dlc = h[4] & 0x0F;
    const uint8_t n = payloadLen(f);
    for (uint8_t i = 0; i < n; ++i) f.data[i] = SPI.transfer(0);
    csHigh(CS1);  // Raising CS after READ RX BUFFER clears that buffer's RXnIF.
    f.ms = millis();
}
void handleRx(uint8_t buffer) {
#if OPENMOTO_SNIFFER_BENCH
    // CAN2 is the only possible sender during BENCH: request ABAT as soon as
    // CAN1 holds a frame, before spending SPI time reading it. The frame is
    // still fully verified below; an unexpected frame fails the bench.
    if (benchRunning && !bench.abortRequested) { can2RequestAbort(); bench.abortRequested = true; }
#endif
    Frame f = {};
    readFrame(buffer, f);
#if OPENMOTO_SNIFFER_BENCH
    if (benchRunning) benchCheck(f);
#endif
    if (!capturing) return;  // Drained but not counted while stopped.
    ++c.rx;
    if (f.ext) ++c.ext; else ++c.std;
    if (f.rtr) ++c.rtr;
    if (f.dlc > 8) ++c.dlcOver8;
    emitFrame(f);
}
void handleErrors(uint8_t intf) {
    if (intf & IF_ERR) {
        const uint8_t e = readReg(CS1, R_EFLG);
        if (capturing) {
            ++c.errif; c.eflgOr |= e;
            if (e & EF_RX0OVR) ++c.hwOverrun;
            if (e & EF_RX1OVR) ++c.hwOverrun;
        }
        if (e & (EF_RX0OVR | EF_RX1OVR)) bitModify(CS1, R_EFLG, EF_RX0OVR | EF_RX1OVR, 0);
    }
    if ((intf & IF_MERR) && capturing) ++c.merr;
    // Acknowledge flags so INT releases; this does not change controller error handling.
    bitModify(CS1, R_CANINTF, intf & (IF_ERR | IF_MERR), 0);
}
void handleFlags(uint8_t intf) {
    // RXB0 first: with rollover, RXB0 normally holds the older frame. If both
    // are full after an RXB0 refill the print order can differ (see docs).
    if (intf & IF_RX0) handleRx(0);
    if (intf & IF_RX1) handleRx(1);
    if (intf & (IF_ERR | IF_MERR)) handleErrors(intf);
}
// INT is an active-low level output: SPI is used only while it is asserted.
// READ STATUS (2 bytes) dispatches RX; CANINTF is read only for error flags.
// A bounded number of passes keeps serial and command handling responsive.
void serviceCan1() {
    if (!snifferEnabled) return;
    for (uint8_t pass = 0; pass < 4 && digitalRead(INT1_PIN) == LOW; ++pass) {
        const uint8_t rx = readStatus(CS1) & (IF_RX0 | IF_RX1);
        if (rx) {
            if (rx & IF_RX0) handleRx(0);
            if (rx & IF_RX1) handleRx(1);
        } else {
            const uint8_t intf = readReg(CS1, R_CANINTF) & (IF_ERR | IF_MERR);
            if (!intf) break;
            handleErrors(intf);
        }
    }
}

// ---- Status / statistics ------------------------------------------------
void printStats() {
    Msg m;
    const uint32_t now = millis(), dt = now - lastStats, n = c.rx - lastStatsRx;
    const uint32_t fps10 = dt ? uint32_t((uint64_t(n) * 10000ULL) / dt) : 0;
    out.print(F("S ms=")); out.print(now);
    out.print(F(" rx=")); out.print(c.rx);
    out.print(F(" std=")); out.print(c.std);
    out.print(F(" ext=")); out.print(c.ext);
    out.print(F(" rtr=")); out.print(c.rtr);
    out.print(F(" fps=")); out.print(fps10 / 10); out.print('.'); out.print(fps10 % 10);
    out.print(F(" window_ms=")); out.print(dt);
    out.print(F(" printed=")); out.print(c.printed);
    out.print(F(" serial_drop=")); out.print(c.serialDrop);
    out.print(F(" hw_overrun=")); out.print(c.hwOverrun);
    out.print(F(" dropped_min=")); out.print(c.serialDrop + c.hwOverrun);
    out.print(F(" merr=")); out.print(c.merr);
    out.print(F(" errif=")); out.print(c.errif);
    out.print(F(" eflg_or=0x")); out.print(c.eflgOr, HEX);
    out.print(F(" tec_peak=")); out.print(c.peakTec);
    out.print(F(" rec_peak=")); out.print(c.peakRec);
    out.print(F(" dlc_gt8=")); out.print(c.dlcOver8);
    out.print(F(" queue_peak=")); out.print(qPeak);
    out.print(F(" int_edges=")); out.print(readIntEdges());
    out.print(F(" int_missed=")); out.print(c.intMissed);
    out.print(F(" mode_faults=")); out.print(c.modeFaults);
    out.print(F(" capture=")); out.println(capturing ? F("ON") : F("OFF"));
    lastStats = now; lastStatsRx = c.rx;
}
void resetCounters() {
    c = Counters();
    noInterrupts(); intEdges = 0; interrupts();
    lastStats = millis(); lastStatsRx = 0;
    qPeak = qCount;
}
void stopAll() {
    can1.setConfigMode();
    can2.setConfigMode();
}
void health() {
    if (!snifferEnabled) return;
    // Safety: CAN1 must never leave LISTEN_ONLY (NORMAL would ACK/transmit).
    if (mode(CS1) != MODE_LISTEN) {
        stopAll();
        snifferEnabled = false; capturing = false; ++c.modeFaults;
        Msg m;
        out.println(F("# ALERT CAN1 left LISTEN_ONLY; both controllers forced to CONFIG; reset required"));
        return;
    }
    // Fallback poll: interrupt delivery is not yet hardware-validated. Read the
    // flags first, then the pin; flags set while INT is high indicate a miss.
    const uint8_t intf = readReg(CS1, R_CANINTF) & IF_SERVICED;
    if (intf && digitalRead(INT1_PIN) == HIGH) {
        if (capturing) ++c.intMissed;
        handleFlags(intf);
    } else if (intf & (IF_ERR | IF_MERR)) {
        // Under continuous RX the dispatcher prioritizes RX buffers; make sure
        // error/overflow flags are still counted and acknowledged.
        handleErrors(intf & (IF_ERR | IF_MERR));
    }
    // Listen-only deactivates the error counters (datasheet 9.3): expected 0.
    const uint8_t tec = readReg(CS1, R_TEC), rec = readReg(CS1, R_REC);
    if (tec > c.peakTec) c.peakTec = tec;
    if (rec > c.peakRec) c.peakRec = rec;
}
void periodic() {
    const uint32_t now = millis();
    if (now - lastHealth >= HEALTH_MS) { lastHealth = now; health(); }
    if (capturing && !testRunning && now - lastStats >= STATS_MS) printStats();
}

void help() {
    Msg m;
#if OPENMOTO_SNIFFER_BENCH
    out.println(F("# Commands: START STOP STATS RESET HELP BENCH FLOOD"));
#else
    out.println(F("# Commands: START STOP STATS RESET HELP"));
#endif
    out.println(F("#  START  begin capture (count + print frames)"));
    out.println(F("#  STOP   stop capture (CAN1 stays LISTEN_ONLY; frames drained, not counted)"));
    out.println(F("#  STATS  print statistics; fps is over the window since the previous S line"));
    out.println(F("#  RESET  zero all counters"));
#if OPENMOTO_SNIFFER_BENCH
    out.println(F("#  BENCH  reset counters, CAN2 sends deterministic test traffic, verify on CAN1"));
    out.println(F("#  FLOOD  measurement: CAN2 unACKed retransmission flood, print on then off"));
#endif
    out.println(F("# Frame: F <ms> <id hex> <S|E> <dlc> [payload hex] [R]"));
    out.println(F("# Stats: S key=value ...; status lines start with #"));
}

#if OPENMOTO_SNIFFER_BENCH
void benchReport(bool can2Stopped, bool pass) {
    Msg m;
    out.print(F("# BENCH frames_expected=")); out.print(BENCH_TOTAL);
    out.print(F(" matched=")); out.print(bench.matched);
    out.print(F(" missing=")); out.print(bench.missing);
    out.print(F(" unexpected=")); out.print(bench.unexpected);
    out.print(F(" timeouts=")); out.println(bench.timeouts);
    out.print(F("# BENCH copies_total=")); out.print(bench.copiesTotal);
    out.print(F(" duplicate_copies=")); out.print(bench.copiesTotal - bench.matched);
    out.print(F(" max_copies=")); out.print(bench.maxCopies);
    out.print(F(" seen_1x=")); out.print(bench.hist[0]);
    out.print(F(" seen_2x=")); out.print(bench.hist[1]);
    out.print(F(" seen_3plus=")); out.println(bench.hist[2]);
    out.print(F("# BENCH timing first_frame_latency_us=")); out.print(bench.firstLatencyUs);
    out.print(F(" CAN2_TEC_after_first=")); out.print(bench.can2TecAfterFirst);
    out.print(F(" later_latency_us_min=")); out.print(bench.latMinUs);
    out.print(F(" later_latency_us_max=")); out.print(bench.latMaxUs);
    out.print(F(" recopy_us_min=")); out.print(bench.recopyMinUs);
    out.print(F(" recopy_us_max=")); out.print(bench.recopyMaxUs);
    out.print(F(" recopy_frames=")); out.println(bench.recopyFrames);
    out.println(F("# BENCH note: CAN1 is LISTEN_ONLY and never ACKs; CAN2 ACK errors,"
                  " error-passive state and retransmitted duplicates are expected"));
    out.print(F("# BENCH CAN2 TEC=")); out.print(bench.can2Tec);
    out.print(F(" REC=")); out.print(bench.can2Rec);
    out.print(F(" peak_TEC=")); out.print(bench.can2PeakTec);
    out.print(F(" peak_REC=")); out.print(bench.can2PeakRec);
    out.print(F(" EFLG_OR=0x")); out.print(bench.can2EflgOr, HEX);
    out.print(F(" CANINTF_OR=0x")); out.println(bench.can2IntfOr, HEX);
    out.print(F("# BENCH CAN1 mode=")); out.print(snifferEnabled ? F("LISTEN_ONLY") : F("FAULT"));
    out.print(F(" tec_peak=")); out.print(c.peakTec);
    out.print(F(" rec_peak=")); out.print(c.peakRec);
    out.print(F(" merr=")); out.print(c.merr);
    out.print(F(" hw_overrun=")); out.print(c.hwOverrun);
    out.print(F(" serial_drop=")); out.print(c.serialDrop);
    out.print(F(" int_missed=")); out.println(c.intMissed);
    out.println(can2Stopped ? F("# MCP2515 #2 stopped: CONFIG (reset, TEC cleared)")
                            : F("# MCP2515 #2 stop: FAIL; disconnect USB"));
    if (bench.safetyStop) out.println(F("# BENCH stopped early by safety check"));
    out.println(pass ? F("BENCH: PASS") : F("BENCH: FAIL"));
}
void benchRun() {
    if (!snifferEnabled) { Msg m; out.println(F("# ERR sniffer disabled; reset required")); return; }
    if (mode(CS2) != MODE_CONFIG) { Msg m; out.println(F("# ERR CAN2 not in CONFIG")); return; }
    const bool wasCapturing = capturing;
    resetCounters();
    bench = Bench();
    {
        Msg m;
        out.print(F("# BENCH start frames=")); out.print(BENCH_TOTAL);
        out.println(F(" CAN2=NORMAL (auto-retransmit ON) CAN1=LISTEN_ONLY; counters reset"));
    }
    // Wait for queued output to finish so the bench capture begins cleanly.
    while (!outputIdle()) serviceAll();
    capturing = true; benchRunning = true; testRunning = true;
    // Retransmission is deliberately left enabled (no one-shot mode): with no
    // ACKing node, CAN2 must reach error-passive before CAN1 sees a valid frame.
    bool ok = can2.setNormalMode() == MCP2515::ERROR_OK && mode(CS2) == MODE_NORMAL;
    uint16_t seq = 0;
    for (uint8_t round = 0; ok && round < BENCH_ROUNDS; ++round) {
        for (uint8_t index = 0; ok && index < SPEC_COUNT; ++index, ++seq) {
            if (!snifferEnabled || mode(CS1) != MODE_LISTEN) { bench.safetyStop = true; ok = false; break; }
            bench.expected = benchFrame(round, index, seq);
            bench.copies = 0; bench.abortRequested = false;
            bench.queuedUs = micros();
            can2Queue(bench.expected);
            const uint32_t start = millis();
            bool aborted = false; uint32_t abortAt = 0;
            while (true) {
                serviceAll(); periodic();
                // benchCheck() already issued ABAT on the first verified copy;
                // here the abort is completed and ABAT cleared.
                if (!aborted && bench.copies) { can2Abort(); aborted = true; abortAt = millis(); }
                if (!aborted && millis() - start >= BENCH_TIMEOUT_MS) {
                    ++bench.timeouts; can2Abort(); aborted = true; abortAt = millis();
                }
                if (aborted && millis() - abortAt >= BENCH_QUIET_MS) break;
            }
            sampleCan2();
            if (seq == 0) bench.can2TecAfterFirst = bench.can2Tec;
            if (bench.copies) {
                ++bench.matched; bench.copiesTotal += bench.copies;
                if (bench.copies > bench.maxCopies) bench.maxCopies = bench.copies;
                ++bench.hist[bench.copies >= 3 ? 2 : bench.copies - 1];
                const uint32_t lat = bench.firstUs - bench.queuedUs;
                if (seq == 0) {
                    bench.firstLatencyUs = lat;
                } else {
                    if (!bench.latMinUs || lat < bench.latMinUs) bench.latMinUs = lat;
                    if (lat > bench.latMaxUs) bench.latMaxUs = lat;
                }
                if (bench.copies >= 2) {
                    const uint32_t re = bench.secondUs - bench.firstUs;
                    if (!bench.recopyFrames || re < bench.recopyMinUs) bench.recopyMinUs = re;
                    if (re > bench.recopyMaxUs) bench.recopyMaxUs = re;
                    ++bench.recopyFrames;
                }
            } else {
                ++bench.missing;
            }
            // Let printed lines drain before the next frame (deterministic log).
            drainOutput(BENCH_DRAIN_MS);
            if (bench.missing || bench.unexpected) ok = false;  // Fail fast, like Phase 2.
            if (bench.can2EflgOr & 0x20) ok = false;            // TXBO: unexpected; see docs.
        }
    }
    can2Abort();
    // reset() clears CAN2's TEC/REC (left at error-passive by the no-ACK bench);
    // restore its bitrate so a later BENCH starts from the same state.
    const bool can2Stopped = can2.setConfigMode() == MCP2515::ERROR_OK &&
                             can2.reset() == MCP2515::ERROR_OK && bitrateOk(can2, CS2) &&
                             mode(CS2) == MODE_CONFIG;
    // Drain trailing frames, then stop bench accounting.
    const uint32_t tail = millis();
    while (millis() - tail < 20) { serviceAll(); periodic(); }
    drainOutput(2000);  // All frame lines precede the report and verdict.
    benchRunning = false; testRunning = false;
    health();
    // A lost copy (controller overrun) or an unprinted line is a bench failure:
    // the printed log must be complete for the sequence check.
    const bool pass = ok && can2Stopped && snifferEnabled && bench.matched == BENCH_TOTAL &&
                      bench.missing == 0 && bench.unexpected == 0 && bench.timeouts == 0 &&
                      c.modeFaults == 0 && c.peakTec == 0 && c.peakRec == 0 &&
                      c.hwOverrun == 0 && c.serialDrop == 0 && c.intMissed == 0;
    benchReport(can2Stopped, pass);
    printStats();
    capturing = wasCapturing;
}

// FLOOD (measurement only, not pass/fail): CAN2 retransmits one unACKed frame
// back-to-back for FLOOD_MS, first with frame printing, then count-only. Shows
// the sniffer's real throughput limit and the no-ACK retransmission behavior.
constexpr uint32_t FLOOD_MS = 1000;
void floodPhase(bool print, bool shortest) {
    resetCounters();
    printFrames = print;
    bench = Bench();
    Frame f = {};
    if (shortest) {
        f.id = 0x7FF; f.dlc = 0;  // Shortest frame: worst-case arrival rate.
    } else {
        f.id = 0x123; f.dlc = 8;
        const uint8_t pattern[8] = {0x4F, 0x4D, 0xF1, 0x00, 0x00, 0x00, 0xA5, 0x5A};
        memcpy(f.data, pattern, 8);
    }
    bool ok = can2.setNormalMode() == MCP2515::ERROR_OK && mode(CS2) == MODE_NORMAL;
    const uint32_t start = millis();
    if (ok) can2Queue(f);
    while (ok && millis() - start < FLOOD_MS) {
        serviceAll(); periodic();
        if (!snifferEnabled) { ok = false; break; }
    }
    const uint32_t elapsed = millis() - start;
    sampleCan2();
    can2Abort();
    const bool stopped = can2.setConfigMode() == MCP2515::ERROR_OK && can2.reset() == MCP2515::ERROR_OK &&
                         bitrateOk(can2, CS2) && mode(CS2) == MODE_CONFIG;
    const uint32_t tail = millis();
    while (millis() - tail < 20) { serviceAll(); periodic(); }
    drainOutput(5000);
    printFrames = true;
    Msg m;
    out.print(F("# FLOOD frame=")); out.print(shortest ? F("7FF_DLC0") : F("123_DLC8"));
    out.print(F(" print=")); out.print(print ? F("ON") : F("OFF"));
    out.print(F(" ms=")); out.print(elapsed);
    out.print(F(" rx=")); out.print(c.rx);
    out.print(F(" rx_per_s=")); out.print(elapsed ? uint32_t((uint64_t(c.rx) * 1000ULL) / elapsed) : 0);
    out.print(F(" printed=")); out.print(c.printed);
    out.print(F(" serial_drop=")); out.print(c.serialDrop);
    out.print(F(" hw_overrun=")); out.print(c.hwOverrun);
    out.print(F(" merr=")); out.print(c.merr);
    out.print(F(" CAN2_TEC=")); out.print(bench.can2Tec);
    out.print(F(" CAN2_REC=")); out.print(bench.can2Rec);
    out.print(F(" CAN2_EFLG_OR=0x")); out.print(bench.can2EflgOr, HEX);
    out.print(F(" CAN2_CANINTF_OR=0x")); out.print(bench.can2IntfOr, HEX);
    out.print(F(" CAN1_mode=")); out.print(snifferEnabled ? F("LISTEN_ONLY") : F("FAULT"));
    out.print(F(" CAN2_stopped=")); out.println(stopped && ok ? F("CONFIG") : F("FAIL"));
}
void floodRun() {
    if (!snifferEnabled) { Msg m; out.println(F("# ERR sniffer disabled; reset required")); return; }
    if (mode(CS2) != MODE_CONFIG) { Msg m; out.println(F("# ERR CAN2 not in CONFIG")); return; }
    const bool wasCapturing = capturing;
    { Msg m; out.println(F("# FLOOD start: measurement only; CAN2 NORMAL unACKed retransmission, CAN1 LISTEN_ONLY")); }
    while (!outputIdle()) serviceAll();
    capturing = true; testRunning = true;
    floodPhase(true, false);
    floodPhase(false, false);
    floodPhase(false, true);
    testRunning = false;
    { Msg m; out.println(F("FLOOD: DONE")); }
    capturing = wasCapturing;
}
#endif

void runCommand(const char *cmd) {
    if (!strcmp(cmd, "START")) {
        if (!snifferEnabled) { Msg m; out.println(F("# ERR sniffer disabled; reset required")); return; }
        capturing = true; lastStats = millis(); lastStatsRx = c.rx;
        Msg m; out.println(F("# CAPTURE ON"));
    } else if (!strcmp(cmd, "STOP")) {
        capturing = false;
        Msg m; out.println(F("# CAPTURE OFF"));
    } else if (!strcmp(cmd, "STATS")) {
        printStats();
    } else if (!strcmp(cmd, "RESET")) {
        resetCounters();
        Msg m; out.println(F("# COUNTERS RESET"));
    } else if (!strcmp(cmd, "HELP")) {
        help();
#if OPENMOTO_SNIFFER_BENCH
    } else if (!strcmp(cmd, "BENCH")) {
        benchRun();
    } else if (!strcmp(cmd, "FLOOD")) {
        floodRun();
#endif
    } else {
        Msg m; out.print(F("# ERR unknown command: ")); out.println(cmd);
    }
}
void pollCommands() {
    static char buf[16];
    static uint8_t len = 0;
    static bool overflow = false;
    while (Serial.available() > 0) {
        const char ch = char(Serial.read());
        if (ch == '\r' || ch == '\n') {
            if (overflow) { Msg m; out.println(F("# ERR command too long")); }
            else if (len) { buf[len] = 0; runCommand(buf); }
            len = 0; overflow = false;
        } else if (len < sizeof(buf) - 1) {
            buf[len++] = char(toupper(ch));
        } else {
            overflow = true;
        }
    }
}

// ---- Initialization -----------------------------------------------------
void result(uint8_t node, const __FlashStringHelper *step, bool ok) {
    out.print(F("MCP2515 #")); out.print(node); out.print(' '); out.print(step);
    out.println(ok ? F(": PASS") : F(": FAIL"));
}
bool bitrateOk(MCP2515 &can, uint8_t cs) {
    return can.setBitrate(CAN_500KBPS, MCP_8MHZ) == MCP2515::ERROR_OK &&
           readReg(cs, R_CNF1) == 0x00 && readReg(cs, R_CNF2) == 0x90 && readReg(cs, R_CNF3) == 0x82;
}
bool initialize() {
    Msg m;
    out.println(F("Init - SPI, bitrate, filters, LISTEN_ONLY"));
    if (!csMappingOk()) { out.println(F("CS port mapping: FAIL")); return false; }
    bool config1 = can1.reset() == MCP2515::ERROR_OK && can1.setConfigMode() == MCP2515::ERROR_OK &&
                   mode(CS1) == MODE_CONFIG;
    bool config2 = can2.reset() == MCP2515::ERROR_OK && can2.setConfigMode() == MCP2515::ERROR_OK &&
                   mode(CS2) == MODE_CONFIG;
    // Swapped CNF1 challenges detect absent chips, stuck MISO and CS cross-selection.
    bool spi1 = config1, spi2 = config2;
    for (uint8_t round = 0; round < 2; ++round) {
        const uint8_t a = round == 0 ? 0x15 : 0x2A, b = round == 0 ? 0x2A : 0x15;
        if (config1) writeReg(CS1, R_CNF1, a);
        if (config2) writeReg(CS2, R_CNF1, b);
        spi1 &= readReg(CS1, R_CNF1) == a;
        spi2 &= readReg(CS2, R_CNF1) == b;
    }
    result(1, F("SPI"), spi1); result(2, F("SPI"), spi2);
    if (!spi1 || !spi2) return false;

    const bool rate1 = bitrateOk(can1, CS1);
    result(1, F("initialized 500k/8MHz"), rate1);
    if (!rate1) return false;
#if OPENMOTO_SNIFFER_BENCH
    const bool rate2 = bitrateOk(can2, CS2);
    result(2, F("initialized 500k/8MHz"), rate2);
    if (!rate2) return false;
#else
    writeReg(CS2, R_CNF1, 0x00);  // Leave CAN2 with reset-like CNF1; it stays in CONFIG.
#endif
    // Library reset(): RXM=00 (std+ext), BUKT rollover, all filters/masks 0 = accept all.
    bool accept = (readReg(CS1, R_RXB0CTRL) & 0x64) == 0x04 && (readReg(CS1, R_RXB1CTRL) & 0x60) == 0 &&
                  readReg(CS1, R_CANINTE) == IF_SERVICED;
    for (uint8_t r = R_RXM0SIDH; r < R_RXM0SIDH + 8; ++r) accept &= readReg(CS1, r) == 0;
    result(1, F("accept-all std+ext filters"), accept);
    if (!accept) return false;

    const bool txIdle = !(readReg(CS1, R_TXB0CTRL) & TXREQ) && !(readReg(CS1, R_TXB1CTRL) & TXREQ) &&
                        !(readReg(CS1, R_TXB2CTRL) & TXREQ);
    const bool listen = txIdle && can1.setListenOnlyMode() == MCP2515::ERROR_OK &&
                        mode(CS1) == MODE_LISTEN && (readReg(CS1, R_CANCTRL) & MODE_MASK) == MODE_LISTEN;
    result(1, F("LISTEN_ONLY"), listen);
    if (!listen) return false;
    const bool silent2 = mode(CS2) == MODE_CONFIG;
    result(2, F("CONFIG (silent)"), silent2);
    return silent2;
}
} // namespace

void setup() {
    // Set latches HIGH before OUTPUT to avoid a transient selection pulse.
    digitalWrite(53, HIGH); pinMode(53, OUTPUT);
    digitalWrite(CS1, HIGH); digitalWrite(CS2, HIGH);
    pinMode(CS1, OUTPUT); pinMode(CS2, OUTPUT);
    pinMode(INT1_PIN, INPUT_PULLUP); pinMode(INT2_PIN, INPUT_PULLUP);
    Serial.begin(OPENMOTO_SNIFFER_BAUD); SPI.begin(); delay(500);
    {
        Msg m;
        out.println(F("OpenMoto Phase 3A CAN sniffer"));
#if OPENMOTO_SNIFFER_BENCH
        out.println(F("Variant: bench (CAN2 test traffic only on BENCH command)"));
#else
        out.println(F("Variant: sniffer (CAN2 held silent in CONFIG)"));
#endif
        out.print(F("CAN1: LISTEN_ONLY 500000 bps, 8 MHz, CS D10, INT D2; serial "));
        out.println(uint32_t(OPENMOTO_SNIFFER_BAUD));
        out.println(F("ISOLATED BENCH ONLY; no OBD, ECU, motorcycle or external 12V"));
    }
    if (!initialize()) {
        stopAll();
        Msg m; out.println(F("INIT: FAIL; controllers set to CONFIG"));
        while (true) delay(1000);
    }
    attachInterrupt(digitalPinToInterrupt(INT1_PIN), onCan1Int, FALLING);
    snifferEnabled = true;
    resetCounters();
    Serial.flush();
    const int txFree = Serial.availableForWrite();  // Confirms the 256-byte core TX buffer.
    Msg m;
    out.print(F("# serial_tx_free=")); out.println(txFree);
    out.println(F("READY capture=OFF; send HELP"));
}

void loop() {
    serviceCan1();
    flushOutput();
    pollCommands();
    periodic();
}
