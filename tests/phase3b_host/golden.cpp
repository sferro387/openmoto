// Host-only: emit golden records from the firmware's protocol.h encoder.
// Output: one line per record, "<name> <hex>". Used by tests/test_phase3b_protocol.py.
#include "../../firmware/mega-can/src/phase3b/protocol.h"
#include <cstdio>

using namespace omproto;

static void emit(const char *name, const uint8_t *rec, uint8_t n) {
    std::printf("%s ", name);
    for (uint8_t i = 0; i < n; ++i) std::printf("%02X", rec[i]);
    std::printf("\n");
}

int main() {
    uint8_t rec[MAX_RECORD];
    const uint8_t d8[8] = {0x4F, 0x4D, 0x5B, 0xA5, 0x5A, 0x00, 0xFF, 0x5A};  // contains A5 5A
    uint8_t n;
    // crc check value
    const uint8_t check[] = {'1', '2', '3', '4', '5', '6', '7', '8', '9'};
    std::printf("crc_check %04X\n", crc16(check, 9));
    n = encodeFrame(rec, 0, 0x01020304UL, 0x123, false, false, 8, d8); emit("std8", rec, n);
    n = encodeFrame(rec, 1, 0xFFFFFFFFUL, 0x1FFFFFFF, true, false, 3, d8); emit("ext3", rec, n);
    n = encodeFrame(rec, 2, 0, 0x321, false, true, 4, d8); emit("std_rtr4", rec, n);
    n = encodeFrame(rec, 3, 12345, 0x0ABCDEF, true, true, 0, d8); emit("ext_rtr0", rec, n);
    n = encodeFrame(rec, 4, 7, 0x7FF, false, false, 0, d8); emit("std0", rec, n);
    n = encodeFrame(rec, 5, 8, 0x000, false, false, 15, d8); emit("dlc15", rec, n);
    n = encodeFrame(rec, 65535, 9, 0x456, false, false, 8, d8); emit("seq_max", rec, n);
    n = encodeDrop(rec, 6, 17, 1000017); emit("drop", rec, n);
    Stats s = {0x11111111, 0x22222222, 3, 4, 5, 6, 7, 8, 9, 10, 11, 12, 0x15, 0, 0, 5432};
    n = encodeStats(rec, 7, s); emit("stats", rec, n);
    n = encodeText(rec, 8, "# STREAM start dlc=8 ms=5000", 28, false); emit("text", rec, n);
    n = encodeText(rec, 9, "part one ", 9, true); emit("text_cont", rec, n);
    n = encodeHello(rec, 10, 1, 4000000UL, 921600UL, 64, 42); emit("hello", rec, n);
    // Streaming CRC path used by the firmware: record without CRC + byte-wise CRC.
    n = encodeFrame(rec, 0, 0x01020304UL, 0x123, false, false, 8, d8, false);
    uint16_t crc = 0xFFFF;
    for (uint8_t i = 2; i < n; ++i) crc = crcUpdate(crc, rec[i]);
    rec[n] = uint8_t(crc >> 8); rec[n + 1] = uint8_t(crc);
    emit("std8_streaming", rec, uint8_t(n + 2));
    n = encodeStats(rec, 7, s, false);
    crc = 0xFFFF;
    for (uint8_t i = 2; i < n; ++i) crc = crcUpdate(crc, rec[i]);
    rec[n] = uint8_t(crc >> 8); rec[n + 1] = uint8_t(crc);
    emit("stats_streaming", rec, uint8_t(n + 2));
    return 0;
}
