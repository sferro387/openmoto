#pragma once
#include "SPI.h"
enum CAN_CLOCK { MCP_8MHZ };
enum CAN_SPEED { CAN_500KBPS };
class MCP2515 {
 int i;
public:
 enum ERROR { ERROR_OK=0, ERROR_FAIL=1, ERROR_NOMSG=5 };
 enum TXBn { TXB0 };
 MCP2515(uint8_t cs,uint32_t,SPIClass*):i(cs==10?0:1) {}
 ERROR reset() { memset(mock::regs[i],0,128); mock::regs[i][0x0E]=0x80; return ERROR_OK; }
 ERROR setConfigMode() {
    if(mock::fault=="stop_fail" && mock::wasNormal[i]) return ERROR_FAIL;
    mock::regs[i][0x0E]=0x80; return ERROR_OK;
 }
 ERROR setBitrate(CAN_SPEED,CAN_CLOCK) {
    mock::regs[i][0x2A]=0; mock::regs[i][0x29]=0x90; mock::regs[i][0x28]=0x82;
    if(mock::fault=="wrong_bitrate") mock::regs[i][0x2A]=1;
    return ERROR_OK;
 }
 ERROR setNormalMode() {
    mock::wasNormal[i]=true; mock::regs[i][0x0E]=mock::fault=="normal_fail"?0x40:0;
    return ERROR_OK;
 }
 uint8_t getErrorFlags() { return mock::regs[i][0x2D]; }
 uint8_t getInterrupts() { return mock::regs[i][0x2C]; }
 uint8_t errorCountTX() { return mock::regs[i][0x1C]; }
 uint8_t errorCountRX() { return mock::regs[i][0x1D]; }
 void clearTXInterrupts() { mock::regs[i][0x2C]&=~0x04; }
 bool checkReceive() { return !mock::inbox[i].empty(); }
 ERROR sendMessage(TXBn,const can_frame *f) {
    if(mock::regs[i][0x0E]!=0 || mock::regs[1-i][0x0E]!=0)
        throw std::runtime_error("Transmit outside NORMAL mode");
    mock::regs[i][0x30]=0; mock::regs[i][0x2C]|=0x04;
    can_frame frame=*f;
    bool trigger=frame.data[2]==1 && frame.data[4]==7;
    if(trigger) {
        const auto &fault=mock::fault;
        if(fault=="tx_api") return ERROR_FAIL;
        if(fault=="missing") return ERROR_OK;
        if(fault=="duplicate") mock::inbox[1-i].push_back(frame);
        if(fault=="wrong_id") frame.can_id=0x777;
        if(fault=="extended") frame.can_id|=0x80000000;
        if(fault=="rtr") frame.can_id|=0x40000000;
        if(fault=="dlc") frame.can_dlc=7;
        if(fault=="payload") frame.data[0]^=1;
        if(fault=="sequence") frame.data[4]=8;
        if(fault=="self_rx") mock::inbox[i].push_back(frame);
        if(fault=="eflg") mock::regs[i][0x2D]=0x40;
        if(fault=="tec") mock::regs[i][0x1C]=1;
        if(fault=="rec") mock::regs[1-i][0x1D]=1;
        if(fault=="merrf") mock::regs[i][0x2C]|=0x80;
        if(fault=="tx_pending") { mock::regs[i][0x30]=8; mock::regs[i][0x2C]&=~0x04; }
    }
    mock::inbox[1-i].push_back(frame);
    return ERROR_OK;
 }
 ERROR readMessage(can_frame *f) {
    if(mock::inbox[i].empty()) return ERROR_NOMSG;
    *f=mock::inbox[i].front(); mock::inbox[i].pop_front(); return ERROR_OK;
 }
};
