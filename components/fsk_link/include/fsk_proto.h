#pragma once
// Wire protocol of the fsk-walkie-talkie net. This component is consumed by
// tincan-autopatch via EXTRA_COMPONENT_DIRS, so there is exactly one copy.
// (The legacy Arduino build in src/main.cpp repeats these numbers by hand.)
#include <stdint.h>

// Radio (SX1262 GFSK)
#define FSK_FREQ_MHZ     914.6f   // 33 cm ham band; see README on licensing
#define FSK_BITRATE_KBPS 19.2f
#define FSK_FREQ_DEV_KHZ 10.0f
#define FSK_RX_BW_KHZ    39.0f
#define FSK_PWR_DBM      22
#define FSK_PREAMBLE_BITS 16
#define FSK_SYNC_WORD    { 0x2D, 0xD4 }

// Codec2 mode 3200 geometry
#define C2_BYTES_PER_FRAME   8
#define C2_SAMPLES_PER_FRAME 160
#define FRAMES_PER_PACKET    5

// Audio packet: magic, node id lo, node id hi, sequence, then 5 codec frames
#define PKT_MAGIC_AUDIO      0xC2
#define PKT_HEADER_SIZE      4
#define PKT_PAYLOAD_SIZE     (FRAMES_PER_PACKET * C2_BYTES_PER_FRAME)   // 40
#define PKT_AUDIO_SIZE       (PKT_HEADER_SIZE + PKT_PAYLOAD_SIZE)       // 44

// Control packet: magic, node id lo, node id hi, type, arg  (5 bytes)
#define PKT_MAGIC_CTRL       0xC3
#define PKT_CTRL_SIZE        5
enum fsk_ctrl_type : uint8_t {
    CTRL_CALL       = 1,   // walkie -> gateway: dial the preset extension
    CTRL_HANGUP     = 2,   // walkie -> gateway: end the call
    CTRL_CALL_STATE = 3,   // gateway -> walkie: arg 1 = in call, 0 = idle
    CTRL_BEEP       = 4,   // gateway -> walkie: roger beep, phone side unkeyed
};
#define PKT_CTRL_REPEAT      3   // control packets are sent this many times
