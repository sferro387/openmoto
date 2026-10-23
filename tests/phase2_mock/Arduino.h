#pragma once
// Host-only fault injection. This does NOT model electrical CAN or certify hardware.
#include <cstdint>
#include <cstring>
#include <deque>
#include <sstream>
#include <string>
#include <stdexcept>
using std::uint8_t; using std::uint16_t; using std::uint32_t;
#define F(x) x
using __FlashStringHelper = char;
constexpr int HIGH=1, LOW=0, OUTPUT=1, INPUT_PULLUP=2, HEX=16;
struct can_frame { uint32_t can_id; uint8_t can_dlc; uint8_t data[8]; };
struct Halt {};
namespace mock {
inline std::string fault;
inline uint32_t now=0;
inline uint8_t regs[2][128] = {};
inline std::deque<can_frame> inbox[2];
inline bool wasNormal[2] = {};
inline int selected=-1, step=0;
inline uint8_t instruction=0, address=0, mask=0;
inline bool absent(int i) { return i==1 && fault=="absent2"; }
}
inline uint32_t millis() { return mock::now++; }
inline void delay(uint32_t ms) { if(ms==1000) throw Halt{}; mock::now+=ms; }
inline void pinMode(int,int) {}
inline void digitalWrite(uint8_t pin, int value) {
    if(pin!=9 && pin!=10) return;
    if(value==LOW) { mock::selected=pin==10?0:1; mock::step=0; }
    else mock::selected=-1;
}
struct SerialMock {
    std::ostringstream out;
    void begin(int) {}
    template<class T> void print(T v) { out<<v; }
    void print(uint8_t v) { out<<unsigned(v); }
    void print(uint8_t v,int) { out<<std::hex<<unsigned(v)<<std::dec; }
    void println(uint8_t v, int base) { print(v, base); out<<'\n'; }
    template<class T> void println(T v) { print(v); out<<'\n'; }
};
inline SerialMock Serial;
