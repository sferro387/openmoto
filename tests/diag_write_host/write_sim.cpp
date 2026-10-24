// OpenMoto Phase 9 host simulator: drives the REAL service-write core
// (firmware/mega-can/src/diag_write/write_core.h) against a write-capable mock
// KOMBI. Exercises ISO-TP multi-frame TX (first frame -> flow control ->
// consecutive), the write result (6E / NRC), the read-back, and the compare.
// Nothing talks to hardware or a vehicle.
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <deque>
#include <string>
#include <map>

#ifndef OPENMOTO_WRITE_ALLOW_TX
#define OPENMOTO_WRITE_ALLOW_TX 1
#endif
#include "../../firmware/mega-can/src/diag_write/write_core.h"

namespace {
using diagwrite::Frame;

struct MockKombi {
    enum Mode { M_OK, M_NRC33, M_PENDING_OK, M_READBACK_WRONG, M_NO_FC, M_SILENT };
    Mode mode = M_OK;
    uint16_t txId=0, rxId=0; int16_t txExt=-1, rxExt=-1;
    std::deque<Frame> out;
    // persistent "NVM": did low byte -> stored data bytes
    std::map<uint8_t, std::vector<uint8_t>> nvm;
    // reassembly of an incoming multi-frame write
    std::vector<uint8_t> rx; uint16_t rxTotal=0; bool rxActive=false; uint8_t pend=0;

    void configure(uint16_t t,uint16_t r,int16_t te,int16_t re){ txId=t;rxId=r;txExt=te;rxExt=re; out.clear();
        // seed NVM with the real live values
        nvm[0x2B]={0x00,0x33,0x0A,0x18,0x0A,0x07,0xEA};
        nvm[0x2C]={0x09,0x09,0x07,0xEA};
        nvm[0x2D]={0x14,0x3C};
        nvm[0x19]={0x00,0x00,0x19,0x2C};
    }
    uint8_t ho() const { return rxExt>=0?1:0; }

    void onTesterFrame(const Frame&f){
        if(f.id!=txId) return;
        uint8_t o=0; if(txExt>=0){ if(f.dlc<1||f.data[0]!=uint8_t(txExt))return; o=1; }
        if(f.dlc<=o) return;
        uint8_t pci=f.data[o], type=pci>>4;
        if(type==0){ uint8_t len=pci&0x0F; std::vector<uint8_t> msg(f.data+o+1,f.data+o+1+len); onUds(msg); }
        else if(type==1){ rxTotal=((pci&0x0F)<<8)|f.data[o+1]; rx.assign(f.data+o+2,f.data+8); rxActive=true;
            if(mode!=M_NO_FC) sendFc(); }
        else if(type==2){ if(!rxActive)return; for(uint8_t i=o+1;i<8&&rx.size()<rxTotal;++i) rx.push_back(f.data[i]);
            if(rx.size()>=rxTotal){ rxActive=false; std::vector<uint8_t> msg(rx.begin(),rx.begin()+rxTotal); onUds(msg); } }
        else if(type==3){ /* tester flow control for our read-back reply: stream CFs */ streamPendingCfs(); }
    }

    void sendFc(){ Frame f; mk(f); uint8_t k=ho(); f.data[k++]=0x30; f.data[k++]=0x00; f.data[k++]=0x00; f.dlc=8; out.push_back(f); }
    void mk(Frame&f){ memset(&f,0,sizeof(f)); f.id=rxId; if(rxExt>=0)f.data[0]=uint8_t(rxExt); }

    std::vector<uint8_t> pendingReply; uint16_t replySent=0; bool replyMulti=false;

    void onUds(const std::vector<uint8_t>&req){
        if(mode==M_SILENT) return;
        uint8_t sid=req[0];
        if(mode==M_NRC33){ emit({0x7F,sid,0x33}); return; }
        if(mode==M_PENDING_OK){
            // Two unsolicited responsePending (0x78), then the real reply — all
            // queued; the tester consumes one 0x78 per P2* window, then the reply.
            emit({0x7F,sid,0x78}); emit({0x7F,sid,0x78});
        }
        if(sid==0x2E){ // write: store, reply 6E E1 xx
            uint8_t did=req[2]; std::vector<uint8_t> data(req.begin()+3,req.end());
            if(mode==M_READBACK_WRONG){ data[0]^=0xFF; }   // store a corrupted value
            nvm[did]=data; emit({0x6E,0xE1,did});
        } else if(sid==0x22){ // read-back
            uint8_t did=req[2]; std::vector<uint8_t> r={0x62,0xE1,did};
            auto it=nvm.find(did); if(it!=nvm.end()) r.insert(r.end(),it->second.begin(),it->second.end());
            emit(r);
        }
    }

    // Emit a UDS reply as ISO-TP (single or multi-frame toward the tester).
    void emit(const std::vector<uint8_t>&p){
        uint8_t head=rxExt>=0?1:0;
        if(p.size()+head+1<=8){ Frame f; mk(f); uint8_t k=head; f.data[k++]=uint8_t(p.size());
            for(auto b:p)f.data[k++]=b; while(k<8)f.data[k++]=0; f.dlc=8; out.push_back(f); return; }
        // multi-frame: first frame, then wait for tester FC (type 3) to stream CFs
        pendingReply=p; replySent=0; replyMulti=true;
        Frame ff; mk(ff); uint8_t k=head; ff.data[k++]=uint8_t(0x10|((p.size()>>8)&0x0F)); ff.data[k++]=uint8_t(p.size()&0xFF);
        uint8_t navail=uint8_t(8-k); for(uint8_t i=0;i<navail&&i<p.size();++i)ff.data[k++]=p[i];
        replySent=navail<p.size()?navail:p.size(); ff.dlc=8; out.push_back(ff);
    }
    void streamPendingCfs(){
        if(!replyMulti) return; uint8_t head=rxExt>=0?1:0; uint8_t sn=1;
        while(replySent<pendingReply.size()){ Frame f; mk(f); uint8_t k=head; f.data[k++]=uint8_t(0x20|(sn&0x0F));
            uint8_t cap=uint8_t(8-k); for(uint8_t i=0;i<cap&&replySent<pendingReply.size();++i)f.data[k++]=pendingReply[replySent++];
            while(k<8)f.data[k++]=0; f.dlc=8; out.push_back(f); ++sn; }
        replyMulti=false;
    }
};

class SimHal : public diagwrite::Hal {
public:
    MockKombi &ecu; uint32_t clock=1000; bool active=false; uint16_t rxFilter=0x7FF;
    std::deque<Frame> rx; std::string in,out; diagwrite::TxState lastTx=diagwrite::TXS_NONE;
    bool failActive=false;
    explicit SimHal(MockKombi&e):ecu(e){}
    void feed(const std::string&l){ in+=l; in+='\n'; }
    void advance(uint32_t ms){ clock+=ms; }
    uint32_t nowMs() override { return clock; }
    int readChar() override { if(in.empty())return -1; char c=in.front(); in.erase(in.begin()); return (unsigned char)c; }
    void putChar(char c) override { out.push_back(c); }
    bool canSilence() override { active=false; lastTx=diagwrite::TXS_NONE; return true; }
    bool canSetRxId(uint16_t id) override { rxFilter=id; active=false; return true; }
    bool canEnterActive() override { if(failActive)return false; active=true; return true; }
    bool canTransmit(const Frame&f) override { if(!active)return false; ecu.onTesterFrame(f); lastTx=diagwrite::TXS_SENT; deliver(); return true; }
    diagwrite::TxState canTxState() override { return lastTx; }
    bool canReceive(Frame&f) override { if(rx.empty())return false; f=rx.front(); rx.pop_front(); return true; }
private:
    void deliver(){ for(auto&f:ecu.out) if(f.id==rxFilter) rx.push_back(f); ecu.out.clear(); }
};

struct Driver {
    MockKombi ecu; SimHal hal{ecu}; diagwrite::WriteShell shell{hal};
    Driver(){ shell.begin(); flush(); }
    void flush(){ if(!hal.out.empty()){ std::fputs(hal.out.c_str(),stdout); hal.out.clear(); } }
    void send(const std::string&l){ syncAddr(l); hal.feed(l);
        for(int i=0;i<600;++i){ shell.poll(); if(!hal.rx.empty()||!hal.in.empty())continue; hal.advance(20); } flush(); }
    void syncAddr(const std::string&l){ char c[8]; unsigned t=0,r=0,te=0,re=0;
        int n=std::sscanf(l.c_str(),"%7s %x %x %x %x",c,&t,&r,&te,&re);
        if(n>=3&&(!strcmp(c,"ADDR")||!strcmp(c,"addr"))){ if(n>=5)ecu.configure(t,r,te,re); else ecu.configure(t,r,-1,-1); } }
    void armConfirm(const std::string&w){ hal.feed("ARM "+w+"\n"); for(int i=0;i<60&&!hal.in.empty();++i)shell.poll();
        std::string a=hal.out; std::fputs(a.c_str(),stdout); hal.out.clear();
        auto p=a.rfind("code="); std::string code=p==std::string::npos?"0000":a.substr(p+5,4);
        send("CONFIRM "+code); }
};

}  // namespace

int main(int argc,char**argv){
    std::string sc = argc>1?argv[1]:"stdin";
    if(sc=="stdin"){ Driver d; std::fputs("EOC\n",stdout); std::fflush(stdout); char buf[160];
        while(std::fgets(buf,sizeof(buf),stdin)){ std::string l(buf); if(!l.empty()&&l.back()=='\n')l.pop_back();
            d.send(l); std::fputs("EOC\n",stdout); std::fflush(stdout); } return 0; }
    Driver d;
    auto setup=[&](MockKombi::Mode m){ d.send("ADDR 6F1 660 60 F1"); d.send("PAD 00"); d.ecu.configure(0x6F1,0x660,0x60,0xF1); d.ecu.mode=m; };
    if(sc=="write_e12d"){ setup(MockKombi::M_OK); d.armConfirm("2EE12D2710"); }
    else if(sc=="write_e12c"){ setup(MockKombi::M_OK); d.armConfirm("2EE12C180A07EB"); }
    else if(sc=="write_e12b"){ setup(MockKombi::M_OK); d.armConfirm("2EE12B00330A180A07EA"); }
    else if(sc=="nrc"){ setup(MockKombi::M_NRC33); d.armConfirm("2EE12D2710"); }
    else if(sc=="pending_ok"){ setup(MockKombi::M_PENDING_OK); d.armConfirm("2EE12D2710"); }
    else if(sc=="readback_wrong"){ setup(MockKombi::M_READBACK_WRONG); d.armConfirm("2EE12D2710"); }
    else if(sc=="no_fc"){ setup(MockKombi::M_NO_FC); d.armConfirm("2EE12C180A07EB"); }
    else if(sc=="silent"){ setup(MockKombi::M_SILENT); d.armConfirm("2EE12D2710"); }
    else if(sc=="deny_bad"){ setup(MockKombi::M_OK); d.send("ARM 2EE1AA00"); }       // wrong DID -> denied
    else if(sc=="deny_write_other"){ setup(MockKombi::M_OK); d.send("ARM 2FE12C01"); } // IO control -> denied
    else if(sc=="estop"){ setup(MockKombi::M_OK); d.send("ARM 2EE12D2710"); d.send("!"); d.send("STATUS"); d.send("ARM 2EE12D2710"); }
    else if(sc=="read_e12d"){ setup(MockKombi::M_OK); d.send("READ 22E12D"); }
    else if(sc=="read_e12c"){ setup(MockKombi::M_OK); d.send("READ 22E12C"); }
    else if(sc=="read_e119"){ setup(MockKombi::M_OK); d.send("READ 22E119"); }
    else if(sc=="read_e12b"){ setup(MockKombi::M_OK); d.send("READ 22E12B"); }
    else if(sc=="read_denied"){ setup(MockKombi::M_OK); d.send("READ 22F190"); }     // not one of the four
    else if(sc=="read_cant_write"){ setup(MockKombi::M_OK); d.send("READ 2EE12D2710"); } // READ refuses a 2E
    else if(sc=="status"){ d.send("STATUS"); }
    else { std::fprintf(stderr,"unknown %s\n",sc.c_str()); return 2; }
    return 0;
}
