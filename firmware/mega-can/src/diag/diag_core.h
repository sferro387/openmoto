// OpenMoto Phase 4: portable core of the READ-ONLY diagnostic client.
//
// No Arduino dependencies: this header compiles for the ATmega2560 (avr-g++,
// gnu++11) and for the host (clang++), so the exact logic that runs on the
// board is exercised offline against a mock ECU (tests/diag_host/diag_sim.cpp).
//
// Safety model (see docs/diag-active.md):
//   * Idle state is LISTEN_ONLY (silent). The controller is made active only
//     inside one confirmed transaction and is silenced again at its end.
//   * Only allowlisted, non-mutating UDS requests can be armed (policyCheck).
//     Everything else is denied by default; mutating services are denied by name.
//   * Every transmission needs ARM <req> followed by CONFIRM <code>, where the
//     code is derived from the exact CAN frame the firmware will send and is
//     printed by the firmware. A confirmation is single use and expires.
//   * A transaction transmits at most 1 request frame and 1 flow-control frame.
//   * All waits are bounded; transactions are rate limited; repeated failures
//     lock the client out until power cycle; '!' (or ESTOP) is an emergency stop
//     that is latched until reset.
//   * Addressing has NO defaults: ADDR and PAD must be set explicitly.
#pragma once
#include <stdint.h>
#include <string.h>

#ifndef OPENMOTO_DIAG_ALLOW_TX
#define OPENMOTO_DIAG_ALLOW_TX 0
#endif

// Phase 5: allow EXACTLY one additional, manufacturer-specific read-only probe,
// ReadDataByIdentifier 0xF150 ("22 F1 50"), used by MotoScan to identify the BMW
// KOMBI instrument cluster (docs/diag-kombi-findings.md). 0xF150 is outside the
// ISO 14229-1 identification range, so it is NOT in ALLOWED_DIDS and stays denied
// unless this flag is set. It remains a pure read (service 0x22): no write,
// reset, session, security or routine is enabled by this flag.
#ifndef OPENMOTO_DIAG_KOMBI_PROBE
#define OPENMOTO_DIAG_KOMBI_PROBE 0
#endif

// Phase 7: read-only KOMBI SERVICE INSPECTION. Allows EXACTLY four additional
// manufacturer-specific ReadDataByIdentifier DIDs used by MotoScan to read the
// cluster's existing service values (docs/diag-kombi-findings.md):
//   0xE119 odometer, 0xE12B clock, 0xE12C next service date, 0xE12D service
//   distance. All are service 0x22 pure reads. This flag enables NO write
//   (0x2E), reset, session, security, routine or coding -- those remain denied
//   by serviceForbidden(), which policyCheck() tests first. Separate from the
//   Phase 5 KOMBI_PROBE flag so the F150 probe build is unaffected.
#ifndef OPENMOTO_DIAG_SERVICE_READ
#define OPENMOTO_DIAG_SERVICE_READ 0
#endif

namespace diag {

// The one extra DID permitted only when OPENMOTO_DIAG_KOMBI_PROBE=1.
constexpr uint16_t KOMBI_PROBE_DID = 0xF150;

// The four service-inspection DIDs permitted only when OPENMOTO_DIAG_SERVICE_READ=1.
static const uint16_t SERVICE_READ_DIDS[] = {0xE119, 0xE12B, 0xE12C, 0xE12D};

// ---- Limits (compile-time; no runtime override) -----------------------------
constexpr uint16_t MAX_RESP_BYTES = 64;      // reassembly cap
constexpr uint32_t ARM_TTL_MS = 30000;       // armed confirmation lifetime
constexpr uint32_t TX_DONE_MS = 100;         // request frame must leave the controller
constexpr uint32_t P2_MS = 300;              // wait for first response frame
constexpr uint32_t P2_STAR_MS = 3000;        // wait after NRC 0x78 (responsePending)
constexpr uint32_t N_CR_MS = 300;            // wait between consecutive frames
constexpr uint32_t TOTAL_MS = 6000;          // hard cap for one transaction
constexpr uint32_t MIN_GAP_MS = 1000;        // minimum time between transactions
constexpr uint8_t MAX_PENDING = 3;           // NRC 0x78 repetitions tolerated
constexpr uint8_t MAX_TRANSACTIONS = 25;     // per power cycle
constexpr uint8_t MAX_FAILS = 3;             // consecutive failures before lockout
constexpr uint8_t MAX_BAD_CONFIRMS = 3;      // wrong CONFIRM codes before lockout
constexpr uint8_t FC_STMIN_MS = 2;           // STmin we ask the ECU to honour
constexpr uint8_t DIAG_LINE_MAX = 64;
constexpr uint8_t DIAG_TRACE_MAX = 16;

struct Frame {  // standard 11-bit data frame only
    uint16_t id;
    uint8_t dlc;
    uint8_t data[8];
};

enum TxState : uint8_t { TXS_NONE = 0, TXS_PENDING = 1, TXS_SENT = 2, TXS_FAILED = 3 };

// Hardware abstraction implemented by the Arduino glue and by the host simulator.
class Hal {
public:
    virtual ~Hal() {}
    virtual uint32_t nowMs() = 0;
    virtual int readChar() = 0;                 // -1 when nothing is available
    virtual void putChar(char c) = 0;
    // Abort anything pending, enter LISTEN_ONLY, verify CANSTAT. true if verified.
    virtual bool canSilence() = 0;
    // Restrict reception to this response ID (called while silent).
    virtual bool canSetRxId(uint16_t id) = 0;
    // Enter NORMAL (one-shot) mode and verify it. false in builds without TX.
    virtual bool canEnterActive() = 0;
    // Queue exactly one frame (one-shot: no automatic retransmission).
    virtual bool canTransmit(const Frame &f) = 0;
    virtual TxState canTxState() = 0;
    virtual bool canReceive(Frame &f) = 0;      // non-blocking
};

// ---- Allowlist policy ---------------------------------------------------------
enum PolicyKind : uint8_t {
    PK_TESTER_PRESENT, PK_READ_DID, PK_DTC_COUNT, PK_DTC_LIST,
    PK_DENY_EMPTY, PK_DENY_LENGTH, PK_DENY_FORBIDDEN, PK_DENY_NOT_LISTED,
    PK_DENY_SUBFUNCTION, PK_DENY_DID
};
inline bool policyAllowed(PolicyKind k) { return k <= PK_DTC_LIST; }
inline const char *policyText(PolicyKind k) {
    switch (k) {
    case PK_TESTER_PRESENT: return "TesterPresent";
    case PK_READ_DID: return "ReadDataByIdentifier";
    case PK_DTC_COUNT: return "ReadDTCInformation/reportNumberOfDTCByStatusMask";
    case PK_DTC_LIST: return "ReadDTCInformation/reportDTCByStatusMask";
    case PK_DENY_EMPTY: return "empty-request";
    case PK_DENY_LENGTH: return "length-not-allowed";
    case PK_DENY_FORBIDDEN: return "service-forbidden(mutating-or-security)";
    case PK_DENY_NOT_LISTED: return "service-not-allowlisted";
    case PK_DENY_SUBFUNCTION: return "subfunction-not-allowlisted";
    case PK_DENY_DID: return "DID-not-allowlisted";
    }
    return "?";
}
// ISO 14229-1 identification DIDs only (no manufacturer-specific DID is verified).
static const uint16_t ALLOWED_DIDS[] = {0xF186, 0xF187, 0xF188, 0xF189, 0xF18A, 0xF18B, 0xF18C,
                                        0xF190, 0xF191, 0xF192, 0xF193, 0xF194, 0xF195, 0xF197};
inline bool didAllowed(uint16_t did) {
#if OPENMOTO_DIAG_KOMBI_PROBE
    if (did == KOMBI_PROBE_DID) return true;  // Phase 5: KOMBI identification probe only
#endif
#if OPENMOTO_DIAG_SERVICE_READ
    for (uint8_t i = 0; i < sizeof(SERVICE_READ_DIDS) / sizeof(SERVICE_READ_DIDS[0]); ++i)
        if (SERVICE_READ_DIDS[i] == did) return true;  // Phase 7: four service reads only
#endif
    for (uint8_t i = 0; i < sizeof(ALLOWED_DIDS) / sizeof(ALLOWED_DIDS[0]); ++i)
        if (ALLOWED_DIDS[i] == did) return true;
    return false;
}
// Services that change ECU state, security state, memory or communication. They
// are denied by name (and would be denied anyway: the policy is default-deny).
inline bool serviceForbidden(uint8_t sid) {
    switch (sid) {
    case 0x10: case 0x11: case 0x14: case 0x27: case 0x28: case 0x2A: case 0x2C: case 0x2E:
    case 0x2F: case 0x31: case 0x34: case 0x35: case 0x36: case 0x37: case 0x38: case 0x3D:
    case 0x83: case 0x84: case 0x85: case 0x86: case 0x87:
        return true;
    default:
        return false;
    }
}
inline PolicyKind policyCheck(const uint8_t *req, uint8_t n) {
    if (n == 0) return PK_DENY_EMPTY;
    const uint8_t sid = req[0];
    if (serviceForbidden(sid)) return PK_DENY_FORBIDDEN;
    switch (sid) {
    case 0x3E:  // TesterPresent, exactly 3E 00 (no suppressPositiveResponse)
        if (n != 2) return PK_DENY_LENGTH;
        return req[1] == 0x00 ? PK_TESTER_PRESENT : PK_DENY_SUBFUNCTION;
    case 0x22:  // exactly one allowlisted DID
        if (n != 3) return PK_DENY_LENGTH;
        return didAllowed(uint16_t(req[1]) << 8 | req[2]) ? PK_READ_DID : PK_DENY_DID;
    case 0x19:  // 19 01 mm / 19 02 mm (no suppressPositiveResponse bit)
        if (n != 3) return PK_DENY_LENGTH;
        if (req[1] == 0x01) return PK_DTC_COUNT;
        if (req[1] == 0x02) return PK_DTC_LIST;
        return PK_DENY_SUBFUNCTION;
    default:
        return PK_DENY_NOT_LISTED;
    }
}

// ---- UDS response validation ----------------------------------------------------
enum RespKind : uint8_t { RK_POSITIVE, RK_NEGATIVE, RK_PENDING, RK_INVALID };
inline RespKind validateResponse(const uint8_t *req, uint8_t rl, const uint8_t *rsp, uint16_t n,
                                 uint8_t *nrc) {
    *nrc = 0;
    if (rl == 0 || n == 0) return RK_INVALID;
    if (rsp[0] == 0x7F) {
        if (n != 3 || rsp[1] != req[0] || rsp[2] == 0x00) return RK_INVALID;
        *nrc = rsp[2];
        return rsp[2] == 0x78 ? RK_PENDING : RK_NEGATIVE;
    }
    if (rsp[0] != uint8_t(req[0] + 0x40)) return RK_INVALID;
    switch (req[0]) {
    case 0x3E: return (n == 2 && rsp[1] == 0x00) ? RK_POSITIVE : RK_INVALID;
    case 0x22: return (n >= 4 && rl == 3 && rsp[1] == req[1] && rsp[2] == req[2]) ? RK_POSITIVE : RK_INVALID;
    case 0x19:
        if (rl != 3 || n < 3 || rsp[1] != req[1]) return RK_INVALID;
        if (req[1] == 0x01) return n == 6 ? RK_POSITIVE : RK_INVALID;
        if (req[1] == 0x02) return ((n - 3) % 4 == 0) ? RK_POSITIVE : RK_INVALID;
        return RK_INVALID;
    default: return RK_INVALID;
    }
}

// ---- Output helper ------------------------------------------------------------------
struct Out {
    Hal &h;
    explicit Out(Hal &hal) : h(hal) {}
    void s(const char *p) { while (*p) h.putChar(*p++); }
    void hex(uint8_t v) {
        static const char D[] = "0123456789ABCDEF";
        h.putChar(D[v >> 4]); h.putChar(D[v & 15]);
    }
    void id(uint16_t v) {  // 3 hex digits (11-bit identifier)
        static const char D[] = "0123456789ABCDEF";
        h.putChar(D[(v >> 8) & 15]); hex(uint8_t(v));
    }
    void bytes(const uint8_t *p, uint16_t n) { for (uint16_t i = 0; i < n; ++i) hex(p[i]); }
    void u(uint32_t v) {
        char b[11]; uint8_t i = 0;
        do { b[i++] = char('0' + v % 10); v /= 10; } while (v);
        while (i) h.putChar(b[--i]);
    }
    void nl() { h.putChar('\n'); }
};

// ---- Shell + transaction engine ------------------------------------------------------
class Shell {
public:
    explicit Shell(Hal &hal) : h(hal), o(hal) {}

    void begin() {
        o.s("OpenMoto Phase 4 read-only diagnostic client\n");
        o.s(OPENMOTO_DIAG_ALLOW_TX ? "BUILD tx=ENABLED (diag_readonly)\n" : "BUILD tx=DRYRUN (no transmit code)\n");
        o.s("NOT FOR VEHICLE USE until the prerequisites in docs/diag-active.md are met\n");
        if (!h.canSilence()) {
            estopped = true;
            o.s("ALERT silent-mode-unverified at boot; client latched off\n");
        }
        o.s("READY send STATUS\n");
    }

    void poll() {
        int ch;
        for (uint8_t budget = 64; budget && (ch = h.readChar()) >= 0; --budget) onChar(char(ch));
        const uint32_t now = h.nowMs();
        if (state == S_ARMED && elapsed(now, armedAt) > ARM_TTL_MS) disarm("expired");
        if (state == S_TX || state == S_RESP || state == S_CF) step(now);
    }

    // Exposed for tests / status.
    bool isEstopped() const { return estopped; }
    bool isLockedOut() const { return lockedOut; }
    bool isArmed() const { return state == S_ARMED; }
    bool isBusy() const { return state == S_TX || state == S_RESP || state == S_CF; }

private:
    enum State : uint8_t { S_IDLE, S_ARMED, S_TX, S_RESP, S_CF };
    enum Status : uint8_t {
        ST_OK, ST_NEGATIVE, ST_NO_RESPONSE, ST_CF_TIMEOUT, ST_TOTAL_TIMEOUT, ST_BAD_SEQUENCE,
        ST_PROTOCOL, ST_TOO_LARGE, ST_INVALID, ST_TOO_MANY_PENDING, ST_TX_FAILED, ST_TX_TIMEOUT,
        ST_MODE_FAIL, ST_TX_REFUSED, ST_ESTOP, ST_DRYRUN
    };
    static const char *statusText(Status s) {
        switch (s) {
        case ST_OK: return "OK"; case ST_NEGATIVE: return "NEGATIVE";
        case ST_NO_RESPONSE: return "NO_RESPONSE"; case ST_CF_TIMEOUT: return "CF_TIMEOUT";
        case ST_TOTAL_TIMEOUT: return "TOTAL_TIMEOUT"; case ST_BAD_SEQUENCE: return "BAD_SEQUENCE";
        case ST_PROTOCOL: return "PROTOCOL_ERROR"; case ST_TOO_LARGE: return "TOO_LARGE";
        case ST_INVALID: return "INVALID_RESPONSE"; case ST_TOO_MANY_PENDING: return "TOO_MANY_PENDING";
        case ST_TX_FAILED: return "TX_FAILED"; case ST_TX_TIMEOUT: return "TX_TIMEOUT";
        case ST_MODE_FAIL: return "MODE_FAIL"; case ST_TX_REFUSED: return "TX_REFUSED";
        case ST_ESTOP: return "ESTOP"; case ST_DRYRUN: return "DRYRUN";
        }
        return "?";
    }
    struct Trace { char dir; Frame f; };

    Hal &h;
    Out o;
    State state = S_IDLE;
    // line assembly
    char line[DIAG_LINE_MAX + 1];
    uint8_t lineLen = 0;
    bool lineOverflow = false;
    // configuration (no defaults)
    bool addrSet = false, padSet = false;
    uint16_t txId = 0, rxId = 0;
    int16_t txExt = -1, rxExt = -1;
    int16_t pad = -1;
    // limits / latches
    bool estopped = false, lockedOut = false;
    uint8_t txCount = 0, fails = 0, badConfirms = 0;
    bool haveLastEnd = false;
    uint32_t lastEnd = 0;
    // armed request
    uint8_t req[8];
    uint8_t reqLen = 0;
    Frame reqFrame;
    uint16_t armCode = 0;
    uint32_t armedAt = 0;
    uint16_t armSeq = 0;
    PolicyKind armKind = PK_DENY_EMPTY;
    // transaction
    uint32_t tStart = 0, deadline = 0, txStart = 0;
    uint8_t resp[MAX_RESP_BYTES];
    uint16_t respLen = 0, respTotal = 0;
    uint8_t nextSn = 1, pendingCount = 0, framesTx = 0, framesRx = 0, ignored = 0;
    Trace trace[DIAG_TRACE_MAX];
    uint8_t traceN = 0;

    static bool elapsedBeyond(uint32_t now, uint32_t since, uint32_t ms) { return uint32_t(now - since) > ms; }
    static uint32_t elapsed(uint32_t now, uint32_t since) { return uint32_t(now - since); }
    static bool reached(uint32_t now, uint32_t t) { return int32_t(now - t) >= 0; }

    // ---- parsing helpers
    static int hexVal(char c) {
        if (c >= '0' && c <= '9') return c - '0';
        if (c >= 'a' && c <= 'f') return c - 'a' + 10;
        if (c >= 'A' && c <= 'F') return c - 'A' + 10;
        return -1;
    }
    static bool parseHexNumber(const char *s, uint32_t maxVal, uint32_t &out) {
        if (!*s) return false;
        uint32_t v = 0;
        for (uint8_t n = 0; *s; ++s, ++n) {
            const int d = hexVal(*s);
            if (d < 0 || n >= 8) return false;
            v = v << 4 | uint32_t(d);
            if (v > maxVal) return false;
        }
        out = v;
        return true;
    }
    // Concatenate hex byte tokens ("22 F1 90" or "22F190"); every token must have even length.
    static bool parseHexBytes(char **tok, uint8_t ntok, uint8_t *out, uint8_t cap, uint8_t &n) {
        n = 0;
        for (uint8_t t = 0; t < ntok; ++t) {
            const char *s = tok[t];
            const size_t len = strlen(s);
            if (len == 0 || (len & 1)) return false;
            for (size_t i = 0; i < len; i += 2) {
                const int a = hexVal(s[i]), b = hexVal(s[i + 1]);
                if (a < 0 || b < 0 || n >= cap) return false;
                out[n++] = uint8_t(a << 4 | b);
            }
        }
        return n > 0;
    }

    // ---- input
    void onChar(char c) {
        if (c == '!') {  // emergency stop: highest priority, valid anywhere
            lineLen = 0; lineOverflow = false;
            emergencyStop("bang");
            return;
        }
        if (c == '\r') return;
        if (c == '\n') {
            line[lineLen] = 0;
            const bool ovf = lineOverflow;
            const uint8_t len = lineLen;
            lineLen = 0; lineOverflow = false;
            if (ovf) { o.s("ERR line-too-long\n"); return; }
            if (len) execLine();
            return;
        }
        if (lineLen < DIAG_LINE_MAX) line[lineLen++] = c; else lineOverflow = true;
    }

    void execLine() {
        char *tok[10];
        uint8_t nt = 0;
        char *p = line;
        while (*p && nt < 10) {
            while (*p == ' ' || *p == '\t') *p++ = 0;
            if (!*p) break;
            tok[nt++] = p;
            while (*p && *p != ' ' && *p != '\t') ++p;
        }
        if (*p && nt == 10) { o.s("ERR too-many-arguments\n"); return; }
        if (nt == 0) return;
        for (char *c = tok[0]; *c; ++c) if (*c >= 'a' && *c <= 'z') *c = char(*c - 32);
        const char *cmd = tok[0];
        if (!strcmp(cmd, "HELLO")) { cmdHello(); return; }
        if (!strcmp(cmd, "STATUS")) { cmdStatus(); return; }
        if (!strcmp(cmd, "POLICY")) { cmdPolicy(tok + 1, nt - 1); return; }
        if (!strcmp(cmd, "ESTOP")) { emergencyStop("command"); return; }
        if (estopped || lockedOut) {
            o.s(estopped ? "ERR estop-latched reset-required\n" : "ERR lockout reset-required\n");
            return;
        }
        if (!strcmp(cmd, "ADDR")) cmdAddr(tok + 1, nt - 1);
        else if (!strcmp(cmd, "PAD")) cmdPad(tok + 1, nt - 1);
        else if (!strcmp(cmd, "ARM")) cmdArm(tok + 1, nt - 1);
        else if (!strcmp(cmd, "CONFIRM")) cmdConfirm(tok + 1, nt - 1);
        else if (!strcmp(cmd, "DISARM")) { if (state == S_ARMED) disarm("command"); else o.s("OK nothing-armed\n"); }
        else o.s("ERR unknown-command\n");
    }

    // ---- commands
    void cmdHello() {
        o.s("OpenMoto Phase 4 read-only diagnostic client\n");
        o.s(OPENMOTO_DIAG_ALLOW_TX ? "BUILD tx=ENABLED\n" : "BUILD tx=DRYRUN\n");
        o.s("OK hello\n");
    }
    void cmdStatus() {
        o.s("STATUS state=");
        o.s(state == S_IDLE ? "IDLE" : state == S_ARMED ? "ARMED" : "BUSY");
        o.s(" addr="); o.s(addrSet ? "SET" : "UNSET");
        o.s(" pad="); o.s(padSet ? "SET" : "UNSET");
        o.s(" txn="); o.u(txCount);
        o.s(" fails="); o.u(fails);
        o.s(" estop="); o.u(estopped);
        o.s(" lockout="); o.u(lockedOut);
        o.s(" tx_build="); o.u(OPENMOTO_DIAG_ALLOW_TX);
        o.nl();
    }
    void cmdPolicy(char **a, uint8_t n) {
        uint8_t b[16], len;
        if (n == 0 || !parseHexBytes(a, n, b, sizeof(b), len)) { o.s("ERR bad-hex\n"); return; }
        const PolicyKind k = policyCheck(b, len);
        o.s(policyAllowed(k) ? "POLICY ALLOW " : "POLICY DENY ");
        o.s(policyText(k)); o.nl();
    }
    void cmdAddr(char **a, uint8_t n) {
        if (state != S_IDLE && state != S_ARMED) { o.s("ERR busy\n"); return; }
        uint32_t t, r, te = 0, re = 0;
        const bool ext = (n == 4);
        if (!(n == 2 || n == 4) || !parseHexNumber(a[0], 0x7FF, t) || !parseHexNumber(a[1], 0x7FF, r) ||
            (ext && (!parseHexNumber(a[2], 0xFF, te) || !parseHexNumber(a[3], 0xFF, re)))) {
            o.s("ERR bad-addr usage: ADDR <txid> <rxid> [<txext> <rxext>] (hex, 11-bit)\n");
            return;
        }
        if (t == 0 || r == 0 || t == 0x7DF || r == 0x7DF || t == r) {
            o.s("ERR addr-forbidden (0, 0x7DF functional broadcast, or tx==rx)\n");
            return;
        }
        if (state == S_ARMED) disarm("addr-changed");
        if (!h.canSetRxId(uint16_t(r))) { o.s("ERR rx-filter-failed\n"); addrSet = false; return; }
        txId = uint16_t(t); rxId = uint16_t(r);
        txExt = ext ? int16_t(te) : int16_t(-1);
        rxExt = ext ? int16_t(re) : int16_t(-1);
        addrSet = true;
        o.s("OK addr tx="); o.id(txId); o.s(" rx="); o.id(rxId);
        if (ext) { o.s(" txext="); o.hex(uint8_t(te)); o.s(" rxext="); o.hex(uint8_t(re)); }
        o.nl();
    }
    void cmdPad(char **a, uint8_t n) {
        if (state != S_IDLE && state != S_ARMED) { o.s("ERR busy\n"); return; }
        uint32_t v;
        if (n != 1) { o.s("ERR bad-pad usage: PAD <hex byte>|NONE\n"); return; }
        if (!strcmp(a[0], "NONE") || !strcmp(a[0], "none")) pad = -1;
        else if (parseHexNumber(a[0], 0xFF, v) && strlen(a[0]) == 2) pad = int16_t(v);
        else { o.s("ERR bad-pad usage: PAD <hex byte>|NONE\n"); return; }
        if (state == S_ARMED) disarm("pad-changed");
        padSet = true;
        o.s("OK pad="); if (pad < 0) o.s("NONE"); else o.hex(uint8_t(pad)); o.nl();
    }

    uint8_t headLen() const { return txExt >= 0 ? 1 : 0; }
    // Fill f with an ISO-TP frame carrying `pci` + payload (single frame or flow control).
    void buildFrame(Frame &f, const uint8_t *body, uint8_t bodyLen) const {
        memset(&f, 0, sizeof(f));
        f.id = txId;
        uint8_t k = 0;
        if (txExt >= 0) f.data[k++] = uint8_t(txExt);
        for (uint8_t i = 0; i < bodyLen; ++i) f.data[k++] = body[i];
        if (pad >= 0) { while (k < 8) f.data[k++] = uint8_t(pad); f.dlc = 8; }
        else f.dlc = k;
    }
    void buildSingleFrame(Frame &f) const {
        uint8_t body[8];
        body[0] = uint8_t(reqLen);  // PCI: SF, length
        memcpy(body + 1, req, reqLen);
        buildFrame(f, body, uint8_t(1 + reqLen));
    }

    bool gatesOk(uint32_t now) {
        if (!addrSet || !padSet) { o.s("ERR not-configured (ADDR and PAD must be set explicitly)\n"); return false; }
        if (txCount >= MAX_TRANSACTIONS) { o.s("ERR session-limit reset-required\n"); return false; }
        if (haveLastEnd && elapsed(now, lastEnd) < MIN_GAP_MS) {
            o.s("ERR rate-limit wait_ms="); o.u(MIN_GAP_MS - elapsed(now, lastEnd)); o.nl();
            return false;
        }
        return true;
    }

    void cmdArm(char **a, uint8_t n) {
        if (state != S_IDLE && state != S_ARMED) { o.s("ERR busy\n"); return; }
        const uint32_t now = h.nowMs();
        uint8_t b[8], len;
        if (n == 0 || !parseHexBytes(a, n, b, sizeof(b), len)) { o.s("ERR bad-hex\n"); return; }
        const PolicyKind k = policyCheck(b, len);
        if (!policyAllowed(k)) { o.s("ERR policy-deny "); o.s(policyText(k)); o.nl(); return; }
        if (!gatesOk(now)) return;
        if (len > 7 - headLen()) { o.s("ERR request-too-long-for-single-frame\n"); return; }
        if (state == S_ARMED) disarm("rearmed");
        memcpy(req, b, len); reqLen = len; armKind = k;
        buildSingleFrame(reqFrame);
        // Confirmation code: FNV-1a over the exact frame, a counter and the time.
        uint32_t x = 2166136261UL;
        const uint8_t mix[] = {uint8_t(reqFrame.id >> 8), uint8_t(reqFrame.id), reqFrame.dlc, uint8_t(armSeq),
                               uint8_t(armSeq >> 8), uint8_t(now), uint8_t(now >> 8), uint8_t(now >> 16)};
        for (uint8_t i = 0; i < sizeof(mix); ++i) { x ^= mix[i]; x *= 16777619UL; }
        for (uint8_t i = 0; i < reqFrame.dlc; ++i) { x ^= reqFrame.data[i]; x *= 16777619UL; }
        armCode = uint16_t((x >> 16) ^ x);
        if (armCode == 0) armCode = 0xA55A;
        ++armSeq;
        armedAt = now;
        state = S_ARMED;
        o.s("ARMED code="); o.hex(uint8_t(armCode >> 8)); o.hex(uint8_t(armCode));
        o.s(" req="); o.bytes(req, reqLen);
        o.s(" desc="); o.s(policyText(armKind));
        o.s(" tx_id="); o.id(reqFrame.id);
        o.s(" dlc="); o.u(reqFrame.dlc);
        o.s(" data="); o.bytes(reqFrame.data, reqFrame.dlc);
        o.s(" max_tx_frames=2 expires_ms="); o.u(ARM_TTL_MS);
        o.s(OPENMOTO_DIAG_ALLOW_TX ? " tx_build=1\n" : " tx_build=0\n");
    }

    void disarm(const char *why) {
        if (state == S_ARMED) state = S_IDLE;
        armCode = 0;
        o.s("DISARMED reason="); o.s(why); o.nl();
    }

    void cmdConfirm(char **a, uint8_t n) {
        const uint32_t now = h.nowMs();
        if (state != S_ARMED) { o.s("ERR not-armed\n"); return; }
        uint32_t code;
        if (n != 1 || strlen(a[0]) != 4 || !parseHexNumber(a[0], 0xFFFF, code) || code != armCode) {
            disarm("bad-code");
            if (++badConfirms >= MAX_BAD_CONFIRMS) emergencyStop("too-many-bad-confirmations");
            return;
        }
        badConfirms = 0;
        if (elapsed(now, armedAt) > ARM_TTL_MS) { disarm("expired"); return; }
        if (!gatesOk(now)) { state = S_IDLE; armCode = 0; return; }
        armCode = 0;  // single use
        startTransaction(now);
    }

    // ---- transaction engine
    void traceAdd(char dir, const Frame &f) {
        if (traceN < DIAG_TRACE_MAX) { trace[traceN].dir = dir; trace[traceN].f = f; ++traceN; }
    }
    void startTransaction(uint32_t now) {
        respLen = respTotal = 0; nextSn = 1; pendingCount = 0; framesTx = framesRx = ignored = 0; traceN = 0;
        tStart = now;
        if (!OPENMOTO_DIAG_ALLOW_TX) { finish(ST_DRYRUN, 0); return; }
        if (!h.canEnterActive()) { finish(ST_MODE_FAIL, 0); return; }
        Frame stale;  // discard anything received before our request
        for (uint8_t i = 0; i < 16 && h.canReceive(stale); ++i) {}
        if (!h.canTransmit(reqFrame)) { finish(ST_TX_REFUSED, 0); return; }
        ++framesTx; traceAdd('T', reqFrame);
        txStart = h.nowMs();
        state = S_TX;
    }

    void finish(Status st, uint8_t nrc) {
        const uint32_t now = h.nowMs();
        const bool silent = h.canSilence();
        if (!silent) { estopped = true; o.s("ALERT silent-mode-unverified; client latched off\n"); }
        state = S_IDLE;
        if (st != ST_DRYRUN) {
            ++txCount;
            if (st == ST_OK || st == ST_NEGATIVE) fails = 0;
            else if (st != ST_ESTOP && ++fails >= MAX_FAILS) {
                lockedOut = true;
                o.s("ALERT lockout consecutive-failures; reset required\n");
            }
            haveLastEnd = true; lastEnd = now;
        }
        for (uint8_t i = 0; i < traceN; ++i) {
            o.s("TRACE "); o.s(trace[i].dir == 'T' ? "tx" : "rx");
            o.s(" id="); o.id(trace[i].f.id); o.s(" dlc="); o.u(trace[i].f.dlc);
            o.s(" data="); o.bytes(trace[i].f.data, trace[i].f.dlc); o.nl();
        }
        o.s("RESULT status="); o.s(statusText(st));
        o.s(" req="); o.bytes(req, reqLen);
        if (st == ST_OK || st == ST_NEGATIVE) { o.s(" resp="); o.bytes(resp, respLen); }
        if (st == ST_NEGATIVE) { o.s(" nrc="); o.hex(nrc); }
        o.s(" frames_tx="); o.u(framesTx);
        o.s(" frames_rx="); o.u(framesRx);
        o.s(" ignored="); o.u(ignored);
        o.s(" pending="); o.u(pendingCount);
        o.s(" ms="); o.u(elapsed(now, tStart));
        o.s(" silent="); o.u(silent);
        o.nl();
    }

    void emergencyStop(const char *why) {
        const bool wasActive = (state == S_TX || state == S_RESP || state == S_CF);
        estopped = true;
        armCode = 0;
        if (wasActive) {
            finish(ST_ESTOP, 0);
        } else {
            state = S_IDLE;
            if (!h.canSilence()) o.s("ALERT silent-mode-unverified\n");
        }
        o.s("EVT estop reason="); o.s(why); o.s(" latched=1\n");
    }

    void step(uint32_t now) {
        Frame f;
        for (uint8_t i = 0; i < 16 && (state == S_TX || state == S_RESP || state == S_CF) && h.canReceive(f); ++i)
            onFrame(f, now);
        if (state == S_TX) {
            const TxState ts = h.canTxState();
            if (ts == TXS_FAILED) { finish(ST_TX_FAILED, 0); return; }
            if (ts == TXS_SENT) { state = S_RESP; deadline = now + P2_MS; }
            else if (elapsedBeyond(now, txStart, TX_DONE_MS)) { finish(ST_TX_TIMEOUT, 0); return; }
        }
        if (state == S_IDLE) return;
        if (elapsedBeyond(now, tStart, TOTAL_MS)) { finish(ST_TOTAL_TIMEOUT, 0); return; }
        if (state != S_TX && reached(now, deadline)) finish(state == S_CF ? ST_CF_TIMEOUT : ST_NO_RESPONSE, 0);
    }

    void sendFlowControl() {
        const uint8_t body[3] = {0x30, 0x00, FC_STMIN_MS};  // ContinueToSend, BS=0, STmin
        Frame fc;
        buildFrame(fc, body, 3);
        if (framesTx >= 2 || !h.canTransmit(fc)) { finish(ST_TX_REFUSED, 0); return; }
        ++framesTx; traceAdd('T', fc);
    }

    void completeResponse(uint32_t now) {
        uint8_t nrc;
        switch (validateResponse(req, reqLen, resp, respLen, &nrc)) {
        case RK_POSITIVE: finish(ST_OK, 0); break;
        case RK_NEGATIVE: finish(ST_NEGATIVE, nrc); break;
        case RK_PENDING:
            if (++pendingCount > MAX_PENDING) { finish(ST_TOO_MANY_PENDING, nrc); break; }
            respLen = respTotal = 0; nextSn = 1;
            state = S_RESP; deadline = now + P2_STAR_MS;
            break;
        default: finish(ST_INVALID, 0); break;
        }
    }

    void onFrame(const Frame &f, uint32_t now) {
        if (f.id != rxId) { ++ignored; return; }
        uint8_t o0 = 0;
        if (rxExt >= 0) {
            if (f.dlc < 1 || f.data[0] != uint8_t(rxExt)) { ++ignored; return; }
            o0 = 1;
        }
        if (f.dlc <= o0) { ++ignored; return; }
        ++framesRx; traceAdd('R', f);
        const uint8_t pci = f.data[o0];
        const uint8_t type = pci >> 4;
        const uint8_t cap = uint8_t(7 - o0);  // payload bytes in a single frame
        if (state == S_TX || state == S_RESP) {
            if (type == 0) {  // single frame
                const uint8_t len = pci & 0x0F;
                if (len == 0 || len > cap || f.dlc < o0 + 1 + len) { finish(ST_PROTOCOL, 0); return; }
                memcpy(resp, f.data + o0 + 1, len); respLen = len;
                completeResponse(now);
            } else if (type == 1) {  // first frame
                const uint16_t total = uint16_t((pci & 0x0F) << 8 | (f.dlc > o0 + 1 ? f.data[o0 + 1] : 0));
                if (f.dlc != 8 || total <= cap) { finish(ST_PROTOCOL, 0); return; }
                if (total > MAX_RESP_BYTES) { finish(ST_TOO_LARGE, 0); return; }  // no FC is sent
                const uint8_t n = uint8_t(6 - o0);
                memcpy(resp, f.data + o0 + 2, n); respLen = n; respTotal = total; nextSn = 1;
                sendFlowControl();
                if (state == S_IDLE) return;
                state = S_CF; deadline = now + N_CR_MS;
            } else {
                ++ignored;  // stray CF/FC before a first frame: ignore, bounded by the P2 timeout
            }
        } else if (state == S_CF) {
            if (type != 2) { finish(ST_PROTOCOL, 0); return; }
            if ((pci & 0x0F) != (nextSn & 0x0F)) { finish(ST_BAD_SEQUENCE, 0); return; }
            const uint16_t remaining = uint16_t(respTotal - respLen);
            const uint8_t take = remaining < cap ? uint8_t(remaining) : cap;
            if (f.dlc < o0 + 1 + take) { finish(ST_PROTOCOL, 0); return; }
            memcpy(resp + respLen, f.data + o0 + 1, take); respLen = uint16_t(respLen + take);
            ++nextSn; deadline = now + N_CR_MS;
            if (respLen >= respTotal) completeResponse(now);
        }
    }
};

}  // namespace diag
