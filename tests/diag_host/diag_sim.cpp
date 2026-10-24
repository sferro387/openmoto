// OpenMoto Phase 4 host simulator: drives the REAL firmware diagnostic core
// (firmware/mega-can/src/diag/diag_core.h) against an in-process mock ECU.
//
// This is the exact logic that runs on the Mega: the same Shell, allowlist,
// ISO-TP state machine, confirmation codes, timeouts, rate limits and emergency
// stop. Only the Hal is replaced. Nothing here talks to real hardware.
//
// Build with OPENMOTO_DIAG_ALLOW_TX=1 so the transmit path (the diag_readonly
// environment) is exercised end to end. The mock ECU is a deterministic,
// read-only UDS responder; it also has fault modes so the host CLI and tests
// can check every control and error path offline.
//
// Usage: diag_sim <scenario> [--trace]
//   Scenarios are listed in SCENARIOS[] below and in tests/test_diag.py.
//
// It reads commands either from a built-in script (per scenario) or, with
// "--stdin", from standard input, so tests/test_diag.py and diag_cli.py can
// drive it line by line. A simulated clock advances on demand, so the 6 s
// timeouts resolve instantly rather than in real time.

#include <cstdint>
#include <cstdio>
#include <cstring>
#include <deque>
#include <string>
#include <vector>

#ifndef OPENMOTO_DIAG_ALLOW_TX
#define OPENMOTO_DIAG_ALLOW_TX 1
#endif
#include "../../firmware/mega-can/src/diag/diag_core.h"

namespace {

// ---------------------------------------------------------------------------
// Mock ECU: a minimal ISO-TP + UDS responder. Read-only by construction; it
// implements only positive/negative replies to allowlisted services and has
// explicit fault modes. It never mutates anything and has no write services.
// ---------------------------------------------------------------------------
struct MockEcu {
    enum Mode {
        M_NORMAL,        // answers correctly over single- and multi-frame ISO-TP
        M_SILENT,        // never answers (tests NO_RESPONSE)
        M_NRC_7F,        // always NRC 0x31 requestOutOfRange (tests NEGATIVE)
        M_PENDING_OK,    // two 0x78 responsePending then a positive reply
        M_PENDING_FOREVER,  // keeps sending 0x78 (tests TOO_MANY_PENDING)
        M_BAD_SID,       // positive reply with the wrong service id (tests INVALID)
        M_BAD_ECHO,      // ReadDID reply echoes a different DID (tests INVALID)
        M_BAD_SEQ,       // multi-frame with a wrong consecutive-frame SN
        M_MULTI_NO_CF,   // sends a first frame then stops (tests CF_TIMEOUT)
        M_TRUNC_SF,      // single frame claims more length than the DLC holds
        M_WRONG_ID,      // answers on a different CAN id (frames ignored)
        M_FLOODER,       // emits unrelated frames on a 3rd id (ignored, bounded)
        M_LONG_VIN,      // 17-byte VIN via multi-frame (exercises flow control)
        M_SERVICE,       // Phase 7: service DIDs with documented lengths (multi-frame)
    };

    Mode mode = M_NORMAL;
    uint16_t txId = 0, rxId = 0;   // from the tester's point of view: ECU listens on txId, answers on rxId
    int16_t txExt = -1, rxExt = -1;
    std::deque<diag::Frame> out;   // frames queued toward the tester

    void configure(uint16_t testerTx, uint16_t testerRx, int16_t te, int16_t re) {
        txId = testerTx; rxId = testerRx; txExt = te; rxExt = re; out.clear();
    }

    // The tester transmitted a frame; parse ISO-TP (single frame only from the
    // tester in read-only mode) and queue the reply/replies.
    void onTesterFrame(const diag::Frame &f) {
        if (f.id != txId) return;  // not addressed to this ECU
        uint8_t o = 0;
        if (txExt >= 0) {
            if (f.dlc < 1 || f.data[0] != uint8_t(txExt)) return;
            o = 1;
        }
        if (f.dlc <= o) return;
        const uint8_t pci = f.data[o];
        if ((pci >> 4) == 3) return;        // flow control from tester: consume, no reply
        if ((pci >> 4) != 0) return;        // only single-frame requests are expected
        const uint8_t len = pci & 0x0F;
        if (len == 0 || o + 1 + len > f.dlc) return;
        uint8_t req[8];
        memcpy(req, f.data + o + 1, len);
        reply(req, len);
    }

    void reply(const uint8_t *req, uint8_t n) {
        switch (mode) {
        case M_SILENT: case M_WRONG_ID: return;  // (M_WRONG_ID answers on wrong id below)
        case M_FLOODER:
            // noise handled by tick(); still answer correctly
            positiveOrNrc(req, n);
            return;
        default:
            positiveOrNrc(req, n);
        }
    }

    void positiveOrNrc(const uint8_t *req, uint8_t n) {
        (void)n;
        uint8_t body[128]; uint16_t bl = 0;
        const uint8_t sid = req[0];
        switch (mode) {
        case M_NRC_7F: emitNegative(sid, 0x31); return;
        case M_PENDING_OK:
            // Two responsePending frames, then the correct positive reply. The
            // firmware consumes one per P2*/P2 wait window.
            emitNegative(sid, 0x78);
            emitNegative(sid, 0x78);
            break;  // fall through to emit the positive reply below
        case M_PENDING_FOREVER:
            for (int i = 0; i < 6; ++i) emitNegative(sid, 0x78);
            return;  // never positive: firmware gives up after MAX_PENDING
        case M_BAD_SID:
            body[bl++] = uint8_t(sid + 0x40 + 1);  // wrong positive SID
            body[bl++] = 0x00;
            emitPayload(body, bl);
            return;
        default: break;
        }
        // Normal positive responses for the allowlisted services.
        if (sid == 0x3E) {                         // TesterPresent
            body[bl++] = 0x7E; body[bl++] = 0x00;
        } else if (sid == 0x22) {                  // ReadDataByIdentifier
            uint16_t did = uint16_t(req[1]) << 8 | req[2];
            if (mode == M_BAD_ECHO) did ^= 0x0001;
            body[bl++] = 0x62; body[bl++] = uint8_t(did >> 8); body[bl++] = uint8_t(did);
            // Modes that must exercise multi-frame reassembly use a long (VIN-sized) payload.
            if (mode == M_LONG_VIN || mode == M_MULTI_NO_CF || mode == M_BAD_SEQ)
                for (uint8_t i = 0; i < 17; ++i) body[bl++] = uint8_t('A' + i % 26);
            else if (mode == M_SERVICE) {
                // Phase 7: return each service DID with its documented total length
                // (incl. the 3-byte 62/DID header). Deterministic placeholder data.
                uint8_t dataLen = 2;  // default
                if (did == 0xE119) dataLen = 4;       // 7-byte resp: 4-byte BE odometer
                else if (did == 0xE12B) dataLen = 7;  // 10-byte resp: 7 clock bytes
                else if (did == 0xE12C) dataLen = 4;  // 7-byte resp: day/month/year(2)
                else if (did == 0xE12D) dataLen = 2;  // 5-byte resp: 2-byte BE distance
                for (uint8_t i = 0; i < dataLen; ++i) body[bl++] = uint8_t(0x10 + i);
            }
            else { body[bl++] = 0xDE; body[bl++] = 0xAD; }
        } else if (sid == 0x19 && req[1] == 0x01) { // reportNumberOfDTCByStatusMask
            // 59 01 <statusAvailabilityMask> <DTCFormatIdentifier> <countHi> <countLo>
            body[bl++] = 0x59; body[bl++] = 0x01; body[bl++] = 0xFF;
            body[bl++] = 0x01; body[bl++] = 0x00; body[bl++] = 0x00;
        } else if (sid == 0x19 && req[1] == 0x02) { // reportDTCByStatusMask
            body[bl++] = 0x59; body[bl++] = 0x02; body[bl++] = 0xFF;  // mask echo
            body[bl++] = 0x12; body[bl++] = 0x34; body[bl++] = 0x56; body[bl++] = 0x08;  // one DTC
        } else {
            emitNegative(sid, 0x11);  // serviceNotSupported (should never happen; allowlist gates)
            return;
        }
        emitPayload(body, bl);
    }

    void emitNegative(uint8_t sid, uint8_t nrc) {
        uint8_t b[3] = {0x7F, sid, nrc};
        emitPayload(b, 3);
    }

    // Wrap a UDS payload in ISO-TP frames toward the tester (rxId).
    void emitPayload(const uint8_t *p, uint16_t n) {
        const uint16_t idForReply = (mode == M_WRONG_ID) ? uint16_t(rxId ^ 0x001) : rxId;
        const uint8_t head = rxExt >= 0 ? 1 : 0;
        if (mode == M_TRUNC_SF) {
            diag::Frame f; makeFrame(f, idForReply, head);
            uint8_t k = head;
            f.data[k++] = 0x07;  // claims 7 bytes...
            f.data[k++] = p[0];  // ...but only one is present
            f.dlc = k;
            out.push_back(f);
            return;
        }
        if (n + head + 1 <= 8 && mode != M_BAD_SEQ && mode != M_MULTI_NO_CF) {
            diag::Frame f; makeFrame(f, idForReply, head);
            uint8_t k = head;
            f.data[k++] = uint8_t(n);          // SF PCI
            for (uint16_t i = 0; i < n; ++i) f.data[k++] = p[i];
            while (k < 8) f.data[k++] = 0x00;
            f.dlc = 8;
            out.push_back(f);
            return;
        }
        // Multi-frame. First frame then consecutive frames.
        diag::Frame ff; makeFrame(ff, idForReply, head);
        uint8_t k = head;
        ff.data[k++] = uint8_t(0x10 | ((n >> 8) & 0x0F));
        ff.data[k++] = uint8_t(n & 0xFF);
        uint16_t sent = 0;
        while (k < 8 && sent < n) ff.data[k++] = p[sent++];
        ff.dlc = 8;
        out.push_back(ff);
        if (mode == M_MULTI_NO_CF) return;  // stop: tester should time out waiting for CF
        uint8_t sn = 1;
        while (sent < n) {
            diag::Frame cf; makeFrame(cf, idForReply, head);
            uint8_t j = head;
            uint8_t thisSn = uint8_t(sn & 0x0F);
            if (mode == M_BAD_SEQ && sn == 1) thisSn = 0x0F;  // corrupt the first CF SN
            cf.data[j++] = uint8_t(0x20 | thisSn);
            while (j < 8 && sent < n) cf.data[j++] = p[sent++];
            while (j < 8) cf.data[j++] = 0x00;
            cf.dlc = 8;
            out.push_back(cf);
            ++sn;
        }
    }

    void makeFrame(diag::Frame &f, uint16_t id, uint8_t head) {
        memset(&f, 0, sizeof(f));
        f.id = id; f.dlc = 0;
        if (head) f.data[0] = uint8_t(rxExt);
    }
};

// ---------------------------------------------------------------------------
// Host Hal: bridges diag::Hal to the mock ECU and a simulated clock/serial.
// ---------------------------------------------------------------------------
class SimHal : public diag::Hal {
public:
    MockEcu &ecu;
    uint32_t clock = 1000;
    bool active = false;            // NORMAL mode entered?
    bool silent = true;             // LISTEN_ONLY?
    uint16_t rxFilter = 0x7FF;
    std::deque<diag::Frame> rx;     // frames available to the tester
    std::deque<diag::Frame> txPending;  // frames the tester queued this tick
    std::string input;              // pending serial input
    std::string output;             // captured serial output
    diag::TxState lastTx = diag::TXS_NONE;
    bool failEnterActive = false;   // fault injection: canEnterActive returns false
    bool failSilence = false;       // fault injection: canSilence returns false
    bool failTransmit = false;      // fault injection: canTransmit returns false
    bool noAck = false;             // fault injection: frame leaves but is never ACKed (one-shot TX fail)

    explicit SimHal(MockEcu &e) : ecu(e) {}

    void feed(const std::string &line) { input += line; input += '\n'; }
    void advance(uint32_t ms) { clock += ms; }

    uint32_t nowMs() override { return clock; }
    int readChar() override {
        if (input.empty()) return -1;
        char c = input.front();
        input.erase(input.begin());
        return (unsigned char)c;
    }
    void putChar(char c) override { output.push_back(c); }

    bool canSilence() override {
        if (failSilence) return false;
        active = false; silent = true; lastTx = diag::TXS_NONE;
        txPending.clear();
        return true;
    }
    bool canSetRxId(uint16_t id) override {
        rxFilter = id; silent = true; active = false;
        return true;
    }
    bool canEnterActive() override {
        if (failEnterActive) return false;
        active = true; silent = false; lastTx = diag::TXS_NONE;
        return true;
    }
    bool canTransmit(const diag::Frame &f) override {
        if (!active || failTransmit) return false;
        if (noAck) {
            // One-shot transmit with no acknowledging node: the frame is queued
            // but the single attempt fails (TXERR). It is NOT retransmitted, so
            // there is no error-passive/bus-off storm (the 0x130 failure mode).
            lastTx = diag::TXS_FAILED;
            return true;
        }
        ecu.onTesterFrame(f);
        lastTx = diag::TXS_SENT;  // one-shot send completes immediately in the sim
        // Deliver the ECU's queued replies to the tester's rx (respecting the filter).
        deliver();
        return true;
    }
    diag::TxState canTxState() override { return lastTx; }
    bool canReceive(diag::Frame &f) override {
        if (rx.empty()) return false;
        f = rx.front(); rx.pop_front();
        return true;
    }

private:
    // Forward only frames matching the tester's current acceptance filter, exactly
    // as the MCP2515 hardware would. Everything else is simply not received.
    void deliver() {
        for (const auto &f : ecu.out)
            if (f.id == rxFilter) rx.push_back(f);
        ecu.out.clear();
    }
};

// ---------------------------------------------------------------------------
// Driver: runs a shell against the sim, advancing the simulated clock between
// polls so bounded waits complete. Each line is written, then the engine is
// pumped enough simulated time for an in-flight transaction to finish.
// ---------------------------------------------------------------------------
struct Driver {
    MockEcu ecu;
    SimHal hal{ecu};
    diag::Shell shell{hal};

    Driver() { shell.begin(); flush(); }
    // deferBegin=true constructs without running begin() yet (boot-fault scenarios).
    explicit Driver(bool deferBegin) { if (!deferBegin) { shell.begin(); flush(); } }

    void flush() { if (!hal.output.empty()) { std::fputs(hal.output.c_str(), stdout); hal.output.clear(); } }

    void send(const std::string &line) {
        maybeSyncEcuAddr(line);
        hal.feed(line);
        // Pump the shell; a transaction can take up to TOTAL_MS. Advance the
        // simulated clock in small steps and poll until it settles to idle.
        for (int i = 0; i < 400; ++i) {
            shell.poll();
            if (!hal.rx.empty() || !hal.input.empty()) { continue; }
            hal.advance(50);
        }
        flush();
    }

    // Keep the mock ECU's addressing in sync with an ADDR command typed into the
    // shell (used in --stdin mode, where scenarios don't pre-configure the ECU).
    void maybeSyncEcuAddr(const std::string &line) {
        char cmd[8];
        unsigned tx = 0, rx = 0, te = 0, re = 0;
        int n = std::sscanf(line.c_str(), "%7s %x %x %x %x", cmd, &tx, &rx, &te, &re);
        if (n >= 3 && (std::strcmp(cmd, "ADDR") == 0 || std::strcmp(cmd, "addr") == 0)) {
            if (n >= 5) ecu.configure(uint16_t(tx), uint16_t(rx), int16_t(te), int16_t(re));
            else ecu.configure(uint16_t(tx), uint16_t(rx), -1, -1);
        }
    }
};

// Helper: standard addressing used across scenarios (physical tester-style IDs
// are illustrative only — these are NOT claimed R18 addresses).
void setAddr(Driver &d) { d.send("ADDR 6F1 612"); d.send("PAD 00"); d.ecu.configure(0x6F1, 0x612, -1, -1); }

// Parse the ARMED line to recover the confirmation code and CONFIRM it.
std::string lastArmCode(const std::string &out) {
    auto pos = out.rfind("ARMED code=");
    if (pos == std::string::npos) return "";
    return out.substr(pos + 11, 4);
}

// Because the Driver prints and clears output, we re-capture per step for ARM.
void armConfirm(Driver &d, const std::string &req) {
    d.hal.feed("ARM " + req + "\n");
    for (int i = 0; i < 50 && !d.hal.input.empty(); ++i) d.shell.poll();
    std::string armed = d.hal.output;
    std::fputs(armed.c_str(), stdout); d.hal.output.clear();
    std::string code = lastArmCode(armed);
    if (code.size() == 4) d.send("CONFIRM " + code);
    else d.send("CONFIRM 0000");
}

}  // namespace

int main(int argc, char **argv) {
    std::string scenario = argc > 1 ? argv[1] : "stdin";

    if (scenario == "stdin") {
        Driver d;
        std::fputs("EOC\n", stdout);  // terminate the startup banner block
        std::fflush(stdout);
        std::string line;
        char buf[256];
        while (std::fgets(buf, sizeof(buf), stdin)) {
            line = buf;
            if (!line.empty() && line.back() == '\n') line.pop_back();
            d.send(line);
            // Explicit end-of-command marker so a line-framed host can tell when
            // the shell has finished reacting to this input line.
            std::fputs("EOC\n", stdout);
            std::fflush(stdout);
        }
        return 0;
    }

    if (scenario == "silence_fail_boot") {
        // canSilence() fails at boot: the client must latch off immediately.
        Driver d(true);         // defer begin()
        d.hal.failSilence = true;
        d.shell.begin();
        d.flush();
        d.send("STATUS");
        return 0;
    }

    // Scenario table (each prints a deterministic transcript to stdout).
    Driver d;
    if (scenario == "status") {
        d.send("STATUS");
    } else if (scenario == "policy") {
        d.send("POLICY 22F190");  // allow
        d.send("POLICY 2EF190");  // deny (write DID)
        d.send("POLICY 1101");    // deny (ECU reset)
        d.send("POLICY 2701");    // deny (security access)
        d.send("POLICY 31010203");// deny (routine control)
        d.send("POLICY 22F1A0");  // deny (DID not allowlisted)
    } else if (scenario == "read_did") {
        setAddr(d); armConfirm(d, "22F190");
    } else if (scenario == "tester_present") {
        setAddr(d); armConfirm(d, "3E00");
    } else if (scenario == "dtc_count") {
        setAddr(d); armConfirm(d, "190100");
    } else if (scenario == "dtc_list") {
        setAddr(d); armConfirm(d, "1902FF");
    } else if (scenario == "read_vin_multiframe") {
        setAddr(d); d.ecu.mode = MockEcu::M_LONG_VIN; armConfirm(d, "22F190");
    } else if (scenario == "negative") {
        setAddr(d); d.ecu.mode = MockEcu::M_NRC_7F; armConfirm(d, "22F190");
    } else if (scenario == "no_response") {
        setAddr(d); d.ecu.mode = MockEcu::M_SILENT; armConfirm(d, "22F190");
    } else if (scenario == "pending_forever") {
        setAddr(d); d.ecu.mode = MockEcu::M_PENDING_FOREVER; armConfirm(d, "22F190");
    } else if (scenario == "pending_ok") {
        setAddr(d); d.ecu.mode = MockEcu::M_PENDING_OK; armConfirm(d, "22F190");
    } else if (scenario == "bad_sid") {
        setAddr(d); d.ecu.mode = MockEcu::M_BAD_SID; armConfirm(d, "22F190");
    } else if (scenario == "bad_echo") {
        setAddr(d); d.ecu.mode = MockEcu::M_BAD_ECHO; armConfirm(d, "22F190");
    } else if (scenario == "cf_timeout") {
        setAddr(d); d.ecu.mode = MockEcu::M_MULTI_NO_CF; armConfirm(d, "22F190");
    } else if (scenario == "bad_sequence") {
        setAddr(d); d.ecu.mode = MockEcu::M_BAD_SEQ; armConfirm(d, "22F190");
    } else if (scenario == "wrong_confirm") {
        setAddr(d);
        d.send("ARM 22F190");
        d.send("CONFIRM FFFF");   // wrong code → disarm
    } else if (scenario == "deny_write") {
        setAddr(d);
        d.send("ARM 2EF19000");   // write DID → policy deny, never armed
    } else if (scenario == "deny_security") {
        setAddr(d);
        d.send("ARM 2701");       // security access → policy deny
    } else if (scenario == "require_addr") {
        d.send("ARM 22F190");     // no ADDR/PAD yet → not-configured
    } else if (scenario == "estop_bang") {
        setAddr(d);
        d.send("ARM 22F190");
        d.send("!");              // emergency stop
        d.send("STATUS");
        d.send("ARM 3E00");       // refused: estop latched
    } else if (scenario == "estop_command") {
        setAddr(d);
        d.send("ESTOP");
        d.send("ARM 3E00");
    } else if (scenario == "lockout") {
        setAddr(d); d.ecu.mode = MockEcu::M_SILENT;
        for (int i = 0; i < 3; ++i) { armConfirm(d, "22F190"); d.hal.advance(1100); }
        d.send("STATUS");
        armConfirm(d, "22F190");  // refused: lockout
    } else if (scenario == "rate_limit") {
        setAddr(d);
        armConfirm(d, "3E00");
        d.send("ARM 3E00");       // immediately: ok to arm
        // CONFIRM will hit the MIN_GAP rate limit if attempted too soon.
    } else if (scenario == "mode_fail") {
        setAddr(d); d.hal.failEnterActive = true; armConfirm(d, "22F190");
    } else if (scenario == "kombi_probe") {
        // Phase 5: the first real KOMBI read-only probe, extended addressing.
        d.send("ADDR 6F1 660 60 F1"); d.send("PAD 00");
        d.ecu.configure(0x6F1, 0x660, 0x60, 0xF1);
        armConfirm(d, "22F150");
    } else if (scenario == "kombi_probe_noack") {
        // Probe on a bus where no node ACKs (the 0x130 failure mode). One-shot TX
        // fails cleanly as TX_FAILED; no retransmission storm.
        d.send("ADDR 6F1 660 60 F1"); d.send("PAD 00");
        d.ecu.configure(0x6F1, 0x660, 0x60, 0xF1);
        d.hal.noAck = true;
        armConfirm(d, "22F150");
    } else if (scenario == "kombi_probe_denied") {
        // Same probe, but when built WITHOUT OPENMOTO_DIAG_KOMBI_PROBE it is denied.
        d.send("ADDR 6F1 660 60 F1"); d.send("PAD 00");
        d.ecu.configure(0x6F1, 0x660, 0x60, 0xF1);
        d.send("ARM 22F150");
    } else if (scenario == "service_denied") {
        // A service DID when built WITHOUT the flag: must be denied, not armed.
        d.send("ADDR 6F1 660 60 F1"); d.send("PAD 00");
        d.ecu.configure(0x6F1, 0x660, 0x60, 0xF1);
        d.send("ARM 22E12C");
    } else if (!strncmp(scenario.c_str(), "service_", 8)) {
        // Phase 7: read one service DID. scenario = service_E119/E12B/E12C/E12D.
        // Requires the sim built with OPENMOTO_DIAG_SERVICE_READ=1 to ARM.
        d.send("ADDR 6F1 660 60 F1"); d.send("PAD 00");
        d.ecu.configure(0x6F1, 0x660, 0x60, 0xF1);
        d.ecu.mode = MockEcu::M_SERVICE;
        std::string did = scenario.substr(8);  // e.g. "E119"
        armConfirm(d, "22" + did);
    } else {
        std::fprintf(stderr, "unknown scenario: %s\n", scenario.c_str());
        return 2;
    }
    return 0;
}
