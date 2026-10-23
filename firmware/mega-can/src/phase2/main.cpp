#include <Arduino.h>
#include <SPI.h>
#include <mcp2515.h>
#include <string.h>

#if OPENMOTO_MCP_CLOCK_MHZ != 8
#error "Phase 2 requires both module crystals confirmed as 8 MHz"
#endif

namespace {
constexpr uint8_t CS1 = 10, CS2 = 9;
constexpr uint32_t SPI_HZ = 1000000; // Conservative speed for breadboard jumpers.
constexpr uint32_t FRAME_TIMEOUT_MS = 250, QUIET_MS = 5;
// MCP2515 registers/instructions; raw access supplements the unmodified library.
constexpr uint8_t CANSTAT = 0x0E, CANCTRL = 0x0F, CNF1 = 0x2A;
constexpr uint8_t CANINTF = 0x2C, TXB0CTRL = 0x30;
constexpr uint8_t TXREQ = 0x08, TX_ERRORS = 0x70, TX0IF = 0x04;
MCP2515 can1(CS1, SPI_HZ, &SPI), can2(CS2, SPI_HZ, &SPI);
struct Node { MCP2515 &can; uint8_t cs; uint8_t number; };
Node nodes[] = {{can1, CS1, 1}, {can2, CS2, 2}};

uint8_t readReg(const Node &n, uint8_t address) {
    SPI.beginTransaction(SPISettings(SPI_HZ, MSBFIRST, SPI_MODE0));
    digitalWrite(n.cs, LOW);
    SPI.transfer(0x03); SPI.transfer(address);
    uint8_t value = SPI.transfer(0);
    digitalWrite(n.cs, HIGH);
    SPI.endTransaction();
    return value;
}
void writeReg(const Node &n, uint8_t address, uint8_t value) {
    SPI.beginTransaction(SPISettings(SPI_HZ, MSBFIRST, SPI_MODE0));
    digitalWrite(n.cs, LOW);
    SPI.transfer(0x02); SPI.transfer(address); SPI.transfer(value);
    digitalWrite(n.cs, HIGH);
    SPI.endTransaction();
}
void bitModify(const Node &n, uint8_t address, uint8_t mask, uint8_t value) {
    SPI.beginTransaction(SPISettings(SPI_HZ, MSBFIRST, SPI_MODE0));
    digitalWrite(n.cs, LOW);
    SPI.transfer(0x05); SPI.transfer(address); SPI.transfer(mask); SPI.transfer(value);
    digitalWrite(n.cs, HIGH);
    SPI.endTransaction();
}

struct Stats {
    uint16_t requested = 0, queued = 0, transmitted = 0, received = 0, verified = 0;
    uint16_t timeouts = 0, missing = 0, payload = 0, duplicates = 0;
    uint16_t unexpected = 0, sequence = 0, apiErrors = 0, controllerErrors = 0;
    uint8_t maxTec[2] = {}, maxRec[2] = {}, eflg[2] = {}, errint[2] = {};
    uint32_t start = 0, duration = 0;
};
uint8_t seen[125]; // 1000 sequence bits; reset for each test direction.
uint32_t suiteStart;

void nodeLabel(const Node &n) { Serial.print(F("MCP2515 #")); Serial.print(n.number); }
void result(const Node &n, const __FlashStringHelper *step, bool ok) {
    nodeLabel(n); Serial.print(' '); Serial.print(step);
    Serial.println(ok ? F(": PASS") : F(": FAIL"));
}

bool sampleErrors(Stats &s) {
    bool ok = true;
    for (uint8_t i = 0; i < 2; ++i) {
        uint8_t flags = nodes[i].can.getErrorFlags();
        uint8_t ints = nodes[i].can.getInterrupts() & 0xA0; // MERRF, ERRIF
        uint8_t tec = nodes[i].can.errorCountTX(), rec = nodes[i].can.errorCountRX();
        s.eflg[i] |= flags; s.errint[i] |= ints;
        if (tec > s.maxTec[i]) s.maxTec[i] = tec;
        if (rec > s.maxRec[i]) s.maxRec[i] = rec;
        if (flags || ints || tec || rec || (readReg(nodes[i], CANSTAT) & 0xE0) != 0) {
            ++s.controllerErrors;
            ok = false;
        }
    }
    return ok;
}

bool stopControllers() {
    bool stopped = true;
    // Abort pending NORMAL-mode retransmissions before printing lengthy reports.
    for (auto &n : nodes) bitModify(n, CANCTRL, 0x10, 0x10); // ABAT
    for (auto &n : nodes) {
        const uint32_t start = millis();
        while ((readReg(n, TXB0CTRL) & TXREQ) && millis() - start < 10) {}
        const bool ok = n.can.setConfigMode() == MCP2515::ERROR_OK &&
                        (readReg(n, CANSTAT) & 0xE0) == 0x80;
        stopped &= ok;
        nodeLabel(n); Serial.println(ok ? F(" stopped: CONFIG") : F(" stop: FAIL; disconnect USB"));
    }
    return stopped;
}
void halt() { while (true) delay(1000); }
void finalResult(bool ok) {
    Serial.print(F("Total duration ms: ")); Serial.println(millis() - suiteStart);
    Serial.println(ok ? F("PHASE 2: PASS") : F("PHASE 2: FAIL"));
    halt();
}
void field(const __FlashStringHelper *label, uint32_t value) {
    Serial.print(label); Serial.println(value);
}
void report(const Stats &s) {
    field(F("Frames requested: "), s.requested);
    field(F("Frames queued: "), s.queued);
    field(F("Frames transmitted (TX complete): "), s.transmitted);
    field(F("Frames received: "), s.received);
    field(F("Frames verified: "), s.verified);
    field(F("Timeouts: "), s.timeouts);
    field(F("Missing frames: "), s.missing);
    field(F("Payload mismatches: "), s.payload);
    field(F("Duplicate frames: "), s.duplicates);
    field(F("Unexpected frames: "), s.unexpected);
    field(F("Sequence errors: "), s.sequence);
    field(F("SPI/library/TX API errors: "), s.apiErrors);
    field(F("CAN controller error observations: "), s.controllerErrors);
    for (uint8_t i = 0; i < 2; ++i) {
        nodeLabel(nodes[i]); Serial.print(F(" TEC=")); Serial.print(nodes[i].can.errorCountTX());
        Serial.print(F(" REC=")); Serial.print(nodes[i].can.errorCountRX());
        Serial.print(F(" sampled_peak_TEC=")); Serial.print(s.maxTec[i]);
        Serial.print(F(" sampled_peak_REC=")); Serial.print(s.maxRec[i]);
        Serial.print(F(" EFLG_OR=0x")); Serial.print(s.eflg[i], HEX);
        Serial.print(F(" ERRINT_OR=0x")); Serial.println(s.errint[i], HEX);
    }
    field(F("Duration ms: "), s.duration);
}

bool initialize() {
    Serial.println(F("Test A - independent SPI and initialization"));
    bool config[2];
    for (uint8_t i = 0; i < 2; ++i) {
        config[i] = nodes[i].can.reset() == MCP2515::ERROR_OK &&
                    nodes[i].can.setConfigMode() == MCP2515::ERROR_OK &&
                    (readReg(nodes[i], CANSTAT) & 0xE0) == 0x80;
    }
    bool spi[2] = {config[0], config[1]};
    // Distinct, swapped read/write challenges detect stuck MISO, absent chips,
    // shared/shorted CS and cross-selection. Only write CNF1 while in CONFIG.
    for (uint8_t round = 0; round < 2; ++round) {
        const uint8_t a = round == 0 ? 0x15 : 0x2A;
        const uint8_t b = round == 0 ? 0x2A : 0x15;
        if (config[0]) writeReg(nodes[0], CNF1, a);
        if (config[1]) writeReg(nodes[1], CNF1, b);
        spi[0] &= readReg(nodes[0], CNF1) == a;
        spi[1] &= readReg(nodes[1], CNF1) == b;
    }
    for (uint8_t i = 0; i < 2; ++i) result(nodes[i], F("SPI"), spi[i]);
    if (!spi[0] || !spi[1]) return false;
    bool initialized[2];
    for (uint8_t i = 0; i < 2; ++i) {
        initialized[i] = nodes[i].can.setBitrate(CAN_500KBPS, MCP_8MHZ) == MCP2515::ERROR_OK &&
            readReg(nodes[i], 0x2A) == 0x00 && readReg(nodes[i], 0x29) == 0x90 &&
            readReg(nodes[i], 0x28) == 0x82;
        result(nodes[i], F("initialized 500k/8MHz"), initialized[i]);
    }
    if (!initialized[0] || !initialized[1]) return false;
    bool normal[2];
    for (uint8_t i = 0; i < 2; ++i) {
        normal[i] = nodes[i].can.setNormalMode() == MCP2515::ERROR_OK &&
                    (readReg(nodes[i], CANSTAT) & 0xE0) == 0 &&
                    (readReg(nodes[i], CANCTRL) & 0xE8) == 0;
        result(nodes[i], F("NORMAL"), normal[i]);
    }
    return normal[0] && normal[1];
}

can_frame makeFrame(uint16_t id, uint8_t stage, uint8_t direction, uint32_t seq) {
    can_frame f = {};
    f.can_id = id; f.can_dlc = 8; // No extended, RTR, or error flags.
    f.data[0] = 0x4F; f.data[1] = 0x4D; f.data[2] = stage; f.data[3] = direction;
    for (uint8_t i = 0; i < 4; ++i) f.data[4 + i] = (seq >> (8 * i)) & 0xFF;
    return f;
}

bool receiveExpected(Node &receiver, const can_frame &expected, uint16_t seq,
                     uint16_t count, bool &got, Stats &s) {
    can_frame frame = {};
    const auto rc = receiver.can.readMessage(&frame);
    if (rc == MCP2515::ERROR_NOMSG) return true;
    if (rc != MCP2515::ERROR_OK) { ++s.apiErrors; return false; }
    ++s.received;
    if (frame.can_id != expected.can_id || frame.can_dlc != 8) {
        ++s.unexpected; return false;
    }
    if (frame.data[0] != 0x4F || frame.data[1] != 0x4D ||
        frame.data[2] != expected.data[2] || frame.data[3] != expected.data[3]) {
        ++s.payload; return false;
    }
    uint32_t rxSeq = 0;
    for (uint8_t i = 0; i < 4; ++i) rxSeq |= uint32_t(frame.data[4 + i]) << (8 * i);
    if (rxSeq >= count) { ++s.unexpected; ++s.sequence; return false; }
    const uint8_t mask = 1U << (rxSeq & 7);
    if (seen[rxSeq >> 3] & mask) { ++s.duplicates; return false; }
    if (rxSeq != seq) { ++s.sequence; return false; }
    if (memcmp(frame.data, expected.data, 8) != 0) { ++s.payload; return false; }
    seen[rxSeq >> 3] |= mask;
    ++s.verified; got = true;
    return true;
}

bool rejectSenderRx(Node &sender, Stats &s) {
    can_frame f = {};
    auto rc = sender.can.readMessage(&f);
    if (rc == MCP2515::ERROR_NOMSG) return true;
    if (rc == MCP2515::ERROR_OK) ++s.unexpected; // NORMAL must not self-receive.
    else ++s.apiErrors;
    return false;
}

bool exchange(Node &sender, Node &receiver, uint16_t id, uint8_t stage, uint8_t direction,
              uint16_t count, Stats &s) {
    memset(seen, 0, sizeof(seen));
    s.requested = count; s.start = millis();
    if (!sampleErrors(s)) return false;
    // Stale/unexpected traffic is an error, never silently discarded.
    if (sender.can.checkReceive() || receiver.can.checkReceive()) {
        ++s.unexpected; return false;
    }
    for (uint16_t seq = 0; seq < count; ++seq) {
        const can_frame expected = makeFrame(id, stage, direction, seq);
        sender.can.clearTXInterrupts();
        if (readReg(sender, TXB0CTRL) & TXREQ) { ++s.apiErrors; return false; }
        if (sender.can.sendMessage(MCP2515::TXB0, &expected) != MCP2515::ERROR_OK) {
            ++s.apiErrors; return false;
        }
        ++s.queued;
        const uint32_t start = millis();
        bool got = false, completed = false;
        while (!got || !completed) {
            if (!receiveExpected(receiver, expected, seq, count, got, s) ||
                !rejectSenderRx(sender, s)) return false;
            const uint8_t ctrl = readReg(sender, TXB0CTRL);
            if (ctrl & TX_ERRORS) { ++s.controllerErrors; return false; }
            if (!completed && !(ctrl & TXREQ) && (readReg(sender, CANINTF) & TX0IF)) {
                completed = true; ++s.transmitted;
            }
            if (!sampleErrors(s)) return false;
            if (got && completed) break;
            if (millis() - start >= FRAME_TIMEOUT_MS) {
                ++s.timeouts;
                if (!got) ++s.missing;
                return false;
            }
            delay(1);
        }
        // Service both controllers during pacing and catch trailing duplicates.
        const uint32_t quietStart = millis();
        do {
            if (!receiveExpected(receiver, expected, seq, count, got, s) ||
                !rejectSenderRx(sender, s) || !sampleErrors(s)) return false;
            delay(1);
        } while (millis() - quietStart < QUIET_MS);
    }
    return true;
}

bool runTest(uint8_t senderIndex, uint8_t stage, uint16_t count) {
    Serial.print(F("Test ")); Serial.print(stage == 1 ? 'B' : stage == 2 ? 'C' : 'D');
    Serial.print(F(" CAN")); Serial.print(senderIndex + 1);
    Serial.print(F(" -> CAN")); Serial.print(2 - senderIndex);
    Serial.print(F(" requested=")); Serial.println(count);
    Stats s;
    bool ok = exchange(nodes[senderIndex], nodes[1 - senderIndex],
                       senderIndex == 0 ? 0x123 : 0x456, stage, senderIndex + 1, count, s);
    s.duration = millis() - s.start;
    ok = ok && s.queued == count && s.transmitted == count &&
         s.received == count && s.verified == count;
    if (!ok) stopControllers();
    report(s);
    if (!ok) Serial.println(F("Remaining frames NOT ATTEMPTED; test stopped on first failure."));
    Serial.println(ok ? F("TEST: PASS") : F("TEST: FAIL"));
    return ok;
}
} // namespace

void setup() {
    // Set latches HIGH before OUTPUT to avoid a transient selection pulse.
    digitalWrite(53, HIGH); pinMode(53, OUTPUT);
    digitalWrite(CS1, HIGH); digitalWrite(CS2, HIGH);
    pinMode(CS1, OUTPUT); pinMode(CS2, OUTPUT);
    pinMode(2, INPUT_PULLUP); pinMode(3, INPUT_PULLUP);
    Serial.begin(115200); SPI.begin(); delay(500);
    suiteStart = millis();
    Serial.println(F("OpenMoto Phase 2"));
    Serial.println(F("Board: ELEGOO Mega 2560"));
    Serial.println(F("CAN Controllers: 2 x MCP2515"));
    Serial.println(F("Oscillators: 8 MHz; Bitrate: 500000; SPI: 1000000"));
    Serial.println(F("ISOLATED BENCH ONLY; no OBD, ECU, motorcycle or external 12V"));
    if (!initialize()) { stopControllers(); finalResult(false); }
    if (!runTest(0, 1, 100)) finalResult(false);
    if (!runTest(1, 2, 100)) finalResult(false);
    if (!runTest(0, 3, 1000)) finalResult(false);
    if (!runTest(1, 4, 1000)) finalResult(false);
    // Capture any final errors before disabling both CAN controllers.
    Stats finalStats;
    bool healthy = sampleErrors(finalStats);
    healthy = stopControllers() && healthy;
    if (!healthy) report(finalStats);
    finalResult(healthy);
}
void loop() {} // Run once per reset; no continuous bus transmission.
