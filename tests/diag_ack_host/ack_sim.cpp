// OpenMoto Phase 6 host simulator: drives the REAL ACK-only bring-up core
// (firmware/mega-can/src/diag_ack/ack_core.h) against a simulated CAN
// controller. This is the exact state machine that runs on the Mega; only the
// AckHal is replaced. Nothing here talks to hardware or a vehicle.
//
// The simulated controller models the four bus conditions the bring-up must
// distinguish, including the key one the task raises: a bus where frames are
// present but UNACKNOWLEDGED while we are passive (LISTEN_ONLY), which then
// becomes clean once we enter ACK-only (NORMAL) and start driving the ACK slot.
//
// Usage: ack_sim <scenario> [--stdin]
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <string>

#include "../../firmware/mega-can/src/diag_ack/ack_core.h"

namespace {

using ackbringup::BusStats;

// Simulated MCP2515 + bus. Mode and counters evolve per poll depending on the
// scenario and whether we are currently ACKing (NORMAL) or silent (LISTEN).
struct SimController {
    enum Mode { CONFIG, LISTEN, NORMAL };
    enum Bus {
        B_HEALTHY_ACKED,   // another node ACKs; clean in both windows
        B_NO_ACK_WAS_US,   // lone transmitter, no ACK while we are passive; clean once we ACK
        B_WRONG_BITRATE,   // our bit timing disagrees: errors climb, little/no rx
        B_NO_CONNECTION,   // quiet wire, nothing received, counters stay 0
        B_FRAMES_NO_DIAG,  // frames acked fine (same as healthy for bring-up)
    };

    Mode mode = CONFIG;
    Bus bus = B_HEALTHY_ACKED;
    uint32_t rxCount = 0;
    uint8_t tec = 0, rec = 0, eflg = 0;
    bool txReqAny = false;     // would be set if a TX buffer were loaded (never, by design)
    bool rtsPinsDisabled = false;
    bool spiBroken = false;
    uint32_t clock = 1000;

    void tickBus(uint32_t ms) {
        // Advance simulated time and evolve rx/counters per window and bus type.
        clock += ms;
        const bool acking = (mode == NORMAL);
        const bool silent = (mode == LISTEN);
        switch (bus) {
        case B_HEALTHY_ACKED:
        case B_FRAMES_NO_DIAG:
            if (acking || silent) rxCount += ms / 2;  // ~500 fps model
            // clean counters either way (a healthy bus tolerates a passive listener)
            break;
        case B_NO_ACK_WAS_US:
            if (silent) {
                // We see the lone transmitter's repeats and accumulate RX errors
                // (nobody ACKs, including us). Surface via EFLG RXEP + REC.
                rxCount += ms / 2;
                if (rec < 120) rec = uint8_t(rec + 3);
                if (rec >= 96) eflg |= 0x08;  // RXEP (receive error passive-ish)
                eflg |= 0x40;                 // RX activity/overflow-ish marker
            } else if (acking) {
                // Now we drive the ACK slot: the transmitter is satisfied, our
                // counters recover, frames keep coming cleanly.
                rxCount += ms / 2;
                if (rec > 0) rec = uint8_t(rec > 4 ? rec - 4 : 0);
                eflg &= uint8_t(~0x08);
            }
            break;
        case B_WRONG_BITRATE:
            // Our timing disagrees: almost no valid rx, errors climb fast in any
            // mode that participates; even passive accrues receive errors.
            if (rec < 200) rec = uint8_t(rec + 20);
            if (acking && tec < 200) tec = uint8_t(tec + 20);
            eflg |= 0x08;
            break;
        case B_NO_CONNECTION:
            // Quiet wire: nothing received, no errors.
            break;
        }
    }

    void readInto(BusStats &s) {
        s.rxCount = rxCount;
        s.tec = tec;
        s.rec = rec;
        s.eflg = eflg;
        s.errorPassive = (eflg & 0x18) != 0 || tec >= 128 || rec >= 128;
        s.busOff = (eflg & 0x20) != 0 || tec >= 255;
        s.mode_normal = (mode == NORMAL);
        s.spiError = spiBroken;
    }
};

class SimAckHal : public ackbringup::AckHal {
public:
    SimController &c;
    std::string input, output;
    explicit SimAckHal(SimController &ctrl) : c(ctrl) {}

    uint32_t nowMs() override { return c.clock; }
    int readChar() override {
        if (input.empty()) return -1;
        char ch = input.front(); input.erase(input.begin());
        return (unsigned char)ch;
    }
    void putChar(char ch) override { output.push_back(ch); }

    bool enterConfig() override {
        c.mode = SimController::CONFIG; c.rtsPinsDisabled = true;
        c.tec = 0; c.rec = 0; c.eflg = 0;  // counters/flags clear on entering CONFIG
        return true;
    }
    bool enterListenOnly() override {
        // Real glue goes CONFIG -> LISTEN, which clears the error counters.
        c.mode = SimController::LISTEN; c.rtsPinsDisabled = true; c.txReqAny = false;
        c.tec = 0; c.rec = 0; c.eflg = 0;
        return true;
    }
    bool enterAckOnly() override {
        // Verify no TX pending / RTS disabled (always true by design) then NORMAL,
        // via CONFIG, which clears the error counters before the active window.
        c.rtsPinsDisabled = true;
        if (c.txReqAny) return false;
        c.tec = 0; c.rec = 0; c.eflg = 0;
        c.mode = SimController::NORMAL;
        return true;
    }
    bool noTxPending() override { return !c.txReqAny; }
    void readStats(BusStats &s) override { c.readInto(s); }
};

struct Driver {
    SimController ctrl;
    SimAckHal hal{ctrl};
    ackbringup::AckShell shell{hal};

    explicit Driver(SimController::Bus bus) { ctrl.bus = bus; shell.begin(); flush(); }

    void flush() { if (!hal.output.empty()) { std::fputs(hal.output.c_str(), stdout); hal.output.clear(); } }

    void send(const std::string &line) {
        hal.input += line; hal.input += '\n';
        // Pump the shell, advancing simulated time so the bounded windows elapse.
        for (int i = 0; i < 1200; ++i) {
            shell.poll();
            if (!hal.input.empty()) continue;
            ctrl.tickBus(ackbringup::POLL_MS);
        }
        flush();
    }

    // Recover the ARMED confirmation code and CONFIRM it.
    void armConfirm() {
        hal.input += "ARM\n";
        for (int i = 0; i < 80 && !hal.input.empty(); ++i) shell.poll();
        std::string armed = hal.output;
        std::fputs(armed.c_str(), stdout); hal.output.clear();
        auto pos = armed.rfind("ARMED code=");
        std::string code = pos == std::string::npos ? "0000" : armed.substr(pos + 11, 4);
        send("CONFIRM " + code);
    }
};

SimController::Bus busFor(const std::string &name) {
    if (name == "healthy") return SimController::B_HEALTHY_ACKED;
    if (name == "no_ack_was_us") return SimController::B_NO_ACK_WAS_US;
    if (name == "wrong_bitrate") return SimController::B_WRONG_BITRATE;
    if (name == "no_connection") return SimController::B_NO_CONNECTION;
    if (name == "frames_no_diag") return SimController::B_FRAMES_NO_DIAG;
    return SimController::B_HEALTHY_ACKED;
}

}  // namespace

int main(int argc, char **argv) {
    std::string scenario = argc > 1 ? argv[1] : "healthy";
    bool stdinMode = false, spiBreak = false, estopMid = false;
    for (int i = 1; i < argc; ++i) {
        if (!strcmp(argv[i], "--stdin")) stdinMode = true;
        else if (!strcmp(argv[i], "--spi-break")) spiBreak = true;
        else if (!strcmp(argv[i], "--estop-mid")) estopMid = true;
    }

    Driver d(busFor(scenario));
    if (spiBreak) d.ctrl.spiBroken = true;

    if (stdinMode) {
        std::fputs("EOC\n", stdout); std::fflush(stdout);
        char buf[128];
        while (std::fgets(buf, sizeof(buf), stdin)) {
            std::string line(buf);
            if (!line.empty() && line.back() == '\n') line.pop_back();
            d.send(line);
            std::fputs("EOC\n", stdout); std::fflush(stdout);
        }
        return 0;
    }

    d.send("STATUS");
    if (estopMid) {
        // ARM, CONFIRM, then estop partway through (emergency stop while active).
        d.hal.input += "ARM\n";
        for (int i = 0; i < 80 && !d.hal.input.empty(); ++i) d.shell.poll();
        std::string armed = d.hal.output; std::fputs(armed.c_str(), stdout); d.hal.output.clear();
        auto pos = armed.rfind("ARMED code=");
        std::string code = pos == std::string::npos ? "0000" : armed.substr(pos + 11, 4);
        d.hal.input += "CONFIRM " + code + "\n";
        // run a few polls (into the passive window) then hit estop
        for (int i = 0; i < 20; ++i) { d.shell.poll(); d.ctrl.tickBus(ackbringup::POLL_MS); }
        d.flush();
        d.send("!");
        d.send("STATUS");
        d.send("ARM");  // must be refused: estop latched
    } else {
        d.armConfirm();
        d.send("STATUS");
    }
    return 0;
}
