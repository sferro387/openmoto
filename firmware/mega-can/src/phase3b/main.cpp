// OpenMoto Phase 3B: high-performance CAN1 LISTEN_ONLY sniffer.
//
// Derived from Phase 3A (src/phase3a/main.cpp, unchanged). Adds: build-time
// SPI clock and baud, SPI stress/readback checks, a versioned binary record
// protocol (protocol.h), ASCII/BINARY/COUNT output modes, RAM high-water
// measurement and, in the bench build, unique-frame STREAM tests (CAN2 in
// one-shot mode), FLOOD with selectable DLC and a timer wrap test.
//
// CAN1 (CS D10, INT D2) never transmits: no transmit path exists for CAN1 and
// its LISTEN_ONLY mode is verified every 100 ms. CAN2 (CS D9) is held in CONFIG
// except during bench commands. See docs/phase3b.md.
#include <Arduino.h>
#include <SPI.h>
#include <mcp2515.h>
#include <ctype.h>
#include <stdlib.h>
#include <string.h>
#include "protocol.h"

#if OPENMOTO_MCP_CLOCK_MHZ != 8
#error "Phase 3B requires the confirmed 8 MHz module crystals"
#endif
#ifndef OPENMOTO_SNIFFER_BENCH
#define OPENMOTO_SNIFFER_BENCH 0
#endif
#ifndef OPENMOTO_SNIFFER_BAUD
#define OPENMOTO_SNIFFER_BAUD 115200
#endif
#ifndef OPENMOTO_SPI_HZ
#define OPENMOTO_SPI_HZ 1000000
#endif
#ifndef OPENMOTO_QUEUE_SIZE
#define OPENMOTO_QUEUE_SIZE 64
#endif
// Optional output pacing in bytes/s for frame/DROP/STATS records (0 = off).
// Measured: the stock ATmega16U2 USB-serial bridge occasionally drops single
// bytes when fed back-to-back bursts; pacing bounds rate and burst length.
#ifndef OPENMOTO_TX_PACE_BPS
#define OPENMOTO_TX_PACE_BPS 0
#endif
// MCP2515 FCLK max is 10 MHz (DS20001801K, SPI AC characteristics). The AVR
// SPI clock is F_CPU / {2,4,8,...}; only exact dividers are accepted so the
// configured value is the real clock.
#if OPENMOTO_SPI_HZ > 10000000
#error "MCP2515 SPI clock limit is 10 MHz"
#endif
#if (OPENMOTO_SPI_HZ != 8000000) && (OPENMOTO_SPI_HZ != 4000000) && (OPENMOTO_SPI_HZ != 2000000) && \
    (OPENMOTO_SPI_HZ != 1000000)
#error "OPENMOTO_SPI_HZ must be an exact F_CPU/2^n divider: 8, 4, 2 or 1 MHz"
#endif
#if SERIAL_TX_BUFFER_SIZE < 128
#error "Phase 3B expects -DSERIAL_TX_BUFFER_SIZE=256"
#endif
#if (OPENMOTO_QUEUE_SIZE & (OPENMOTO_QUEUE_SIZE - 1)) || OPENMOTO_QUEUE_SIZE > 128
#error "OPENMOTO_QUEUE_SIZE must be a power of two <= 128"
#endif

extern volatile unsigned long timer0_overflow_count;  // Arduino core (wiring.c)
extern volatile unsigned long timer0_millis;
extern char __heap_start;

namespace {
constexpr uint8_t CS1 = 10, CS2 = 9, INT1_PIN = 2, INT2_PIN = 3;
constexpr uint32_t SPI_HZ = OPENMOTO_SPI_HZ;
constexpr uint32_t BAUD = OPENMOTO_SNIFFER_BAUD;
constexpr uint32_t HEALTH_MS = 100, ASCII_STATS_MS = 5000, BIN_STATS_MS = 1000;

constexpr uint8_t R_CANSTAT = 0x0E, R_CANCTRL = 0x0F, R_TEC = 0x1C, R_REC = 0x1D;
constexpr uint8_t R_RXM0SIDH = 0x20, R_CNF3 = 0x28, R_CNF2 = 0x29, R_CNF1 = 0x2A;
constexpr uint8_t R_CANINTE = 0x2B, R_CANINTF = 0x2C, R_EFLG = 0x2D;
constexpr uint8_t R_TXB0CTRL = 0x30, R_TXB0D0 = 0x36, R_TXB1CTRL = 0x40, R_TXB2CTRL = 0x50;
constexpr uint8_t R_RXB0CTRL = 0x60, R_RXB1CTRL = 0x70;
constexpr uint8_t I_WRITE = 0x02, I_READ = 0x03, I_BITMOD = 0x05, I_READ_STATUS = 0xA0;
constexpr uint8_t I_READ_RX0 = 0x90, I_READ_RX1 = 0x94, I_LOAD_TX0 = 0x40, I_RTS_TX0 = 0x81;
constexpr uint8_t MODE_MASK = 0xE0, MODE_NORMAL = 0x00, MODE_LISTEN = 0x60, MODE_CONFIG = 0x80;
constexpr uint8_t IF_RX0 = 0x01, IF_RX1 = 0x02, IF_ERR = 0x20, IF_MERR = 0x80;
constexpr uint8_t IF_SERVICED = IF_RX0 | IF_RX1 | IF_ERR | IF_MERR;
constexpr uint8_t EF_RX0OVR = 0x40, EF_RX1OVR = 0x80, EF_TXBO = 0x20;
constexpr uint8_t TXREQ = 0x08, TXERR = 0x10, ABTF = 0x40, CANCTRL_OSM = 0x08, CANCTRL_ABAT = 0x10;
constexpr uint8_t CNF1_500K = 0x00, CNF2_500K = 0x90, CNF3_500K = 0x82;  // 500 kbps @ 8 MHz

MCP2515 can1(CS1, SPI_HZ, &SPI), can2(CS2, SPI_HZ, &SPI);

// ---- SPI ------------------------------------------------------------------
// Direct-port chip select (Mega 2560: D10 = PB4, D9 = PH6), mapping verified at
// startup. PORTH is not bit-addressable: read-modify-write with interrupts off.
inline void csWrite(uint8_t cs, bool high) {
    volatile uint8_t *port = cs == CS1 ? &PORTB : &PORTH;
    const uint8_t mask = cs == CS1 ? _BV(PB4) : _BV(PH6);
    const uint8_t sreg = SREG;
    cli();
    if (high) *port |= mask; else *port &= uint8_t(~mask);
    SREG = sreg;
}
// CAN1 INT (D2 = PE4) read directly: it gates output work (RX has priority).
inline bool can1Pending() { return !(PINE & _BV(PE4)); }
bool csMappingOk() {
    return portOutputRegister(digitalPinToPort(CS1)) == &PORTB && digitalPinToBitMask(CS1) == _BV(PB4) &&
           portOutputRegister(digitalPinToPort(CS2)) == &PORTH && digitalPinToBitMask(CS2) == _BV(PH6) &&
           portInputRegister(digitalPinToPort(INT1_PIN)) == &PINE && digitalPinToBitMask(INT1_PIN) == _BV(PE4);
}
inline void csLow(uint8_t cs) {
    SPI.beginTransaction(SPISettings(SPI_HZ, MSBFIRST, SPI_MODE0));
    csWrite(cs, false);
}
inline void csHigh(uint8_t cs) {
    csWrite(cs, true);
    SPI.endTransaction();
}
uint8_t readReg(uint8_t cs, uint8_t address) {
    csLow(cs);
    SPI.transfer(I_READ); SPI.transfer(address);
    const uint8_t v = SPI.transfer(0);
    csHigh(cs);
    return v;
}
void readRegs(uint8_t cs, uint8_t address, uint8_t *out, uint8_t n) {
    csLow(cs);
    SPI.transfer(I_READ); SPI.transfer(address);
    for (uint8_t i = 0; i < n; ++i) out[i] = SPI.transfer(0);
    csHigh(cs);
}
void writeReg(uint8_t cs, uint8_t address, uint8_t value) {
    csLow(cs);
    SPI.transfer(I_WRITE); SPI.transfer(address); SPI.transfer(value);
    csHigh(cs);
}
void writeRegs(uint8_t cs, uint8_t address, const uint8_t *in, uint8_t n) {
    csLow(cs);
    SPI.transfer(I_WRITE); SPI.transfer(address);
    for (uint8_t i = 0; i < n; ++i) SPI.transfer(in[i]);
    csHigh(cs);
}
void bitModify(uint8_t cs, uint8_t address, uint8_t mask, uint8_t value) {
    csLow(cs);
    SPI.transfer(I_BITMOD); SPI.transfer(address); SPI.transfer(mask); SPI.transfer(value);
    csHigh(cs);
}
uint8_t readStatus(uint8_t cs) {
    csLow(cs);
    SPI.transfer(I_READ_STATUS);
    const uint8_t v = SPI.transfer(0);
    csHigh(cs);
    return v;
}
uint8_t mode(uint8_t cs) { return readReg(cs, R_CANSTAT) & MODE_MASK; }

// ---- State ----------------------------------------------------------------
struct Frame {
    uint32_t us, ms, id;
    uint8_t dlc;
    bool ext, rtr;
    uint8_t data[8];
    uint32_t dropAfter;  // queue drops that occurred right after this frame was queued
};
uint8_t payloadLen(const Frame &f) { return omproto::payloadLen(f.dlc, f.rtr); }

struct Counters {
    uint32_t rx, std, ext, rtr, dlcOver8;
    uint32_t printed;    // frame lines/records written to serial
    uint32_t queueDrop;  // received and counted, but output queue was full
    uint32_t hwOverrun;  // RXnOVR events; each means >=1 frame lost in the controller
    uint32_t merr, errif, intMissed, modeFaults, spiErrors;
    uint8_t eflgOr, peakTec, peakRec;
};
Counters c;
volatile uint32_t intEdges = 0;
enum class OutMode : uint8_t { ASCII, BINARY, COUNT };
OutMode outMode = OutMode::ASCII;
bool snifferEnabled = false, capturing = false, testRunning = false;
#if OPENMOTO_SNIFFER_BENCH
bool benchRunning = false, streamRunning = false;
#endif
uint8_t msgDepth = 0;
uint32_t lastHealth = 0, lastStats = 0, lastStatsRx = 0, lastBinStats = 0;
uint16_t recSeq = 0;         // binary record sequence number
uint32_t recordsSent = 0;    // binary records of all types
// Timing diagnostics (micros): longest output record emission, longest gap
// between CAN1 service calls, longest CAN1 service call. Reset with counters.
uint16_t maxFlushUs = 0, maxGapUs = 0, maxServiceUs = 0;
uint32_t lastServiceEndUs = 0;
#if OPENMOTO_SNIFFER_BENCH
uint16_t tFlush = 0, tGap = 0, tService = 0;  // snapshot at the end of test traffic
void snapTiming() { tFlush = maxFlushUs; tGap = maxGapUs; tService = maxServiceUs; }
#endif

void serviceCan1();
bool bitrateOk(MCP2515 &can, uint8_t cs);

// ---- RAM high-water (stack painting) --------------------------------------
constexpr uint8_t PAINT = 0xA5;
void paintStack() {
    uint8_t *p = reinterpret_cast<uint8_t *>(&__heap_start);
    uint8_t *sp = reinterpret_cast<uint8_t *>(SP);
    while (p < sp - 32) *p++ = PAINT;  // leave margin below the current frame
}
// Full scan of ~5 KB takes ~5 ms (measured: it stalled CAN1 reception when
// done in the 1 s binary heartbeat). It is therefore only run while no test
// traffic is active (startup, STATS command, end-of-test reports); the
// heartbeat reports the cached value from the last scan.
uint16_t cachedFreeRam = 0;
uint16_t minFreeRam() {  // untouched bytes above the heap start (no malloc used)
    const uint8_t *p = reinterpret_cast<const uint8_t *>(&__heap_start);
    const uint8_t *sp = reinterpret_cast<const uint8_t *>(SP);
    uint16_t n = 0;
    while (p < sp && *p == PAINT) { ++p; ++n; }
    cachedFreeRam = n;
    return n;
}

// ---- Serial output --------------------------------------------------------
// Write bytes that are known to fit in the TX buffer, servicing CAN1 between
// bytes whenever its INT is asserted. Measured: writing a whole record without
// this (up to ~900 us under 1 Mbaud TX-interrupt load) exceeded CAN1's two-
// buffer slack and caused controller overruns. Servicing only enqueues frames
// (no output), so records cannot interleave.
void writePreemptible(const uint8_t *p, uint8_t n) {
    for (uint8_t i = 0; i < n; ++i) {
        if (can1Pending()) serviceCan1();
        Serial.write(p[i]);
    }
}
// Transmit a record built without CRC, computing the CRC byte by byte while
// writing so the whole emission is preemptible by CAN1 servicing. Measured:
// computing a 57-byte STATS CRC up front left a non-preemptible window that
// caused ~1 controller overrun per second during DLC 0 floods.
void emitStreaming(const uint8_t *p, uint8_t nNoCrc) {
    uint16_t crc = 0xFFFF;
    for (uint8_t i = 0; i < nNoCrc; ++i) {
        if (can1Pending()) serviceCan1();
        if (i >= 2) crc = omproto::crcUpdate(crc, p[i]);
        Serial.write(p[i]);
    }
    if (can1Pending()) serviceCan1();
    Serial.write(uint8_t(crc >> 8));
    Serial.write(uint8_t(crc));
}
void writeReliable(const uint8_t *p, uint8_t n) {  // waits for room, keeps draining CAN1
    while (Serial.availableForWrite() < int(n)) serviceCan1();
    writePreemptible(p, n);
}
void emitRecordReliable(uint8_t *rec, uint8_t len) {
    writeReliable(rec, len);
    ++recSeq; ++recordsSent;
}
// Status text. ASCII/COUNT: raw lines. BINARY: each line becomes TEXT records
// (split with the continued flag if longer than 63 bytes).
struct Out : public Print {
    char line[omproto::MAX_BODY - 1];
    uint8_t len = 0;
    void flushText(bool continued) {
        uint8_t rec[omproto::MAX_RECORD];
        const uint8_t n = omproto::encodeText(rec, recSeq, line, len, continued);
        emitRecordReliable(rec, n);
        len = 0;
    }
    size_t write(uint8_t b) override {
        if (outMode != OutMode::BINARY) {
            while (Serial.availableForWrite() <= 0) serviceCan1();
            return Serial.write(b);
        }
        if (b == '\r') return 1;
        if (b == '\n') { flushText(false); return 1; }
        if (len == sizeof(line)) flushText(true);
        line[len++] = char(b);
        return 1;
    }
    using Print::write;
};
Out out;
struct Msg {
    Msg() { ++msgDepth; }
    ~Msg() { --msgDepth; }
};
void onCan1Int() { ++intEdges; }
uint32_t readIntEdges() { noInterrupts(); const uint32_t v = intEdges; interrupts(); return v; }

char *putDec(char *p, uint32_t v) {  // division-free (Phase 3A measurement)
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
constexpr uint8_t MAX_LINE = 43;  // Phase 3A ASCII frame line format
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

// ---- Output queue ---------------------------------------------------------
// The receive path only copies frames here; flushOutput() formats/encodes at
// most one record per call and never blocks.
constexpr uint8_t QUEUE_SIZE = OPENMOTO_QUEUE_SIZE;
Frame queue[QUEUE_SIZE];
uint8_t qHead = 0, qTail = 0, qCount = 0, qPeak = 0;
void enqueue(const Frame &f) {
    if (outMode == OutMode::COUNT) return;
    if (qCount == QUEUE_SIZE) {
        // The queue is full, so the drop happens after the newest queued frame:
        // record it there. A DROP record is emitted right after that frame
        // (exact position, at most one per frame; no starvation of frames).
        ++c.queueDrop;
        ++queue[(qHead - 1) & (QUEUE_SIZE - 1)].dropAfter;
        return;
    }
    queue[qHead] = f;
    queue[qHead].dropAfter = 0;
    qHead = uint8_t((qHead + 1) & (QUEUE_SIZE - 1));
    if (++qCount > qPeak) qPeak = qCount;
}
#if OPENMOTO_TX_PACE_BPS
constexpr uint32_t PACE_US_PER_BYTE = 1000000UL / OPENMOTO_TX_PACE_BPS;
constexpr uint32_t PACE_BURST_US = 64UL * PACE_US_PER_BYTE;  // up to 64 bytes of credit
uint32_t paceNext = 0;
// Keep paceNext within ~100 ms of now (called from the health tick): the
// signed-difference comparisons below are only valid while |now - paceNext|
// < 2^31 us (35.8 min). Found by WRAPTEST: a stale paceNext blocked output.
void paceRefresh() {
    const uint32_t now = micros();
    if (int32_t(now - paceNext) > int32_t(PACE_BURST_US)) paceNext = now - PACE_BURST_US;
}
#if OPENMOTO_SNIFFER_BENCH
void paceReanchor() { paceNext = micros() - PACE_BURST_US; }  // after WRAPTEST's time jump
#endif
// Virtual-clock token bucket (wrap-safe signed differences).
bool paceAllow(uint8_t n) {
    const uint32_t now = micros();
    if (int32_t(now - paceNext) > int32_t(PACE_BURST_US)) paceNext = now - PACE_BURST_US;
    if (int32_t(paceNext - now) > 0) return false;
    paceNext += uint32_t(n) * PACE_US_PER_BYTE;
    return true;
}
#else
inline bool paceAllow(uint8_t) { return true; }
inline void paceRefresh() {}
inline void paceReanchor() {}
#endif
omproto::Stats snapshot();
bool binStatsDue = false;
uint32_t pendingDrop = 0, pendingDropTotal = 0;  // DROP owed right after the last FRAME written
void flushOutputImpl();
void flushOutput() {
    // Strict priority: never start encoding/writing while CAN1 holds a frame.
    // Under overload frames are then dropped in the queue (counted at their
    // exact position) instead of being lost inside the controller.
    if (msgDepth || can1Pending()) return;
    const uint32_t t0 = micros();
    flushOutputImpl();
    const uint32_t dt = micros() - t0;
    if (dt > maxFlushUs) maxFlushUs = dt > 0xFFFF ? 0xFFFF : uint16_t(dt);
}
// One output step: at most one record (or ASCII line), written preemptibly.
void flushOutputImpl() {
    const int room = Serial.availableForWrite();
    if (outMode == OutMode::BINARY) {
        uint8_t rec[omproto::MAX_RECORD];
        if (pendingDrop) {  // must directly follow the FRAME it belongs to
            if (room < omproto::HEADER_LEN + 8 + omproto::CRC_LEN ||
                !paceAllow(omproto::HEADER_LEN + 8 + omproto::CRC_LEN)) return;
            const uint8_t n = omproto::encodeDrop(rec, recSeq, pendingDrop, pendingDropTotal, false);
            ++recSeq; ++recordsSent; pendingDrop = 0;
            emitStreaming(rec, n);
            return;
        }
        if (binStatsDue) {  // heartbeat; deferred (not lost) if no room
            if (room < omproto::HEADER_LEN + omproto::STATS_BODY_LEN + omproto::CRC_LEN ||
                !paceAllow(omproto::HEADER_LEN + omproto::STATS_BODY_LEN + omproto::CRC_LEN)) return;
            const uint8_t n = omproto::encodeStats(rec, recSeq, snapshot(), false);
            ++recSeq; ++recordsSent; binStatsDue = false;
            emitStreaming(rec, n);
            return;
        }
        if (!qCount) return;
        const uint8_t need = omproto::HEADER_LEN + 9 + payloadLen(queue[qTail]) + omproto::CRC_LEN;
        if (room < need || !paceAllow(need)) return;
        const Frame f = queue[qTail];  // copy, then pop: servicing may enqueue meanwhile
        qTail = uint8_t((qTail + 1) & (QUEUE_SIZE - 1));
        --qCount;
        const uint8_t n = omproto::encodeFrame(rec, recSeq, f.us, f.id, f.ext, f.rtr, f.dlc, f.data, false);
        ++recSeq; ++recordsSent; ++c.printed;
        if (f.dropAfter) { pendingDrop = f.dropAfter; pendingDropTotal = c.queueDrop; }
        emitStreaming(rec, n);
    } else {
        if (!qCount || room < int(MAX_LINE) || !paceAllow(MAX_LINE)) return;
        char line[MAX_LINE + 1];
        const Frame f = queue[qTail];
        qTail = uint8_t((qTail + 1) & (QUEUE_SIZE - 1));
        --qCount;
        ++c.printed;
        const uint8_t n = formatFrame(line, f);
        writePreemptible(reinterpret_cast<const uint8_t *>(line), n);
    }
}
bool outputIdle() {
    return !qCount && !binStatsDue && !pendingDrop &&
           Serial.availableForWrite() >= SERIAL_TX_BUFFER_SIZE - 1;
}
void serviceAll() { serviceCan1(); flushOutput(); }
#if OPENMOTO_SNIFFER_BENCH
void drainOutput(uint32_t maxMs) {
    const uint32_t start = millis();
    while (!outputIdle() && millis() - start < maxMs) serviceAll();
}
#endif

// ---- Bench (CAN2 traffic) -------------------------------------------------
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
    uint32_t matched, missing, unexpected, timeouts, copiesTotal, maxCopies;
    uint8_t can2Tec, can2Rec, can2PeakTec, can2EflgOr, can2IntfOr;
};
Bench bench;

// Unique-frame stream (CAN2 one-shot mode). DLC 8: ID 0x100, payload
// 4F 4D 5B s0 s1 s2 s3 ~s0 (32-bit sequence). DLC 0: ID = seq & 0x7FF.
constexpr uint32_t STREAM_ID8 = 0x100;
struct StreamTest {
    uint8_t dlc;
    bool started;          // a first frame has been seen
    uint32_t sent, expect, unique, lostGaps, dup, reorder, corrupt, txOk, attemptsFlagged, flagRaces;
    uint32_t lastSeq;
    uint8_t txCtrlOr, can2EflgOr, can2IntfOr, can2Tec, can2Rec;
};
StreamTest st;

void can2RequestAbort() { bitModify(CS2, R_CANCTRL, CANCTRL_ABAT, CANCTRL_ABAT); }
// Transmit path exists ONLY for CAN2 (CS2 hard-coded).
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
    csLow(CS2); SPI.transfer(I_LOAD_TX0);
    for (uint8_t i = 0; i < 5 + n; ++i) SPI.transfer(b[i]);
    csHigh(CS2);
    csLow(CS2); SPI.transfer(I_RTS_TX0); csHigh(CS2);  // SPI mode 00, CS raised at once (errata 2)
}
bool can2Abort() {
    can2RequestAbort();
    const uint32_t start = millis();
    while ((readReg(CS2, R_TXB0CTRL) & TXREQ) && millis() - start < 10) serviceCan1();
    const bool idle = !(readReg(CS2, R_TXB0CTRL) & TXREQ);
    bitModify(CS2, R_CANCTRL, CANCTRL_ABAT, 0);
    return idle;
}
// Classify a finished one-shot attempt (TXREQ already clear). Measured: a read
// can land after TXREQ clears but before ABTF/TXERR are set (2 of 16,763
// attempts at 115200), which looked like an impossible ACKed success. If no
// flag is set, read once more; only a second flag-free read counts as tx_ok.
void classifyAttempt(uint8_t ctrl) {
    if (!(ctrl & (ABTF | TXERR))) {
        const uint8_t again = readReg(CS2, R_TXB0CTRL);
        if (again & (ABTF | TXERR)) ++st.flagRaces;
        ctrl = again;
    }
    st.txCtrlOr |= ctrl;
    if (ctrl & (ABTF | TXERR)) ++st.attemptsFlagged; else ++st.txOk;
}
bool can2Stop() {  // ABAT, CONFIG, reset (clears TEC), restore bitrate
    can2Abort();
    bitModify(CS2, R_CANCTRL, CANCTRL_OSM, 0);
    return can2.setConfigMode() == MCP2515::ERROR_OK && can2.reset() == MCP2515::ERROR_OK &&
           bitrateOk(can2, CS2) && mode(CS2) == MODE_CONFIG;
}
Frame benchFrame(uint8_t round, uint8_t index, uint16_t seq) {
    const BenchSpec &s = SPECS[index];
    Frame f = {};
    f.id = s.id; f.ext = s.ext; f.rtr = s.rtr; f.dlc = s.dlc;
    const uint8_t pattern[8] = {0x4F, 0x4D, round, index, uint8_t(seq), uint8_t(seq >> 8), 0xA5, 0x5A};
    if (!f.rtr) memcpy(f.data, pattern, payloadLen(f));
    return f;
}
Frame streamFrame(uint32_t seq, uint8_t dlc) {
    Frame f = {};
    if (dlc == 8) {
        f.id = STREAM_ID8; f.dlc = 8;
        const uint8_t d[8] = {0x4F, 0x4D, 0x5B, uint8_t(seq), uint8_t(seq >> 8), uint8_t(seq >> 16),
                              uint8_t(seq >> 24), uint8_t(~uint8_t(seq))};
        memcpy(f.data, d, 8);
    } else {
        f.id = seq & 0x7FF; f.dlc = 0;
    }
    return f;
}
void benchCheck(const Frame &f) {
    const Frame &e = bench.expected;
    if (!(f.id == e.id && f.ext == e.ext && f.rtr == e.rtr && f.dlc == e.dlc &&
          memcmp(f.data, e.data, payloadLen(f)) == 0)) { ++bench.unexpected; return; }
    ++bench.copies;
}
void streamCheck(const Frame &f) {
    uint32_t seq;
    if (st.dlc == 8) {
        if (f.ext || f.rtr || f.id != STREAM_ID8 || f.dlc != 8 || f.data[0] != 0x4F ||
            f.data[1] != 0x4D || f.data[2] != 0x5B || f.data[7] != uint8_t(~f.data[3])) {
            ++st.corrupt; return;
        }
        seq = uint32_t(f.data[3]) | (uint32_t(f.data[4]) << 8) | (uint32_t(f.data[5]) << 16) |
              (uint32_t(f.data[6]) << 24);
        if (st.started && seq == st.lastSeq) { ++st.dup; return; }
        if (seq < st.expect) { ++st.reorder; return; }
        st.lostGaps += seq - st.expect;
    } else {
        if (f.ext || f.rtr || f.dlc != 0) { ++st.corrupt; return; }
        const uint32_t id = f.id & 0x7FF;
        if (st.started && id == (st.lastSeq & 0x7FF)) { ++st.dup; return; }
        const uint32_t gap = (id - (st.expect & 0x7FF)) & 0x7FF;  // < 2048 consecutive losses
        seq = st.expect + gap;
        st.lostGaps += gap;
    }
    st.started = true;
    st.lastSeq = seq;
    st.expect = seq + 1;
    ++st.unique;
}
#endif

// ---- CAN1 receive path ----------------------------------------------------
void readFrame(uint8_t buffer, Frame &f) {
    uint8_t h[5];
    csLow(CS1);
    SPI.transfer(buffer ? I_READ_RX1 : I_READ_RX0);
    for (uint8_t i = 0; i < 5; ++i) h[i] = SPI.transfer(0);  // SIDH SIDL EID8 EID0 DLC
    f.ext = h[1] & 0x08;
    const uint32_t sid = (uint32_t(h[0]) << 3) | (h[1] >> 5);
    if (f.ext) {
        f.id = (sid << 18) | (uint32_t(h[1] & 0x03) << 16) | (uint32_t(h[2]) << 8) | h[3];
        f.rtr = h[4] & 0x40;
    } else {
        f.id = sid;
        f.rtr = h[1] & 0x10;
    }
    f.dlc = h[4] & 0x0F;
    const uint8_t n = payloadLen(f);
    for (uint8_t i = 0; i < n; ++i) f.data[i] = SPI.transfer(0);
    csHigh(CS1);  // clears RXnIF
    f.us = micros();
    f.ms = millis();
}
void handleRx(uint8_t buffer) {
#if OPENMOTO_SNIFFER_BENCH
    if (benchRunning && !bench.abortRequested) { can2RequestAbort(); bench.abortRequested = true; }
#endif
    Frame f = {};
    readFrame(buffer, f);
#if OPENMOTO_SNIFFER_BENCH
    if (benchRunning) benchCheck(f);
    if (streamRunning) streamCheck(f);
#endif
    if (!capturing) return;
    ++c.rx;
    if (f.ext) ++c.ext; else ++c.std;
    if (f.rtr) ++c.rtr;
    if (f.dlc > 8) ++c.dlcOver8;
    enqueue(f);
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
    bitModify(CS1, R_CANINTF, intf & (IF_ERR | IF_MERR), 0);
}
void handleFlags(uint8_t intf) {
    if (intf & IF_RX0) handleRx(0);
    if (intf & IF_RX1) handleRx(1);
    if (intf & (IF_ERR | IF_MERR)) handleErrors(intf);
}
void serviceCan1Impl();
void serviceCan1() {
    if (!snifferEnabled) return;
    const uint32_t t0 = micros();
    if (lastServiceEndUs) {
        const uint32_t gap = t0 - lastServiceEndUs;
        if (gap > maxGapUs) maxGapUs = gap > 0xFFFF ? 0xFFFF : uint16_t(gap);
    }
    serviceCan1Impl();
    lastServiceEndUs = micros();
    const uint32_t dt = lastServiceEndUs - t0;
    if (dt > maxServiceUs) maxServiceUs = dt > 0xFFFF ? 0xFFFF : uint16_t(dt);
}
void serviceCan1Impl() {
    for (uint8_t pass = 0; pass < 4 && can1Pending(); ++pass) {
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

// ---- Status ---------------------------------------------------------------
omproto::Stats snapshot() {
    omproto::Stats s;
    s.tsUs = micros(); s.ms = millis(); s.rx = c.rx; s.hwOverrun = c.hwOverrun;
    s.queueDrop = c.queueDrop; s.merr = c.merr; s.errif = c.errif; s.intMissed = c.intMissed;
    s.modeFaults = c.modeFaults; s.spiErrors = c.spiErrors; s.records = recordsSent;
    s.queuePeak = qPeak; s.eflgOr = c.eflgOr; s.tecPeak = c.peakTec; s.recPeak = c.peakRec;
    s.minFreeRam = cachedFreeRam;  // never scan inside the heartbeat
    return s;
}
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
    out.print(F(" queue_drop=")); out.print(c.queueDrop);
    out.print(F(" hw_overrun=")); out.print(c.hwOverrun);
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
    out.print(F(" spi_errors=")); out.print(c.spiErrors);
    out.print(F(" min_free_ram=")); out.print(minFreeRam());
    out.print(F(" records=")); out.print(recordsSent);
    out.print(F(" max_flush_us=")); out.print(maxFlushUs);
    out.print(F(" max_gap_us=")); out.print(maxGapUs);
    out.print(F(" max_service_us=")); out.print(maxServiceUs);
    out.print(F(" capture=")); out.println(capturing ? F("ON") : F("OFF"));
    lastStats = now; lastStatsRx = c.rx;
}
void resetCounters() {
    c = Counters();
    noInterrupts(); intEdges = 0; interrupts();
    lastStats = millis(); lastStatsRx = 0;
    qPeak = qCount;
    maxFlushUs = maxGapUs = maxServiceUs = 0;
    lastServiceEndUs = 0;
}
void stopAll() { can1.setConfigMode(); can2.setConfigMode(); }
void health() {
    if (!snifferEnabled) return;
    if (mode(CS1) != MODE_LISTEN) {  // safety: CAN1 must never leave LISTEN_ONLY
        stopAll();
        snifferEnabled = false; capturing = false; ++c.modeFaults;
        Msg m;
        out.println(F("# ALERT CAN1 left LISTEN_ONLY; both controllers forced to CONFIG; reset required"));
        return;
    }
    // Runtime SPI integrity: bit-timing registers must read back unchanged.
    uint8_t cnf[3];
    readRegs(CS1, R_CNF3, cnf, 3);
    if (cnf[0] != CNF3_500K || cnf[1] != CNF2_500K || cnf[2] != CNF1_500K) ++c.spiErrors;
    const uint8_t intf = readReg(CS1, R_CANINTF) & IF_SERVICED;
    if (intf && digitalRead(INT1_PIN) == HIGH) {
        if (capturing) ++c.intMissed;
        handleFlags(intf);
    } else if (intf & (IF_ERR | IF_MERR)) {
        handleErrors(intf & (IF_ERR | IF_MERR));
    }
    const uint8_t tec = readReg(CS1, R_TEC), rec = readReg(CS1, R_REC);
    if (tec > c.peakTec) c.peakTec = tec;
    if (rec > c.peakRec) c.peakRec = rec;
}
void periodic() {
    const uint32_t now = millis();
    if (now - lastHealth >= HEALTH_MS) { lastHealth = now; health(); paceRefresh(); }
    if (outMode == OutMode::BINARY) {
        if (now - lastBinStats >= BIN_STATS_MS) { lastBinStats = now; binStatsDue = true; }
    } else if (capturing && !testRunning && now - lastStats >= ASCII_STATS_MS) {
        printStats();
    }
}
void help() {
    Msg m;
#if OPENMOTO_SNIFFER_BENCH
    out.println(F("# Commands: START STOP STATS RESET HELP ASCII BINARY COUNT BENCH"));
    out.println(F("#  FLOOD <0|8> <ms>  STREAM <0|8> <ms>  WRAPTEST"));
#else
    out.println(F("# Commands: START STOP STATS RESET HELP ASCII BINARY COUNT"));
#endif
    out.println(F("# Output: ASCII = Phase 3A 'F' lines; BINARY = protocol v1 records; COUNT = counters only"));
}

#if OPENMOTO_SNIFFER_BENCH
// ---- BENCH (Phase 3A procedure, stricter accounting) ---------------------
void benchRun() {
    if (!snifferEnabled || mode(CS2) != MODE_CONFIG) { Msg m; out.println(F("# ERR not ready")); return; }
    const bool wasCapturing = capturing;
    resetCounters();
    bench = Bench();
    { Msg m; out.print(F("# BENCH start frames=")); out.println(BENCH_TOTAL); }
    while (!outputIdle()) serviceAll();
    capturing = true; benchRunning = true; testRunning = true;
    bool ok = can2.setNormalMode() == MCP2515::ERROR_OK && mode(CS2) == MODE_NORMAL;
    uint16_t seq = 0;
    for (uint8_t round = 0; ok && round < BENCH_ROUNDS; ++round) {
        for (uint8_t index = 0; ok && index < SPEC_COUNT; ++index, ++seq) {
            if (!snifferEnabled || mode(CS1) != MODE_LISTEN) { ok = false; break; }
            bench.expected = benchFrame(round, index, seq);
            bench.copies = 0; bench.abortRequested = false;
            can2Queue(bench.expected);
            const uint32_t start = millis();
            bool aborted = false; uint32_t abortAt = 0;
            while (true) {
                serviceAll(); periodic();
                if (!aborted && bench.copies) { can2Abort(); aborted = true; abortAt = millis(); }
                if (!aborted && millis() - start >= BENCH_TIMEOUT_MS) {
                    ++bench.timeouts; can2Abort(); aborted = true; abortAt = millis();
                }
                if (aborted && millis() - abortAt >= BENCH_QUIET_MS) break;
            }
            bench.can2Tec = readReg(CS2, R_TEC); bench.can2Rec = readReg(CS2, R_REC);
            if (bench.can2Tec > bench.can2PeakTec) bench.can2PeakTec = bench.can2Tec;
            bench.can2EflgOr |= readReg(CS2, R_EFLG); bench.can2IntfOr |= readReg(CS2, R_CANINTF);
            if (bench.copies) {
                ++bench.matched; bench.copiesTotal += bench.copies;
                if (bench.copies > bench.maxCopies) bench.maxCopies = bench.copies;
            } else {
                ++bench.missing;
            }
            drainOutput(BENCH_DRAIN_MS);
            if (bench.missing || bench.unexpected || (bench.can2EflgOr & EF_TXBO)) ok = false;
        }
    }
    const bool can2Stopped = can2Stop();
    const uint32_t tail = millis();
    while (millis() - tail < 20) { serviceAll(); periodic(); }
    drainOutput(2000);
    benchRunning = false; testRunning = false;
    health();
    const bool pass = ok && can2Stopped && snifferEnabled && bench.matched == BENCH_TOTAL &&
                      bench.missing == 0 && bench.unexpected == 0 && bench.timeouts == 0 &&
                      c.modeFaults == 0 && c.peakTec == 0 && c.peakRec == 0 && c.hwOverrun == 0 &&
                      c.queueDrop == 0 && c.intMissed == 0 && c.spiErrors == 0 &&
                      (outMode == OutMode::COUNT || c.printed == c.rx);
    {
        Msg m;
        out.print(F("# BENCH frames_expected=")); out.print(BENCH_TOTAL);
        out.print(F(" matched=")); out.print(bench.matched);
        out.print(F(" missing=")); out.print(bench.missing);
        out.print(F(" unexpected=")); out.print(bench.unexpected);
        out.print(F(" timeouts=")); out.print(bench.timeouts);
        out.print(F(" copies_total=")); out.print(bench.copiesTotal);
        out.print(F(" max_copies=")); out.println(bench.maxCopies);
        out.print(F("# BENCH CAN2 TEC=")); out.print(bench.can2Tec);
        out.print(F(" peak_TEC=")); out.print(bench.can2PeakTec);
        out.print(F(" EFLG_OR=0x")); out.print(bench.can2EflgOr, HEX);
        out.print(F(" CANINTF_OR=0x")); out.print(bench.can2IntfOr, HEX);
        out.print(F(" stopped=")); out.println(can2Stopped ? F("CONFIG") : F("FAIL"));
        out.println(pass ? F("BENCH: PASS") : F("BENCH: FAIL"));
    }
    printStats();
    capturing = wasCapturing;
}

// ---- FLOOD: one unACKed frame retransmitted back-to-back (Phase 3A method) --
void floodRun(uint8_t dlc, uint32_t ms) {
    if (!snifferEnabled || mode(CS2) != MODE_CONFIG) { Msg m; out.println(F("# ERR not ready")); return; }
    const bool wasCapturing = capturing;
    { Msg m; out.print(F("# FLOOD start dlc=")); out.print(dlc); out.print(F(" ms=")); out.println(ms); }
    while (!outputIdle()) serviceAll();
    resetCounters();
    capturing = true; testRunning = true;
    Frame f = {};
    if (dlc == 8) {
        f.id = 0x123; f.dlc = 8;
        const uint8_t d[8] = {0x4F, 0x4D, 0xF1, 0x00, 0x00, 0x00, 0xA5, 0x5A};
        memcpy(f.data, d, 8);
    } else {
        f.id = 0x7FF; f.dlc = 0;
    }
    bool ok = can2.setNormalMode() == MCP2515::ERROR_OK && mode(CS2) == MODE_NORMAL;
    const uint32_t start = millis();
    if (ok) can2Queue(f);
    while (ok && millis() - start < ms) {
        serviceAll(); periodic();
        if (!snifferEnabled) ok = false;
    }
    const uint32_t elapsed = millis() - start;
    snapTiming();
    const uint8_t tec = readReg(CS2, R_TEC), eflg = readReg(CS2, R_EFLG), intf = readReg(CS2, R_CANINTF);
    const bool stopped = can2Stop();
    const uint32_t tail = millis();
    while (millis() - tail < 20) { serviceAll(); periodic(); }
    drainOutput(10000);
    testRunning = false;
    health();
    Msg m;
    out.print(F("# FLOOD dlc=")); out.print(dlc);
    out.print(F(" ms=")); out.print(elapsed);
    out.print(F(" rx=")); out.print(c.rx);
    out.print(F(" rx_per_s=")); out.print(elapsed ? uint32_t((uint64_t(c.rx) * 1000ULL) / elapsed) : 0);
    out.print(F(" unique=1 printed=")); out.print(c.printed);
    out.print(F(" queue_drop=")); out.print(c.queueDrop);
    out.print(F(" hw_overrun=")); out.print(c.hwOverrun);
    out.print(F(" int_missed=")); out.print(c.intMissed);
    out.print(F(" spi_errors=")); out.print(c.spiErrors);
    out.print(F(" queue_peak=")); out.print(qPeak);
    out.print(F(" merr=")); out.print(c.merr);
    out.print(F(" max_flush_us=")); out.print(tFlush);
    out.print(F(" max_gap_us=")); out.print(tGap);
    out.print(F(" max_service_us=")); out.print(tService);
    out.print(F(" min_free_ram=")); out.print(minFreeRam());
    out.print(F(" CAN2_TEC=")); out.print(tec);
    out.print(F(" CAN2_EFLG=0x")); out.print(eflg, HEX);
    out.print(F(" CAN2_CANINTF=0x")); out.print(intf, HEX);
    out.print(F(" CAN2_stopped=")); out.println(stopped && ok ? F("CONFIG") : F("FAIL"));
    out.println(F("FLOOD: DONE"));
    capturing = wasCapturing;
}

// ---- STREAM: unique frames, CAN2 one-shot mode ----------------------------
// One-shot mode (CANCTRL.OSM) is used deliberately: with no ACKing node, normal
// mode would retransmit each frame until aborted, producing duplicates and
// capping the unique rate. In one-shot mode every RTS is attempted exactly once
// (failed attempts set ABTF/TXERR, datasheet TXBnCTRL note 2), and Phase 3A
// showed CAN1 stores each attempt. Only TXB0 is used, one frame at a time
// (errata 1/4 workaround). ABAT is never used, so each RTS = one attempt.
void streamRun(uint8_t dlc, uint32_t ms) {
    if (!snifferEnabled || mode(CS2) != MODE_CONFIG) { Msg m; out.println(F("# ERR not ready")); return; }
    const bool wasCapturing = capturing;
    { Msg m; out.print(F("# STREAM start dlc=")); out.print(dlc); out.print(F(" ms=")); out.println(ms); }
    while (!outputIdle()) serviceAll();
    resetCounters();
    st = StreamTest();
    st.dlc = dlc;
    capturing = true; testRunning = true; streamRunning = true;
    bool ok = can2.setNormalMode() == MCP2515::ERROR_OK && mode(CS2) == MODE_NORMAL;
    if (ok) {
        bitModify(CS2, R_CANCTRL, CANCTRL_OSM, CANCTRL_OSM);
        ok = (readReg(CS2, R_CANCTRL) & CANCTRL_OSM) != 0;
    }
    const uint32_t start = millis();
    bool pending = false;
    while (ok && millis() - start < ms) {
        serviceAll(); periodic();
        if (!snifferEnabled || (mode(CS1) != MODE_LISTEN)) { ok = false; break; }
        const uint8_t ctrl = readReg(CS2, R_TXB0CTRL);
        if (ctrl & TXREQ) continue;
        if (pending) classifyAttempt(ctrl);  // previous attempt finished
        can2Queue(streamFrame(st.sent, dlc));
        ++st.sent; pending = true;
    }
    // Let the last one-shot attempt finish (never ABAT: keeps sent exact).
    const uint32_t t0 = millis();
    while (pending && millis() - t0 < 20) {
        serviceAll();
        const uint8_t ctrl = readReg(CS2, R_TXB0CTRL);
        if (!(ctrl & TXREQ)) {
            classifyAttempt(ctrl);
            pending = false;
        }
    }
    const uint32_t elapsed = millis() - start;
    snapTiming();
    st.can2Tec = readReg(CS2, R_TEC); st.can2Rec = readReg(CS2, R_REC);
    st.can2EflgOr = readReg(CS2, R_EFLG); st.can2IntfOr = readReg(CS2, R_CANINTF);
    const bool stopped = can2Stop();
    const uint32_t tail = millis();
    while (millis() - tail < 20) { serviceAll(); periodic(); }
    streamRunning = false;
    drainOutput(15000);
    testRunning = false;
    health();
    const uint32_t lostTail = (st.sent > st.expect) ? st.sent - st.expect : 0;
    const uint32_t lost = st.lostGaps + lostTail;
    // Integrity verdict: no corruption, every loss explained by a controller
    // overrun, exact accounting, CAN1 healthy. Loss itself is reported, not hidden.
    const bool accounted = st.unique + lost == st.sent && st.expect <= st.sent;
    const bool explained = (lost == 0) == (c.hwOverrun == 0);
    const bool pass = ok && !pending && stopped && snifferEnabled && accounted && explained &&
                      st.corrupt == 0 && st.dup == 0 && st.reorder == 0 && st.txOk == 0 &&
                      c.rx == st.unique && c.intMissed == 0 && c.spiErrors == 0 && c.modeFaults == 0 &&
                      (outMode == OutMode::COUNT || c.printed + c.queueDrop == c.rx);
    Msg m;
    out.print(F("# STREAM dlc=")); out.print(dlc);
    out.print(F(" ms=")); out.print(elapsed);
    out.print(F(" sent=")); out.print(st.sent);
    out.print(F(" unique=")); out.print(st.unique);
    out.print(F(" lost=")); out.print(lost);
    out.print(F(" lost_tail=")); out.print(lostTail);
    out.print(F(" dup=")); out.print(st.dup);
    out.print(F(" reorder=")); out.print(st.reorder);
    out.print(F(" corrupt=")); out.print(st.corrupt);
    out.print(F(" rx=")); out.print(c.rx);
    out.println();
    out.print(F("# STREAM sent_per_s=")); out.print(elapsed ? uint32_t((uint64_t(st.sent) * 1000ULL) / elapsed) : 0);
    out.print(F(" unique_per_s=")); out.print(elapsed ? uint32_t((uint64_t(st.unique) * 1000ULL) / elapsed) : 0);
    out.print(F(" hw_overrun=")); out.print(c.hwOverrun);
    out.print(F(" queue_drop=")); out.print(c.queueDrop);
    out.print(F(" printed=")); out.print(c.printed);
    out.print(F(" int_missed=")); out.print(c.intMissed);
    out.print(F(" spi_errors=")); out.print(c.spiErrors);
    out.print(F(" queue_peak=")); out.print(qPeak);
    out.print(F(" merr=")); out.print(c.merr);
    out.print(F(" min_free_ram=")); out.println(minFreeRam());
    out.print(F("# STREAM max_flush_us=")); out.print(tFlush);
    out.print(F(" max_gap_us=")); out.print(tGap);
    out.print(F(" max_service_us=")); out.println(tService);
    out.print(F("# STREAM CAN2 TEC=")); out.print(st.can2Tec);
    out.print(F(" REC=")); out.print(st.can2Rec);
    out.print(F(" EFLG=0x")); out.print(st.can2EflgOr, HEX);
    out.print(F(" CANINTF=0x")); out.print(st.can2IntfOr, HEX);
    out.print(F(" TXB0CTRL_OR=0x")); out.print(st.txCtrlOr, HEX);
    out.print(F(" attempts_flagged=")); out.print(st.attemptsFlagged);
    out.print(F(" tx_ok=")); out.print(st.txOk);
    out.print(F(" flag_races=")); out.print(st.flagRaces);
    out.print(F(" stopped=")); out.println(stopped ? F("CONFIG") : F("FAIL"));
    out.print(F("STREAM: ")); out.print(pass ? F("PASS") : F("FAIL"));
    out.print(F(" loss_free=")); out.println(lost == 0 && c.queueDrop == 0 ? F("YES") : F("NO"));
    capturing = wasCapturing;
}

// Presets the core time counters ~3 s before their 32-bit wrap so timestamp
// and interval handling across overflow can be verified on hardware.
void wrapTest() {
    const uint8_t sreg = SREG;
    cli();
    timer0_millis = 0xFFFFFFFFUL - 3000UL;
    timer0_overflow_count = (1UL << 22) - 3000UL;  // micros() = ovf * 1024 us at 16 MHz
    SREG = sreg;
    const uint32_t now = millis();
    lastHealth = lastStats = lastBinStats = now;
    paceReanchor();
    resetCounters();
    Msg m;
    out.print(F("# WRAPTEST millis=")); out.print(now);
    out.print(F(" micros=")); out.println(micros());
}
#endif

void enterBinary() {
    { Msg m; out.println(F("# MODE BINARY")); }
    Serial.flush();
    outMode = OutMode::BINARY;
    uint8_t rec[omproto::MAX_RECORD];
    const uint8_t n = omproto::encodeHello(rec, recSeq, OPENMOTO_SNIFFER_BENCH, SPI_HZ, BAUD,
                                           QUEUE_SIZE, micros());
    emitRecordReliable(rec, n);
    lastBinStats = millis();
}

#if OPENMOTO_SNIFFER_BENCH
bool parseArgs(char *args, uint8_t &dlc, uint32_t &ms) {
    char *end = nullptr;
    const unsigned long d = strtoul(args, &end, 10);
    if (end == args) return false;
    char *end2 = nullptr;
    const unsigned long t = strtoul(end, &end2, 10);
    if (end2 == end || (d != 0 && d != 8) || t < 100 || t > 60000) return false;
    dlc = uint8_t(d); ms = t;
    return true;
}
#endif
void runCommand(char *cmd) {
    char *args = strchr(cmd, ' ');
    if (args) *args++ = 0; else args = cmd + strlen(cmd);
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
    } else if (!strcmp(cmd, "ASCII")) {
        if (outMode == OutMode::BINARY) {
            Msg m; out.println(F("# MODE ASCII"));
        }
        while (!outputIdle()) serviceAll();
        outMode = OutMode::ASCII;
        Msg m; out.println(F("# MODE ASCII"));
    } else if (!strcmp(cmd, "BINARY")) {
        if (outMode != OutMode::BINARY) enterBinary();
    } else if (!strcmp(cmd, "COUNT")) {
        while (!outputIdle()) serviceAll();
        if (outMode == OutMode::BINARY) { Msg m; out.println(F("# MODE COUNT")); }
        outMode = OutMode::COUNT;
        Msg m; out.println(F("# MODE COUNT"));
#if OPENMOTO_SNIFFER_BENCH
    } else if (!strcmp(cmd, "BENCH")) {
        benchRun();
    } else if (!strcmp(cmd, "FLOOD") || !strcmp(cmd, "STREAM")) {
        uint8_t dlc; uint32_t ms;
        if (!parseArgs(args, dlc, ms)) { Msg m; out.println(F("# ERR usage: FLOOD|STREAM <0|8> <100..60000 ms>")); return; }
        if (cmd[0] == 'F') floodRun(dlc, ms); else streamRun(dlc, ms);
    } else if (!strcmp(cmd, "WRAPTEST")) {
        wrapTest();
#endif
    } else {
        Msg m; out.print(F("# ERR unknown command: ")); out.println(cmd);
    }
}
void pollCommands() {
    static char buf[32];
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

// ---- Initialization -------------------------------------------------------
void result(uint8_t node, const __FlashStringHelper *step, bool ok) {
    out.print(F("MCP2515 #")); out.print(node); out.print(' '); out.print(step);
    out.println(ok ? F(": PASS") : F(": FAIL"));
}
bool bitrateOk(MCP2515 &can, uint8_t cs) {
    return can.setBitrate(CAN_500KBPS, MCP_8MHZ) == MCP2515::ERROR_OK &&
           readReg(cs, R_CNF1) == CNF1_500K && readReg(cs, R_CNF2) == CNF2_500K && readReg(cs, R_CNF3) == CNF3_500K;
}
// Burst write/read of the 8 TXB0 data registers (CONFIG mode only: nothing can
// be transmitted) with patterns that toggle every MISO/MOSI bit, then zeroed.
uint16_t spiStress(uint8_t cs) {
    uint16_t errors = 0;
    uint8_t w[8], r[8];
    for (uint16_t round = 0; round < 256; ++round) {
        for (uint8_t i = 0; i < 8; ++i) {
            const uint8_t base = (round & 3) == 0 ? 0x55 : (round & 3) == 1 ? 0xAA
                               : (round & 3) == 2 ? 0x00 : 0xFF;
            w[i] = uint8_t(base ^ (round >> 2) ^ (i * 0x25) ^ ((i & 1) ? 0xFF : 0x00));
        }
        writeRegs(cs, R_TXB0D0, w, 8);
        readRegs(cs, R_TXB0D0, r, 8);
        for (uint8_t i = 0; i < 8; ++i) if (r[i] != w[i]) ++errors;
    }
    memset(w, 0, 8);
    writeRegs(cs, R_TXB0D0, w, 8);
    readRegs(cs, R_TXB0D0, r, 8);
    for (uint8_t i = 0; i < 8; ++i) if (r[i]) ++errors;
    return errors;
}
bool initialize() {
    Msg m;
    out.println(F("Init - SPI, bitrate, filters, LISTEN_ONLY"));
    if (!csMappingOk()) { out.println(F("CS/INT port mapping: FAIL")); return false; }
    const bool config1 = can1.reset() == MCP2515::ERROR_OK && can1.setConfigMode() == MCP2515::ERROR_OK &&
                         mode(CS1) == MODE_CONFIG;
    const bool config2 = can2.reset() == MCP2515::ERROR_OK && can2.setConfigMode() == MCP2515::ERROR_OK &&
                         mode(CS2) == MODE_CONFIG;
    bool spi1 = config1, spi2 = config2;
    for (uint8_t round = 0; round < 2; ++round) {  // cross-selection check (Phase 2/3A)
        const uint8_t a = round == 0 ? 0x15 : 0x2A, b = round == 0 ? 0x2A : 0x15;
        if (config1) writeReg(CS1, R_CNF1, a);
        if (config2) writeReg(CS2, R_CNF1, b);
        spi1 &= readReg(CS1, R_CNF1) == a;
        spi2 &= readReg(CS2, R_CNF1) == b;
    }
    result(1, F("SPI"), spi1); result(2, F("SPI"), spi2);
    if (!spi1 || !spi2) return false;
    const uint16_t e1 = spiStress(CS1), e2 = spiStress(CS2);
    out.print(F("# SPI stress hz=")); out.print(SPI_HZ);
    out.print(F(" bytes_per_chip=2056 errors1=")); out.print(e1);
    out.print(F(" errors2=")); out.println(e2);
    result(1, F("SPI stress"), e1 == 0); result(2, F("SPI stress"), e2 == 0);
    if (e1 || e2) return false;
    const bool rate1 = bitrateOk(can1, CS1);
    result(1, F("initialized 500k/8MHz"), rate1);
    if (!rate1) return false;
#if OPENMOTO_SNIFFER_BENCH
    const bool rate2 = bitrateOk(can2, CS2);
    result(2, F("initialized 500k/8MHz"), rate2);
    if (!rate2) return false;
#else
    writeReg(CS2, R_CNF1, 0x00);
#endif
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
    paintStack();
    digitalWrite(53, HIGH); pinMode(53, OUTPUT);
    digitalWrite(CS1, HIGH); digitalWrite(CS2, HIGH);
    pinMode(CS1, OUTPUT); pinMode(CS2, OUTPUT);
    pinMode(INT1_PIN, INPUT_PULLUP); pinMode(INT2_PIN, INPUT_PULLUP);
    Serial.begin(BAUD); SPI.begin(); delay(500);
    {
        Msg m;
        out.println(F("OpenMoto Phase 3B CAN sniffer"));
#if OPENMOTO_SNIFFER_BENCH
        out.println(F("Variant: bench (CAN2 test traffic only on BENCH/FLOOD/STREAM)"));
#else
        out.println(F("Variant: sniffer (CAN2 held silent in CONFIG)"));
#endif
        out.print(F("Config: spi_hz=")); out.print(SPI_HZ);
        out.print(F(" baud=")); out.print(BAUD);
        out.print(F(" queue=")); out.print(QUEUE_SIZE);
        out.print(F(" protocol=")); out.print(omproto::VERSION);
        out.print(F(" pace_bps=")); out.println(uint32_t(OPENMOTO_TX_PACE_BPS));
        out.println(F("CAN1: LISTEN_ONLY 500000 bps, 8 MHz, CS D10, INT D2"));
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
    const int txFree = Serial.availableForWrite();
    Msg m;
    out.print(F("# serial_tx_free=")); out.print(txFree);
    out.print(F(" min_free_ram=")); out.println(minFreeRam());
    out.println(F("READY capture=OFF mode=ASCII; send HELP"));
}

void loop() {
    serviceCan1();
    flushOutput();
    pollCommands();
    periodic();
}
