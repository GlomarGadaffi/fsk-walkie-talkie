#pragma once
// Thin SX1262 GFSK link on top of RadioLib (ESP-IDF HAL). Half duplex:
// transmit() blocks for the packet's air time (~21 ms for 44 bytes) and
// returns the radio to receive. Received packets are queued by the DIO1 ISR
// path and handed out by receive(). Both are safe to call from one task
// each (a TX task and an RX task) but not re-entrantly.
#include <stdint.h>
#include <stddef.h>
#include <stdbool.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef struct {
    int spi_host;   // spi_host_device_t
    int sck, miso, mosi;
    int cs, dio1, rst, busy;
} fsk_link_pins_t;

typedef struct {
    uint8_t  data[64];
    uint8_t  len;
    int16_t  rssi_dbm;
} fsk_link_pkt_t;

// Bring the radio up in FSK with the fsk_proto.h parameters and start
// receiving. Returns RadioLib's status code (0 = ok).
int  fsk_link_init(const fsk_link_pins_t *pins);

// Blocking transmit, then back to receive. Returns RadioLib status.
int  fsk_link_transmit(const uint8_t *data, size_t len);

// Wait up to timeout_ms for a CRC-clean packet. Returns true if one was
// copied into *pkt.
bool fsk_link_receive(fsk_link_pkt_t *pkt, uint32_t timeout_ms);

// Milliseconds since the last CRC-clean packet was received (UINT32_MAX if none).
uint32_t fsk_link_ms_since_rx(void);

// 16-bit node id derived from the efuse MAC.
uint16_t fsk_link_node_id(void);

#ifdef __cplusplus
}
#endif
