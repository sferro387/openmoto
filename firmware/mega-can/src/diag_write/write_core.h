// OpenMoto Phase 9: portable core of the SERVICE-WRITE client (separate tree).
//
// This is a DISTINCT core from src/diag/diag_core.h so that the read-only builds
// stay byte-identical (adding anything to diag_core.h changes its compiled hash).
// It adds the one capability the read client lacks: ISO-TP MULTI-FRAME TRANSMIT
// (first frame -> wait for the ECU flow-control -> consecutive frames honouring
// the ECU's block size and STmin), needed because E12B(10B) and E12C(7B) writes
// exceed a single CAN frame. E12D(5B) is single-frame.
//
// It performs the three MotoScan service writes and NOTHING else:
//   2E E1 2B clock, 2E E1 2C next service date, 2E E1 2D service distance.
// Every write is: ARM <did> <payload> -> CONFIRM <code> (code bound to the exact
// bytes) -> one bounded write transaction -> MANDATORY read-back (22 E1 xx) and
// compare. Writes mutate cluster NVM; this is not reversible like a read.
//
// Compiles for AVR (firmware) and host (clang++) so the exact logic is tested
// offline against a mock ECU (tests/diag_write_host/write_sim.cpp).
#pragma once
#include <stdint.h>
#include <string.h>

#ifndef OPENMOTO_WRITE_ALLOW_TX
#define OPENMOTO_WRITE_ALLOW_TX 0
#endif

namespace diagwrite {

// ---- bounds -------------------------------------------------------------------
constexpr uint32_t ARM_TTL_MS = 30000;
constexpr uint32_t P2_MS = 400;          // wait for a response frame
constexpr uint32_t P2_STAR_MS = 5000;    // after NRC 0x78
constexpr uint32_t FC_WAIT_MS = 400;     // wait for the ECU flow-control frame
constexpr uint32_t N_CS_MIN_MS = 2;      // our min separation between CFs (if ECU asks 0)
constexpr uint32_t TX_DONE_MS = 120;     // a queued frame must leave the controller
constexpr uint32_t TOTAL_MS = 8000;      // hard cap for one write+readback
constexpr uint32_t MIN_GAP_MS = 1500;    // between transactions (matches MotoScan)
constexpr uint8_t MAX_PENDING = 4;
constexpr uint8_t MAX_WRITES = 6;        // per power cycle
constexpr uint8_t MAX_BAD_CONFIRMS = 3;
constexpr uint8_t LINE_W_MAX = 48;

struct Frame { uint16_t id; uint8_t dlc; uint8_t data[8]; };
enum TxState : uint8_t { TXS_NONE, TXS_PENDING, TXS_SENT, TXS_FAILED };

class Hal {
public:
    virtual ~Hal() {}
    virtual uint32_t nowMs() = 0;
    virtual int readChar() = 0;
    virtual void putChar(char c) = 0;
    virtual bool canSilence() = 0;              // -> verified LISTEN_ONLY
    virtual bool canSetRxId(uint16_t id) = 0;
    virtual bool canEnterActive() = 0;          // NORMAL one-shot; false w/o TX
    virtual bool canTransmit(const Frame &f) = 0;
    virtual TxState canTxState() = 0;
    virtual bool canReceive(Frame &f) = 0;
};

// ---- the three allowed writes -------------------------------------------------
inline bool serviceWriteDid(uint8_t didLo) { return didLo == 0x2B || didLo == 0x2C || didLo == 0x2D; }
inline uint8_t serviceWriteLen(uint8_t didLo) { return didLo == 0x2B ? 10 : didLo == 0x2C ? 7 : 5; }

// Validate a candidate write request (must be 2E E1 {2B|2C|2D} with exact length).
inline bool validWriteReq(const uint8_t *r, uint8_t n) {
    return n >= 3 && r[0] == 0x2E && r[1] == 0xE1 && serviceWriteDid(r[2]) && n == serviceWriteLen(r[2]);
}

// ---- output helper ------------------------------------------------------------
struct Out {
    Hal &h; explicit Out(Hal &x) : h(x) {}
    void s(const char *p) { while (*p) h.putChar(*p++); }
    void hex(uint8_t v){ static const char D[]="0123456789ABCDEF"; h.putChar(D[v>>4]); h.putChar(D[v&15]); }
    void id(uint16_t v){ static const char D[]="0123456789ABCDEF"; h.putChar(D[(v>>8)&15]); hex(uint8_t(v)); }
    void bytes(const uint8_t*p,uint16_t n){ for(uint16_t i=0;i<n;++i) hex(p[i]); }
    void u(uint32_t v){ char b[11]; uint8_t i=0; do{b[i++]=char('0'+v%10); v/=10;}while(v); while(i) h.putChar(b[--i]); }
    void nl(){ h.putChar('\n'); }
};

enum Status : uint8_t {
    ST_OK, ST_NEGATIVE, ST_NO_RESPONSE, ST_FC_TIMEOUT, ST_FC_ABORT, ST_TX_FAIL,
    ST_TX_TIMEOUT, ST_TOTAL_TIMEOUT, ST_MODE_FAIL, ST_PROTOCOL, ST_INVALID,
    ST_TOO_MANY_PENDING, ST_READBACK_MISMATCH, ST_READBACK_FAIL, ST_DRYRUN, ST_ESTOP
};
inline const char *statusText(Status s){
    switch(s){case ST_OK:return "OK";case ST_NEGATIVE:return "NEGATIVE";case ST_NO_RESPONSE:return "NO_RESPONSE";
    case ST_FC_TIMEOUT:return "FC_TIMEOUT";case ST_FC_ABORT:return "FC_OVERFLOW_ABORT";case ST_TX_FAIL:return "TX_FAILED";
    case ST_TX_TIMEOUT:return "TX_TIMEOUT";case ST_TOTAL_TIMEOUT:return "TOTAL_TIMEOUT";case ST_MODE_FAIL:return "MODE_FAIL";
    case ST_PROTOCOL:return "PROTOCOL_ERROR";case ST_INVALID:return "INVALID_RESPONSE";case ST_TOO_MANY_PENDING:return "TOO_MANY_PENDING";
    case ST_READBACK_MISMATCH:return "READBACK_MISMATCH";case ST_READBACK_FAIL:return "READBACK_FAIL";
    case ST_DRYRUN:return "DRYRUN";case ST_ESTOP:return "ESTOP";}
    return "?";
}

// ---- shell --------------------------------------------------------------------
class WriteShell {
public:
    explicit WriteShell(Hal &hal) : h(hal), o(hal) {}

    void begin() {
        o.s("OpenMoto Phase 9 SERVICE-WRITE client\n");
        o.s(OPENMOTO_WRITE_ALLOW_TX ? "BUILD tx=ENABLED (service_write)\n" : "BUILD tx=DRYRUN (no transmit code)\n");
        o.s("WRITES cluster NVM. Idle=LISTEN_ONLY. ARM+CONFIRM per write; read-back verified.\n");
        o.s("NOT FOR VEHICLE USE until docs/diag-kombi-service-reset.md prereqs are met\n");
        if (!h.canSilence()) { estopped = true; o.s("ALERT listen-only-unverified at boot; latched off\n"); }
        o.s("READY send STATUS\n");
    }

    void poll() {
        int ch;
        for (uint8_t b = 48; b && (ch = h.readChar()) >= 0; --b) onChar(char(ch));
        const uint32_t now = h.nowMs();
        if (state == S_ARMED && elapsed(now, armedAt) > ARM_TTL_MS) disarm("expired");
        if (busy()) step(now);
    }

    bool isEstopped() const { return estopped; }

private:
    enum State : uint8_t { S_IDLE, S_ARMED, S_TX_FF, S_WAIT_FC, S_TX_CF, S_WAIT_RESP, S_READBACK };
    bool busy() const { return state >= S_TX_FF; }

    Hal &h; Out o;
    State state = S_IDLE;
    char line[LINE_W_MAX + 1]; uint8_t lineLen = 0; bool lineOvf = false;
    bool estopped = false, lockedOut = false;
    bool addrSet = false, padSet = false;
    uint16_t txId = 0, rxId = 0; int16_t txExt = -1, rxExt = -1, pad = -1;
    uint8_t writeCount = 0, badConfirms = 0;
    bool haveLastEnd = false; uint32_t lastEnd = 0;

    // armed write
    uint8_t req[10]; uint8_t reqLen = 0;
    uint16_t armCode = 0; uint32_t armedAt = 0; uint16_t armSeq = 0;

    // transaction
    uint32_t tStart = 0, phaseAt = 0, txStart = 0;
    uint8_t outBuf[10]; uint8_t outLen = 0, outSent = 0;   // multi-frame TX payload
    uint8_t nextCfSn = 1, fcBs = 0, fcStmin = 0, cfLeftInBlock = 0;
    uint8_t resp[16]; uint16_t respLen = 0, respTotal = 0; uint8_t rbNextSn = 1;
    uint8_t pendingCount = 0;
    bool doingReadback = false;
    bool readOnly = false;                  // standalone READ (22 E1 xx), never a write
    uint8_t rbReq[3];                       // the 22 E1 xx readback request

    static uint32_t elapsed(uint32_t n,uint32_t s){ return uint32_t(n-s); }
    static bool beyond(uint32_t n,uint32_t s,uint32_t ms){ return elapsed(n,s) > ms; }
    static bool reached(uint32_t n,uint32_t t){ return int32_t(n-t) >= 0; }
    static int hv(char c){ if(c>='0'&&c<='9')return c-'0'; if(c>='a'&&c<='f')return c-'a'+10; if(c>='A'&&c<='F')return c-'A'+10; return -1; }
    static bool pnum(const char*s,uint32_t mx,uint32_t&o){ if(!*s)return false; uint32_t v=0; for(uint8_t n=0;*s;++s,++n){int d=hv(*s); if(d<0||n>=8)return false; v=v<<4|uint32_t(d); if(v>mx)return false;} o=v; return true; }
    static bool pbytes(char**t,uint8_t nt,uint8_t*o,uint8_t cap,uint8_t&n){ n=0; for(uint8_t i=0;i<nt;++i){const char*s=t[i]; size_t L=strlen(s); if(!L||(L&1))return false; for(size_t j=0;j<L;j+=2){int a=hv(s[j]),b=hv(s[j+1]); if(a<0||b<0||n>=cap)return false; o[n++]=uint8_t(a<<4|b);}} return n>0; }

    void onChar(char c){
        if (c=='!'){ lineLen=0; lineOvf=false; estop("bang"); return; }
        if (c=='\r') return;
        if (c=='\n'){ line[lineLen]=0; bool ov=lineOvf; uint8_t L=lineLen; lineLen=0; lineOvf=false;
            if(ov){o.s("ERR line-too-long\n");return;} if(L) exec(); return; }
        if (lineLen<LINE_W_MAX) line[lineLen++]=c; else lineOvf=true;
    }

    void exec(){
        char *tok[8]; uint8_t nt=0; char *p=line;
        while(*p && nt<8){ while(*p==' '||*p=='\t')*p++=0; if(!*p)break; tok[nt++]=p; while(*p&&*p!=' '&&*p!='\t')++p; }
        if(nt==0) return;
        for(char*c=tok[0];*c;++c) if(*c>='a'&&*c<='z')*c=char(*c-32);
        const char*cmd=tok[0];
        if(!strcmp(cmd,"STATUS")){ status(); return; }
        if(!strcmp(cmd,"ESTOP")){ estop("command"); return; }
        if(estopped||lockedOut){ o.s(estopped?"ERR estop-latched\n":"ERR lockout\n"); return; }
        if(!strcmp(cmd,"ADDR")) cmdAddr(tok+1,nt-1);
        else if(!strcmp(cmd,"PAD")) cmdPad(tok+1,nt-1);
        else if(!strcmp(cmd,"ARM")) cmdArm(tok+1,nt-1);
        else if(!strcmp(cmd,"READ")) cmdRead(tok+1,nt-1);
        else if(!strcmp(cmd,"CONFIRM")) cmdConfirm(tok+1,nt-1);
        else if(!strcmp(cmd,"DISARM")){ if(state==S_ARMED) disarm("command"); else o.s("OK nothing-armed\n"); }
        else if(!strcmp(cmd,"STOP")){ abortToSilent("command"); o.s("OK stopped\n"); }
        else o.s("ERR unknown-command\n");
    }

    void status(){
        o.s("STATUS state="); o.s(state==S_IDLE?"IDLE":state==S_ARMED?"ARMED":"BUSY");
        o.s(" addr="); o.s(addrSet?"SET":"UNSET"); o.s(" pad="); o.s(padSet?"SET":"UNSET");
        o.s(" writes="); o.u(writeCount); o.s(" estop="); o.u(estopped); o.s(" lockout="); o.u(lockedOut);
        o.s(" tx_build="); o.u(OPENMOTO_WRITE_ALLOW_TX); o.nl();
    }
    void cmdAddr(char**a,uint8_t n){
        if(state!=S_IDLE&&state!=S_ARMED){o.s("ERR busy\n");return;}
        uint32_t t,r,te=0,re=0; bool ext=(n==4);
        if(!(n==2||n==4)||!pnum(a[0],0x7FF,t)||!pnum(a[1],0x7FF,r)||(ext&&(!pnum(a[2],0xFF,te)||!pnum(a[3],0xFF,re)))){
            o.s("ERR bad-addr\n"); return; }
        if(t==0||r==0||t==0x7DF||r==0x7DF||t==r){o.s("ERR addr-forbidden\n");return;}
        if(state==S_ARMED) disarm("addr-changed");
        if(!h.canSetRxId(uint16_t(r))){o.s("ERR rx-filter-failed\n");addrSet=false;return;}
        txId=uint16_t(t); rxId=uint16_t(r); txExt=ext?int16_t(te):int16_t(-1); rxExt=ext?int16_t(re):int16_t(-1);
        addrSet=true; o.s("OK addr tx="); o.id(txId); o.s(" rx="); o.id(rxId);
        if(ext){o.s(" txext=");o.hex(uint8_t(te));o.s(" rxext=");o.hex(uint8_t(re));} o.nl();
    }
    void cmdPad(char**a,uint8_t n){
        if(state!=S_IDLE&&state!=S_ARMED){o.s("ERR busy\n");return;}
        uint32_t v; if(n!=1){o.s("ERR bad-pad\n");return;}
        if(!strcmp(a[0],"NONE")||!strcmp(a[0],"none")) pad=-1;
        else if(pnum(a[0],0xFF,v)&&strlen(a[0])==2) pad=int16_t(v);
        else {o.s("ERR bad-pad\n");return;}
        if(state==S_ARMED) disarm("pad-changed");
        padSet=true; o.s("OK pad="); if(pad<0)o.s("NONE"); else o.hex(uint8_t(pad)); o.nl();
    }

    uint8_t headLen() const { return txExt>=0?1:0; }

    // Standalone READ-ONLY: READ 22E119 | 22E12B | 22E12C | 22E12D. Cannot write.
    // Reuses the exact read-back transport; no ARM/CONFIRM (non-mutating), but the
    // request is hard-restricted to service 0x22 + the four known DIDs.
    void cmdRead(char**a,uint8_t n){
        if(state!=S_IDLE&&state!=S_ARMED){o.s("ERR busy\n");return;}
        uint8_t b[8],len;
        if(n==0||!pbytes(a,n,b,sizeof(b),len)){o.s("ERR bad-hex\n");return;}
        if(len!=3||b[0]!=0x22||b[1]!=0xE1||!(b[2]==0x19||b[2]==0x2B||b[2]==0x2C||b[2]==0x2D)){
            o.s("ERR read-not-allowed (only 22 E1 19|2B|2C|2D)\n");return;}
        if(!addrSet||!padSet){o.s("ERR not-configured (ADDR and PAD first)\n");return;}
        if(state==S_ARMED) disarm("read-issued");
        const uint32_t now=h.nowMs();
        // set up as a read-only transaction: req holds the 22 request for logging.
        memcpy(req,b,3); reqLen=3;
        readOnly=true; doingReadback=true;   // response path treats 62 E1 xx as the result
        startRead(now);
    }
    void startRead(uint32_t now){
        tStart=now; respLen=respTotal=0; pendingCount=0;
        if(!OPENMOTO_WRITE_ALLOW_TX){ finish(ST_DRYRUN); return; }
        if(!h.canEnterActive()){ finish(ST_MODE_FAIL); return; }
        Frame stale; for(uint8_t i=0;i<16&&h.canReceive(stale);++i){}
        memcpy(outBuf,req,3); outLen=3; outSent=0; nextCfSn=1;
        if(!sendSingle(outBuf,3)){ finish(ST_TX_FAIL); return; }
        outSent=3; txStart=h.nowMs(); state=S_WAIT_RESP; phaseAt=now;
    }

    void cmdArm(char**a,uint8_t n){
        if(state!=S_IDLE&&state!=S_ARMED){o.s("ERR busy\n");return;}
        uint8_t b[10],len;
        if(n==0||!pbytes(a,n,b,sizeof(b),len)){o.s("ERR bad-hex\n");return;}
        if(!validWriteReq(b,len)){o.s("ERR not-an-allowed-service-write (only 2E E1 2B/2C/2D, exact length)\n");return;}
        if(!addrSet||!padSet){o.s("ERR not-configured (ADDR and PAD first)\n");return;}
        if(writeCount>=MAX_WRITES){o.s("ERR write-limit reset-required\n");return;}
        const uint32_t now=h.nowMs();
        if(haveLastEnd&&elapsed(now,lastEnd)<MIN_GAP_MS){o.s("ERR rate-limit\n");return;}
        if(state==S_ARMED) disarm("rearmed");
        memcpy(req,b,len); reqLen=len;
        // confirm code: FNV-1a over the exact request bytes + addressing + counter + time
        uint32_t x=2166136261UL;
        uint8_t mix[]={uint8_t(txId>>8),uint8_t(txId),uint8_t(rxId>>8),uint8_t(rxId),uint8_t(txExt),uint8_t(rxExt),
                       uint8_t(pad),uint8_t(armSeq),uint8_t(armSeq>>8),uint8_t(now),uint8_t(now>>8),uint8_t(now>>16)};
        for(uint8_t i=0;i<sizeof(mix);++i){x^=mix[i];x*=16777619UL;}
        for(uint8_t i=0;i<reqLen;++i){x^=req[i];x*=16777619UL;}
        armCode=uint16_t((x>>16)^x); if(!armCode)armCode=0xB33F; ++armSeq; armedAt=now; state=S_ARMED;
        o.s("ARMED code="); o.hex(uint8_t(armCode>>8)); o.hex(uint8_t(armCode));
        o.s(" write="); o.bytes(req,reqLen);
        o.s(" did=E1"); o.hex(req[2]);
        o.s(" frames="); o.u(uint8_t((reqLen+headLen()>7)?/*multi*/2:1));  // informational
        o.s(" readback=22E1"); o.hex(req[2]);
        o.s(" expires_ms="); o.u(ARM_TTL_MS);
        o.s(OPENMOTO_WRITE_ALLOW_TX?" tx_build=1\n":" tx_build=0\n");
    }
    void disarm(const char*w){ if(state==S_ARMED)state=S_IDLE; armCode=0; o.s("DISARMED reason="); o.s(w); o.nl(); }

    void cmdConfirm(char**a,uint8_t n){
        const uint32_t now=h.nowMs();
        if(state!=S_ARMED){o.s("ERR not-armed\n");return;}
        uint32_t code;
        if(n!=1||strlen(a[0])!=4||!pnum(a[0],0xFFFF,code)||code!=armCode){
            disarm("bad-code"); if(++badConfirms>=MAX_BAD_CONFIRMS) estop("too-many-bad-confirmations"); return; }
        badConfirms=0;
        if(elapsed(now,armedAt)>ARM_TTL_MS){disarm("expired");return;}
        armCode=0;
        start(now);
    }

    // ---- ISO-TP multi-frame transmit + response + read-back ----
    void start(uint32_t now){
        tStart=now; respLen=respTotal=0; pendingCount=0; doingReadback=false; readOnly=false;
        if(!OPENMOTO_WRITE_ALLOW_TX){ finish(ST_DRYRUN); return; }
        if(!h.canEnterActive()){ finish(ST_MODE_FAIL); return; }
        Frame stale; for(uint8_t i=0;i<16&&h.canReceive(stale);++i){}
        // Build the write message into outBuf (UDS bytes) and begin ISO-TP TX.
        memcpy(outBuf,req,reqLen); outLen=reqLen; outSent=0; nextCfSn=1;
        if(uint8_t(outLen) <= uint8_t(7-headLen())){
            // single frame
            if(!sendSingle(outBuf,outLen)){ finish(ST_TX_FAIL); return; }
            outSent=outLen; txStart=h.nowMs(); state=S_WAIT_RESP; phaseAt=now;
        } else {
            // first frame
            if(!sendFirst()){ finish(ST_TX_FAIL); return; }
            txStart=h.nowMs(); state=S_TX_FF;
        }
    }

    bool frameOut(const uint8_t *body, uint8_t bodyLen){
        Frame f; memset(&f,0,sizeof(f)); f.id=txId; uint8_t k=0;
        if(txExt>=0) f.data[k++]=uint8_t(txExt);
        for(uint8_t i=0;i<bodyLen;++i) f.data[k++]=body[i];
        if(pad>=0){ while(k<8)f.data[k++]=uint8_t(pad); f.dlc=8; } else f.dlc=k;
        return h.canTransmit(f);
    }
    bool sendSingle(const uint8_t*p,uint8_t len){ uint8_t body[8]; body[0]=len; memcpy(body+1,p,len); return frameOut(body,uint8_t(1+len)); }
    bool sendFirst(){
        uint8_t body[8]; body[0]=uint8_t(0x10|((outLen>>8)&0x0F)); body[1]=uint8_t(outLen&0xFF);
        uint8_t can = uint8_t(6-headLen());  // data bytes in FF after the 2 PCI bytes (6 with ext, else... )
        // with ext: frame = [ext][10 LL][d..] -> 5 data bytes; without ext: 6
        uint8_t navail = uint8_t(txExt>=0?5:6);
        for(uint8_t i=0;i<navail&&i<outLen;++i) body[2+i]=outBuf[i];
        (void)can;
        outSent=uint8_t(navail<outLen?navail:outLen);
        Frame f; memset(&f,0,sizeof(f)); f.id=txId; uint8_t k=0; if(txExt>=0)f.data[k++]=uint8_t(txExt);
        f.data[k++]=body[0]; f.data[k++]=body[1];
        for(uint8_t i=0;i<outSent;++i) f.data[k++]=outBuf[i];
        while(k<8)f.data[k++]=(pad>=0?uint8_t(pad):0); f.dlc=8;
        return h.canTransmit(f);
    }

    void step(uint32_t now){
        if(beyond(now,tStart,TOTAL_MS)){ finish(ST_TOTAL_TIMEOUT); return; }
        // TX completion of the frame currently queued
        if(state==S_TX_FF||state==S_TX_CF){
            TxState ts=h.canTxState();
            if(ts==TXS_FAILED){ finish(ST_TX_FAIL); return; }
            if(ts==TXS_SENT){
                if(state==S_TX_FF){ state=S_WAIT_FC; phaseAt=now; }
                else { // a CF left; more to send?
                    if(outSent>=outLen){ state=S_WAIT_RESP; phaseAt=now; }
                    else if(cfLeftInBlock==0 && fcBs!=0){ state=S_WAIT_FC; phaseAt=now; }
                    else { sendNextCf(now); }
                }
            } else if(beyond(now,txStart,TX_DONE_MS)){ finish(ST_TX_TIMEOUT); return; }
            return;
        }
        Frame f;
        if(state==S_WAIT_FC){
            for(uint8_t i=0;i<8&&h.canReceive(f);++i) if(acceptFrom(f)){ onFlowControl(f,now); return; }
            if(beyond(now,phaseAt,FC_WAIT_MS)) finish(ST_FC_TIMEOUT);
            return;
        }
        if(state==S_WAIT_RESP||state==S_READBACK){
            for(uint8_t i=0;i<8&&h.canReceive(f);++i) if(acceptFrom(f)){ onResp(f,now); return; }
            uint32_t lim = (pendingCount>0?P2_STAR_MS:P2_MS);
            if(beyond(now,phaseAt,lim)) finish(ST_NO_RESPONSE);
            return;
        }
    }

    bool acceptFrom(const Frame&f){
        if(f.id!=rxId) return false;
        if(rxExt>=0){ if(f.dlc<1||f.data[0]!=uint8_t(rxExt)) return false; }
        return true;
    }
    uint8_t off() const { return rxExt>=0?1:0; }

    void onFlowControl(const Frame&f,uint32_t now){
        uint8_t o0=off(); if(f.dlc<o0+1){ finish(ST_PROTOCOL); return; }
        uint8_t pci=f.data[o0]; if((pci>>4)!=3){ finish(ST_PROTOCOL); return; }
        uint8_t fs=pci&0x0F;
        if(fs==2){ finish(ST_FC_ABORT); return; }      // overflow/abort
        if(fs==1){ phaseAt=now; return; }              // wait: stay in S_WAIT_FC
        // fs==0 ClearToSend
        fcBs = f.dlc>o0+1?f.data[o0+1]:0;
        fcStmin = f.dlc>o0+2?f.data[o0+2]:0;
        cfLeftInBlock = fcBs;                           // 0 = unlimited
        state=S_TX_CF; sendNextCf(now);
    }

    void sendNextCf(uint32_t now){
        uint8_t o0=headLen();
        Frame f; memset(&f,0,sizeof(f)); f.id=txId; uint8_t k=0; if(txExt>=0)f.data[k++]=uint8_t(txExt);
        f.data[k++]=uint8_t(0x20|(nextCfSn&0x0F));
        uint8_t cap=uint8_t(7-o0);
        uint8_t take=uint8_t(outLen-outSent); if(take>cap)take=cap;
        for(uint8_t i=0;i<take;++i) f.data[k++]=outBuf[outSent+i];
        while(k<8)f.data[k++]=(pad>=0?uint8_t(pad):0); f.dlc=8;
        outSent=uint8_t(outSent+take); nextCfSn=uint8_t((nextCfSn+1)&0x0F);
        if(cfLeftInBlock) --cfLeftInBlock;
        txStart=now;
        if(!h.canTransmit(f)){ finish(ST_TX_FAIL); return; }
        // stay in S_TX_CF; step() checks TXS and decides next CF / FC wait / resp wait
    }

    void onResp(const Frame&f,uint32_t now){
        uint8_t o0=off(); if(f.dlc<=o0){ finish(ST_PROTOCOL); return; }
        uint8_t pci=f.data[o0]; uint8_t type=pci>>4; uint8_t cap=uint8_t(7-o0);
        if(type==0){ // single frame response
            uint8_t len=pci&0x0F; if(len==0||len>cap||f.dlc<o0+1+len){ finish(ST_PROTOCOL); return; }
            memcpy(resp,f.data+o0+1,len); respLen=len; complete(now);
        } else if(type==1){ // multi-frame response (e.g. read-back of E12B)
            uint16_t total=uint16_t((pci&0x0F)<<8|(f.dlc>o0+1?f.data[o0+1]:0));
            if(f.dlc!=8||total<=cap||total>sizeof(resp)){ finish(ST_PROTOCOL); return; }
            uint8_t nb=uint8_t(6-o0); memcpy(resp,f.data+o0+2,nb); respLen=nb; respTotal=total; rbNextSn=1;
            // send a flow control (ClearToSend, BS=0, STmin) so the ECU streams the rest
            sendRbFlowControl(); state=S_WAIT_RESP; phaseAt=now;
        } else if(type==2){ // consecutive of the read-back
            if((pci&0x0F)!=(rbNextSn&0x0F)){ finish(ST_PROTOCOL); return; }
            uint16_t rem=uint16_t(respTotal-respLen); uint8_t take=rem<cap?uint8_t(rem):cap;
            if(f.dlc<o0+1+take){ finish(ST_PROTOCOL); return; }
            memcpy(resp+respLen,f.data+o0+1,take); respLen=uint16_t(respLen+take); rbNextSn=uint8_t((rbNextSn+1)&0x0F);
            phaseAt=now; if(respLen>=respTotal) complete(now);
        }
    }

    void sendRbFlowControl(){
        uint8_t body[3]={0x30,0x00,N_CS_MIN_MS};
        Frame f; memset(&f,0,sizeof(f)); f.id=txId; uint8_t k=0; if(txExt>=0)f.data[k++]=uint8_t(txExt);
        for(uint8_t i=0;i<3;++i) f.data[k++]=body[i];
        if(pad>=0){ while(k<8)f.data[k++]=uint8_t(pad); f.dlc=8;} else f.dlc=k;
        h.canTransmit(f);
    }

    // A completed UDS response is in resp[0..respLen). Handle the write result,
    // then the read-back, then compare.
    void complete(uint32_t now){
        const uint8_t sid = doingReadback?0x22:0x2E;
        if(resp[0]==0x7F){
            if(respLen!=3||resp[1]!=sid){ finish(ST_INVALID); return; }
            if(resp[2]==0x78){ if(++pendingCount>MAX_PENDING){finish(ST_TOO_MANY_PENDING);return;} respLen=0; state=S_WAIT_RESP; phaseAt=now; return; }
            lastNrc=resp[2]; finish(ST_NEGATIVE); return;
        }
        if(!doingReadback){
            // expect 6E E1 xx
            if(respLen<3||resp[0]!=0x6E||resp[1]!=0xE1||resp[2]!=req[2]){ finish(ST_INVALID); return; }
            // write accepted by ECU -> now READ BACK the same DID and compare data.
            doingReadback=true; pendingCount=0;
            rbReq[0]=0x22; rbReq[1]=0xE1; rbReq[2]=req[2];
            memcpy(outBuf,rbReq,3); outLen=3; outSent=0; nextCfSn=1; respLen=respTotal=0;
            if(!sendSingle(rbReq,3)){ finish(ST_READBACK_FAIL); return; }
            outSent=3; txStart=h.nowMs(); state=S_WAIT_RESP; phaseAt=now; return;
        }
        // read-back response: expect 62 E1 xx <data...>.
        if(respLen<3||resp[0]!=0x62||resp[1]!=0xE1||resp[2]!=req[2]){ finish(readOnly?ST_INVALID:ST_READBACK_FAIL); return; }
        if(readOnly){ finish(ST_OK); return; }   // standalone READ: response is the result
        // post-write read-back: compare data to what we wrote.
        const uint8_t wroteLen = uint8_t(reqLen-3);    // payload bytes we wrote
        const uint8_t gotLen = uint8_t(respLen-3);
        bool match = (gotLen==wroteLen) && (memcmp(resp+3, req+3, wroteLen)==0);
        finish(match?ST_OK:ST_READBACK_MISMATCH);
    }

    uint8_t lastNrc=0;
    void finish(Status st){
        const uint32_t now=h.nowMs();
        const bool silent=h.canSilence();
        if(!silent){ estopped=true; o.s("ALERT listen-only-unverified; latched off\n"); }
        state=S_IDLE;
        // A standalone READ is non-mutating: it does not count against the write limit.
        if(st!=ST_DRYRUN && !readOnly){ ++writeCount; }
        if(st!=ST_DRYRUN){ haveLastEnd=true; lastEnd=now; }
        o.s("RESULT status="); o.s(statusText(st));
        o.s(readOnly?" read=":" write="); o.bytes(req,reqLen);
        if(st==ST_NEGATIVE){ o.s(" nrc="); o.hex(lastNrc); }
        if(doingReadback||st==ST_OK||st==ST_READBACK_MISMATCH){ o.s(readOnly?" value=":" readback="); o.bytes(resp,respLen>16?16:uint8_t(respLen)); }
        o.s(" verified="); o.s((st==ST_OK&&!readOnly)?"YES":readOnly?"n/a":"NO");
        o.s(" ms="); o.u(elapsed(now,tStart)); o.s(" silent="); o.u(silent); o.nl();
        if(st==ST_READBACK_MISMATCH) o.s("ALERT readback-mismatch: cluster value != written value; use rollback reference\n");
        readOnly=false;
    }

    void estop(const char*w){ estopped=true; armCode=0; abortToSilent(w); o.s("EVT estop reason="); o.s(w); o.s(" latched=1\n"); }
    void abortToSilent(const char*){ h.canSilence(); if(state!=S_IDLE) state=S_IDLE; armCode=0; }
};

}  // namespace diagwrite
