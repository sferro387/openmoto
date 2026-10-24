// OpenMoto Phase 9: SERVICE-WRITE client, Arduino Mega 2560 glue.
//
// Logic lives in write_core.h (also compiled on the host against a write-capable
// mock KOMBI). This file provides the MCP2515 hardware layer for CAN1 (CS D10);
// CAN2 (CS D9) is held in CONFIG and physically disconnected.
//
// Unlike the read client, the write path uses NORMAL mode WITHOUT one-shot: a
// multi-frame write must deliver every frame, so normal CAN retransmission is
// allowed, still bounded by the core's per-phase and total timeouts and by the
// one write per ARM/CONFIRM. The transmit path exists only when
// OPENMOTO_WRITE_ALLOW_TX=1 (env service_write); the dryrun env has none.
//
// DO NOT CONNECT TO A MOTORCYCLE until docs/diag-kombi-service-reset.md
// prerequisites are met. Writing mutates cluster NVM.
#include <Arduino.h>
#include <SPI.h>
#include <mcp2515.h>
#include "write_core.h"

#ifndef OPENMOTO_SPI_HZ
#define OPENMOTO_SPI_HZ 1000000
#endif

namespace {
constexpr uint8_t CS1 = 10, CS2 = 9, INT1_PIN = 2, INT2_PIN = 3;
constexpr uint32_t SPI_HZ = OPENMOTO_SPI_HZ, BAUD = 115200;
constexpr uint8_t R_CANSTAT = 0x0E, R_CANCTRL = 0x0F;
constexpr uint8_t R_CNF3 = 0x28, R_CNF2 = 0x29, R_CNF1 = 0x2A;
constexpr uint8_t R_RXF0 = 0x00, R_RXM0 = 0x20, R_RXM1 = 0x24;
constexpr uint8_t R_RXF1 = 0x04, R_RXF2 = 0x08, R_RXF3 = 0x10, R_RXF4 = 0x14, R_RXF5 = 0x18;
constexpr uint8_t R_TXB0CTRL = 0x30, R_TXB1CTRL = 0x40, R_TXB2CTRL = 0x50;
constexpr uint8_t R_RXB0CTRL = 0x60, R_RXB1CTRL = 0x70;
constexpr uint8_t I_WRITE = 0x02, I_READ = 0x03, I_BITMOD = 0x05, I_READ_STATUS = 0xA0;
constexpr uint8_t I_READ_RX0 = 0x90, I_READ_RX1 = 0x94;
constexpr uint8_t MODE_MASK = 0xE0, MODE_NORMAL = 0x00, MODE_LISTEN = 0x60, MODE_CONFIG = 0x80;
constexpr uint8_t TXREQ = 0x08, TXERR = 0x10, MLOA = 0x20, ABTF = 0x40;
constexpr uint8_t CANCTRL_ABAT = 0x10;
constexpr uint8_t CNF1_500K = 0x00, CNF2_500K = 0x90, CNF3_500K = 0x82;

MCP2515 can1(CS1, SPI_HZ, &SPI), can2(CS2, SPI_HZ, &SPI);

inline void csWrite(uint8_t cs, bool high){ volatile uint8_t*p=cs==CS1?&PORTB:&PORTH; const uint8_t m=cs==CS1?_BV(PB4):_BV(PH6);
    const uint8_t s=SREG; cli(); if(high)*p|=m; else *p&=uint8_t(~m); SREG=s; }
bool csMappingOk(){ return portOutputRegister(digitalPinToPort(CS1))==&PORTB && digitalPinToBitMask(CS1)==_BV(PB4) &&
    portOutputRegister(digitalPinToPort(CS2))==&PORTH && digitalPinToBitMask(CS2)==_BV(PH6); }
inline void csLow(uint8_t cs){ SPI.beginTransaction(SPISettings(SPI_HZ,MSBFIRST,SPI_MODE0)); csWrite(cs,false); }
inline void csHigh(uint8_t cs){ csWrite(cs,true); SPI.endTransaction(); }
uint8_t readReg(uint8_t cs,uint8_t a){ csLow(cs); SPI.transfer(I_READ); SPI.transfer(a); uint8_t v=SPI.transfer(0); csHigh(cs); return v; }
void writeReg(uint8_t cs,uint8_t a,uint8_t v){ csLow(cs); SPI.transfer(I_WRITE); SPI.transfer(a); SPI.transfer(v); csHigh(cs); }
void bitModify(uint8_t cs,uint8_t a,uint8_t m,uint8_t v){ csLow(cs); SPI.transfer(I_BITMOD); SPI.transfer(a); SPI.transfer(m); SPI.transfer(v); csHigh(cs); }
uint8_t readStatus(uint8_t cs){ csLow(cs); SPI.transfer(I_READ_STATUS); uint8_t v=SPI.transfer(0); csHigh(cs); return v; }
uint8_t modeOf(uint8_t cs){ return readReg(cs,R_CANSTAT)&MODE_MASK; }

bool requestMode(uint8_t mode){ bitModify(CS1,R_CANCTRL,MODE_MASK,mode); uint32_t t0=millis();
    while(millis()-t0<50) if(modeOf(CS1)==mode) return true; return false; }
void writeIdReg(uint8_t base,uint16_t id){ writeReg(CS1,base,uint8_t(id>>3)); writeReg(CS1,base+1,uint8_t((id&7)<<5)); writeReg(CS1,base+2,0); writeReg(CS1,base+3,0); }

#if OPENMOTO_WRITE_ALLOW_TX
int8_t rrBuf = -1;  // buffer holding the most recently queued frame
__attribute__((noinline)) bool wrEnterNormal(){
    if(!requestMode(MODE_CONFIG)) return false;
    // NOT one-shot: allow normal retransmission so multi-frame delivery is reliable.
    bitModify(CS1,R_CANCTRL,CANCTRL_ABAT,0);
    rrBuf=-1;
    return requestMode(MODE_NORMAL) && modeOf(CS1)==MODE_NORMAL;
}
__attribute__((noinline)) bool wrTransmit(const diagwrite::Frame &f){
    if(f.id==0||f.id>0x7FF||f.dlc>8) return false;
    if(modeOf(CS1)!=MODE_NORMAL) return false;
    int8_t buf=-1;
    if(!(readReg(CS1,R_TXB0CTRL)&TXREQ)) buf=0;
    else if(!(readReg(CS1,R_TXB1CTRL)&TXREQ)) buf=1;
    else if(!(readReg(CS1,R_TXB2CTRL)&TXREQ)) buf=2;
    if(buf<0) return false;
    uint8_t b[13]; b[0]=uint8_t(f.id>>3); b[1]=uint8_t((f.id&7)<<5); b[2]=0; b[3]=0; b[4]=f.dlc;
    for(uint8_t i=0;i<f.dlc;++i) b[5+i]=f.data[i];
    const uint8_t loadInstr = buf==0?0x40:buf==1?0x42:0x44;
    const uint8_t rtsInstr  = buf==0?0x81:buf==1?0x82:0x84;
    csLow(CS1); SPI.transfer(loadInstr); for(uint8_t i=0;i<5+f.dlc;++i) SPI.transfer(b[i]); csHigh(CS1);
    csLow(CS1); SPI.transfer(rtsInstr); csHigh(CS1);
    rrBuf=buf; return true;
}
diagwrite::TxState wrTxState(){
    if(rrBuf<0) return diagwrite::TXS_NONE;
    const uint8_t reg = rrBuf==0?R_TXB0CTRL:rrBuf==1?R_TXB1CTRL:R_TXB2CTRL;
    uint8_t c=readReg(CS1,reg);
    if(c&TXREQ) return diagwrite::TXS_PENDING;
    if(!(c&(ABTF|MLOA|TXERR))) c=readReg(CS1,reg);
    return (c&(ABTF|MLOA|TXERR))?diagwrite::TXS_FAILED:diagwrite::TXS_SENT;
}
#endif

class WriteHal : public diagwrite::Hal {
public:
    uint32_t nowMs() override { return millis(); }
    int readChar() override { return Serial.read(); }
    void putChar(char c) override { Serial.write(uint8_t(c)); }
    bool canSilence() override {
        bitModify(CS1,R_CANCTRL,CANCTRL_ABAT,CANCTRL_ABAT);
        bool ok=requestMode(MODE_CONFIG);
        bitModify(CS1,R_CANCTRL,CANCTRL_ABAT,0);
        ok=ok && !(readReg(CS1,R_TXB0CTRL)&TXREQ) && !(readReg(CS1,R_TXB1CTRL)&TXREQ) && !(readReg(CS1,R_TXB2CTRL)&TXREQ);
        ok=ok && requestMode(MODE_LISTEN);
        return ok && modeOf(CS1)==MODE_LISTEN && modeOf(CS2)==MODE_CONFIG;
    }
    bool canSetRxId(uint16_t id) override {
        if(!requestMode(MODE_CONFIG)){ requestMode(MODE_LISTEN); return false; }
        writeIdReg(R_RXM0,0x7FF); writeIdReg(R_RXM1,0x7FF);
        const uint8_t fr[6]={R_RXF0,R_RXF1,R_RXF2,R_RXF3,R_RXF4,R_RXF5};
        for(uint8_t i=0;i<6;++i) writeIdReg(fr[i],id);
        writeReg(CS1,R_RXB0CTRL,0x04); writeReg(CS1,R_RXB1CTRL,0x00);
        return requestMode(MODE_LISTEN);
    }
#if OPENMOTO_WRITE_ALLOW_TX
    bool canEnterActive() override { return wrEnterNormal(); }
    bool canTransmit(const diagwrite::Frame&f) override { return wrTransmit(f); }
    diagwrite::TxState canTxState() override { return wrTxState(); }
#else
    bool canEnterActive() override { return false; }
    bool canTransmit(const diagwrite::Frame&) override { return false; }
    diagwrite::TxState canTxState() override { return diagwrite::TXS_NONE; }
#endif
    bool canReceive(diagwrite::Frame&f) override {
        for(uint8_t t=0;t<4;++t){ uint8_t st=readStatus(CS1); uint8_t instr=(st&0x01)?I_READ_RX0:(st&0x02)?I_READ_RX1:0;
            if(!instr) return false; uint8_t b[13]; csLow(CS1); SPI.transfer(instr);
            for(uint8_t i=0;i<13;++i) b[i]=SPI.transfer(0); csHigh(CS1);
            bool ext=b[1]&0x08, rtr=b[1]&0x10; if(ext||rtr||(b[4]&0x0F)>8) continue;
            f.id=uint16_t(b[0])<<3|b[1]>>5; f.dlc=b[4]&0x0F;
            for(uint8_t i=0;i<8;++i) f.data[i]=i<f.dlc?b[5+i]:0; return true; }
        return false;
    }
};

WriteHal hal;
diagwrite::WriteShell shell(hal);

bool bitrateOk(){ return can1.setBitrate(CAN_500KBPS,MCP_8MHZ)==MCP2515::ERROR_OK && readReg(CS1,R_CNF1)==CNF1_500K &&
    readReg(CS1,R_CNF2)==CNF2_500K && readReg(CS1,R_CNF3)==CNF3_500K; }
bool initialize(){
    if(!csMappingOk()){ Serial.println(F("CS port mapping: FAIL")); return false; }
    bool c1=can1.reset()==MCP2515::ERROR_OK && can1.setConfigMode()==MCP2515::ERROR_OK && modeOf(CS1)==MODE_CONFIG;
    bool c2=can2.reset()==MCP2515::ERROR_OK && can2.setConfigMode()==MCP2515::ERROR_OK && modeOf(CS2)==MODE_CONFIG;
    Serial.println(c1?F("MCP2515 #1 CONFIG: PASS"):F("MCP2515 #1 CONFIG: FAIL"));
    Serial.println(c2?F("MCP2515 #2 CONFIG: PASS"):F("MCP2515 #2 CONFIG: FAIL"));
    if(!c1||!c2) return false;
    if(!bitrateOk()){ Serial.println(F("MCP2515 #1 bitrate 500k/8MHz: FAIL")); return false; }
    Serial.println(F("MCP2515 #1 bitrate 500k/8MHz: PASS"));
    return modeOf(CS2)==MODE_CONFIG;
}
}  // namespace

void setup(){
    digitalWrite(53,HIGH); pinMode(53,OUTPUT);
    digitalWrite(CS1,HIGH); digitalWrite(CS2,HIGH); pinMode(CS1,OUTPUT); pinMode(CS2,OUTPUT);
    pinMode(INT1_PIN,INPUT_PULLUP); pinMode(INT2_PIN,INPUT_PULLUP);
    Serial.begin(BAUD); SPI.begin(); delay(500);
    if(!initialize()){ can1.setConfigMode(); can2.setConfigMode(); Serial.println(F("INIT: FAIL; controllers set to CONFIG")); while(true) delay(1000); }
    if(!hal.canSetRxId(0x7FF)){ can1.setConfigMode(); can2.setConfigMode(); Serial.println(F("INIT: FAIL rx filter")); while(true) delay(1000); }
    shell.begin();
}
void loop(){ shell.poll(); }
