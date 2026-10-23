// OpenMoto Phase 3B binary capture protocol, version 1.
// Pure C++ (no Arduino dependencies) so the identical encoder is compiled for
// the AVR firmware and for the host-side golden-vector tests.
// Specification: docs/phase3b.md ("Binary protocol v1").
//
// Record layout (multi-byte fields little-endian unless noted):
//   0  0xA5 sync0
//   1  0x5A sync1
//   2  version (1)
//   3  type
//   4  seq low   } record sequence number, uint16, +1 per record (any type),
//   5  seq high  } wraps 65535 -> 0
//   6  len       body length in bytes (0..MAX_BODY)
//   7  body[len]
//   7+len  CRC high byte  } CRC-16/CCITT-FALSE (poly 0x1021, init 0xFFFF,
//   8+len  CRC low byte   } no reflection, no xor-out) over bytes 2..6+len
#pragma once
#include <stdint.h>
#include <string.h>

namespace omproto {

constexpr uint8_t SYNC0 = 0xA5, SYNC1 = 0x5A, VERSION = 1;
constexpr uint8_t HEADER_LEN = 7, CRC_LEN = 2, MAX_BODY = 64;
constexpr uint8_t MAX_RECORD = HEADER_LEN + MAX_BODY + CRC_LEN;  // 73

enum Type : uint8_t {
    T_HELLO = 0x00,  // stream/configuration header
    T_FRAME = 0x01,  // one received CAN frame
    T_DROP = 0x02,   // firmware queue drops since the previous record
    T_STATS = 0x03,  // periodic counters (heartbeat)
    T_TEXT = 0x04,   // status/command text (ASCII), possibly continued
};

// FRAME id_flags field
constexpr uint32_t ID_EXT = 0x80000000UL, ID_RTR = 0x40000000UL, ID_RESERVED = 0x20000000UL;
constexpr uint32_t STD_MASK = 0x7FFUL, EXT_MASK = 0x1FFFFFFFUL;
constexpr uint8_t TEXT_CONTINUED = 0x01;  // TEXT flags bit0: line continues in next TEXT record

// CRC-16/CCITT-FALSE byte update. On AVR the hand-optimized avr-libc
// _crc_xmodem_update (poly 0x1021, MSB-first; identical algorithm, the init
// value is supplied by the caller) is used: the portable C version below was
// measured to cost enough CPU to starve CAN1 reception. Host builds use the
// portable version (golden vectors); the host decoder verifies the CRC of
// every record captured from hardware.
#if defined(__AVR__)
}  // namespace omproto
#include <util/crc16.h>
namespace omproto {
inline uint16_t crcUpdate(uint16_t crc, uint8_t data) { return _crc_xmodem_update(crc, data); }
#else
inline uint16_t crcUpdate(uint16_t crc, uint8_t data) {
    crc = uint16_t((crc >> 8) | (crc << 8));
    crc ^= data;
    crc ^= uint16_t((crc & 0xFF) >> 4);
    crc ^= uint16_t(crc << 12);
    crc ^= uint16_t((crc & 0xFF) << 5);
    return crc;
}
#endif
inline uint16_t crc16(const uint8_t *p, uint16_t n, uint16_t crc = 0xFFFF) {
    while (n--) crc = crcUpdate(crc, *p++);
    return crc;
}

struct Builder {
    uint8_t *buf;
    uint8_t n;
    Builder(uint8_t *b, uint8_t type, uint16_t seq) : buf(b), n(HEADER_LEN) {
        buf[0] = SYNC0; buf[1] = SYNC1; buf[2] = VERSION; buf[3] = type;
        buf[4] = uint8_t(seq); buf[5] = uint8_t(seq >> 8); buf[6] = 0;
    }
    void u8(uint8_t v) { buf[n++] = v; }
    void u16(uint16_t v) { u8(uint8_t(v)); u8(uint8_t(v >> 8)); }
    void u32(uint32_t v) { u16(uint16_t(v)); u16(uint16_t(v >> 16)); }
    void bytes(const uint8_t *p, uint8_t len) { memcpy(buf + n, p, len); n = uint8_t(n + len); }
    // Fill len only; returns length without the 2 CRC bytes (the caller
    // computes the CRC while transmitting; see the firmware's emitRecord()).
    uint8_t finishNoCrc() {
        buf[6] = uint8_t(n - HEADER_LEN);
        return n;
    }
    // Fill len and CRC; returns total record length.
    uint8_t finish() {
        buf[6] = uint8_t(n - HEADER_LEN);
        const uint16_t crc = crc16(buf + 2, uint16_t(n - 2));
        buf[n++] = uint8_t(crc >> 8);
        buf[n++] = uint8_t(crc);
        return n;
    }
};

inline uint8_t payloadLen(uint8_t dlc, bool rtr) { return rtr ? 0 : (dlc > 8 ? 8 : dlc); }

// FRAME body: ts_us u32 | id_flags u32 | dlc u8 | data[payloadLen]
// withCrc=false returns the record without its CRC (len field set); the CRC
// over bytes 2..n-1 must then be appended by the caller (identical result).
inline uint8_t encodeFrame(uint8_t *buf, uint16_t seq, uint32_t tsUs, uint32_t id, bool ext,
                           bool rtr, uint8_t dlc, const uint8_t *data, bool withCrc = true) {
    Builder b(buf, T_FRAME, seq);
    b.u32(tsUs);
    b.u32((id & (ext ? EXT_MASK : STD_MASK)) | (ext ? ID_EXT : 0) | (rtr ? ID_RTR : 0));
    b.u8(dlc & 0x0F);
    b.bytes(data, payloadLen(dlc & 0x0F, rtr));
    return withCrc ? b.finish() : b.finishNoCrc();
}
// DROP body: dropped u32 (frames lost because the firmware output queue was
// full, immediately after the FRAME record that precedes this DROP record) |
// total u32 (cumulative queue drops counted when this record was emitted)
inline uint8_t encodeDrop(uint8_t *buf, uint16_t seq, uint32_t dropped, uint32_t total,
                          bool withCrc = true) {
    Builder b(buf, T_DROP, seq);
    b.u32(dropped); b.u32(total);
    return withCrc ? b.finish() : b.finishNoCrc();
}
// TEXT body: flags u8 | ascii[0..MAX_BODY-1]
inline uint8_t encodeText(uint8_t *buf, uint16_t seq, const char *text, uint8_t len, bool continued) {
    if (len > MAX_BODY - 1) len = MAX_BODY - 1;
    Builder b(buf, T_TEXT, seq);
    b.u8(continued ? TEXT_CONTINUED : 0);
    b.bytes(reinterpret_cast<const uint8_t *>(text), len);
    return b.finish();
}

// STATS body (all little-endian):
struct Stats {
    uint32_t tsUs, ms, rx, hwOverrun, queueDrop, merr, errif, intMissed, modeFaults, spiErrors,
        records;
    uint8_t queuePeak, eflgOr, tecPeak, recPeak;
    uint16_t minFreeRam;
};
constexpr uint8_t STATS_BODY_LEN = 11 * 4 + 4 + 2;  // 50
inline uint8_t encodeStats(uint8_t *buf, uint16_t seq, const Stats &s, bool withCrc = true) {
    Builder b(buf, T_STATS, seq);
    b.u32(s.tsUs); b.u32(s.ms); b.u32(s.rx); b.u32(s.hwOverrun); b.u32(s.queueDrop);
    b.u32(s.merr); b.u32(s.errif); b.u32(s.intMissed); b.u32(s.modeFaults); b.u32(s.spiErrors);
    b.u32(s.records);
    b.u8(s.queuePeak); b.u8(s.eflgOr); b.u8(s.tecPeak); b.u8(s.recPeak);
    b.u16(s.minFreeRam);
    return withCrc ? b.finish() : b.finishNoCrc();
}
// HELLO body: proto u8 | variant u8 (0 sniffer, 1 bench) | spi_hz u32 | baud u32 |
//             queue_size u8 | ts_us u32
constexpr uint8_t HELLO_BODY_LEN = 15;
inline uint8_t encodeHello(uint8_t *buf, uint16_t seq, uint8_t variant, uint32_t spiHz,
                           uint32_t baud, uint8_t queueSize, uint32_t tsUs) {
    Builder b(buf, T_HELLO, seq);
    b.u8(VERSION); b.u8(variant); b.u32(spiHz); b.u32(baud); b.u8(queueSize); b.u32(tsUs);
    return b.finish();
}

}  // namespace omproto
