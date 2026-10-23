#pragma once
#include "Arduino.h"
constexpr int MSBFIRST=1, SPI_MODE0=0;
struct SPISettings { SPISettings(uint32_t,int,int) {} };
struct SPIClass {
 void begin() {}
 void beginTransaction(SPISettings) {}
 void endTransaction() {}
 uint8_t transfer(uint8_t value) {
    using namespace mock;
    if(selected<0) throw std::runtime_error("SPI without chip selection");
    if(absent(selected)) return 0xFF;
    if(step++==0) { instruction=value; return 0; }
    if(step==2) { address=value; return 0; }
    if(instruction==0x03) return regs[selected][address++];
    if(instruction==0x02) {
        regs[selected][address]=value;
        if(fault=="shared_cs") regs[1-selected][address]=value;
        ++address;
    }
    if(instruction==0x05) {
        if(step==3) mask=value;
        else {
            regs[selected][address]=(regs[selected][address]&~mask)|(value&mask);
            if(address==0x0F && (value&0x10)) regs[selected][0x30]&=~0x08;
        }
    }
    return 0;
 }
};
inline SPIClass SPI;
