/*
 * Voice-over-FSK walkie talkie
 *
 * Hardware : LilyGo T3-S3 (ESP32-S3 + SX1262) with the T3-S3-MVSRBoard back
 *            plate (MP34DT05TR / MSM261 mic, MAX98357A amp)
 * Radio    : GFSK, 19.2 kbps, +/-10 kHz deviation, 39 kHz RX bandwidth
 * Codec    : Codec2 mode 3200 -- 8 bytes per 160-sample (20 ms) frame
 * Packet   : 4-byte header + 5 codec frames (40 bytes) = 44 bytes, 100 ms of
 *            speech, ~18 ms on air (~21 ms with preamble/sync/len/CRC)
 *
 * Lineage: started from LilyGo's SX126x_Walkie_Talkie example (GPL-3.0).
 *
 * Push-to-talk is a toggle on the BOOT button: press once to talk, press
 * again to listen. Hold it for LONG_PRESS_MS to ask a tincan-autopatch
 * gateway to place (or end) a phone call.
 */

#include <Arduino.h>
#include <RadioLib.h>
#include "pin_config.h"
#include "Arduino_DriveBus_Library.h"
#include "codec2.h"
#include "heap_audit.h"

// ---------------------------------------------------------------------------
// Configuration
// ---------------------------------------------------------------------------
#define IIS_SAMPLE_RATE 8000
#define IIS_DATA_BIT    16

#define FSK_FREQ_MHZ    914.6f  // 33 cm ham band; see README on licensing
#define FSK_BITRATE     19.2f   // kbps
#define FSK_FREQ_DEV    10.0f   // kHz
#define FSK_RX_BW       39.0f   // kHz
#define FSK_PWR_DBM     22
#define FSK_PREAMBLE    16      // bits

// Codec2 3200 geometry (asserted against the library at start-up)
#define C2_BYTES_PER_FRAME   8
#define C2_SAMPLES_PER_FRAME 160
#define FRAMES_PER_PACKET    5

#define PACKET_MAGIC        0xC2
#define PACKET_HEADER_SIZE  4   // magic, node id (2), sequence
#define PACKET_PAYLOAD_SIZE (FRAMES_PER_PACKET * C2_BYTES_PER_FRAME) // 40
#define PACKET_SIZE         (PACKET_HEADER_SIZE + PACKET_PAYLOAD_SIZE) // 44

// Control packets (tincan-autopatch gateway): magic, node id (2), type, arg
#define PKT_MAGIC_CTRL      0xC3
#define PKT_CTRL_SIZE       5
#define PKT_CTRL_REPEAT     3
enum CtrlType : uint8_t {
    CTRL_CALL       = 1,   // walkie -> gateway: dial the preset extension
    CTRL_HANGUP     = 2,   // walkie -> gateway: end the call
    CTRL_CALL_STATE = 3,   // gateway -> walkie: arg 1 = in call, 0 = idle
    CTRL_BEEP       = 4,   // gateway -> walkie: roger beep, phone side unkeyed
};
#define LONG_PRESS_MS       800

// Receive-side buffering
#define RX_QUEUE_PACKETS     8   // 800 ms of speech, bounded
#define MAX_PLC_PACKETS      2   // conceal up to 200 ms of loss, then go silent
#define JITTER_PREFILL_PKTS  2   // wait for 200 ms before starting playback
#define PLC_HISTORY_MS       500 // forget the last good frame after this much silence

#define DEBOUNCE_MS 200
#define AUDIO_GAIN  5

// ---------------------------------------------------------------------------
// Ring buffer: all-or-nothing push/pop so codec frame boundaries never shift
// ---------------------------------------------------------------------------
template <typename T, size_t CAPACITY>
class RingBuffer {
public:
    RingBuffer() : _head(0), _tail(0), _count(0) { _mutex = xSemaphoreCreateMutex(); }

    // Push exactly n items or nothing. Returns true on success.
    bool push(const T* data, size_t n) {
        bool ok = false;
        xSemaphoreTake(_mutex, portMAX_DELAY);
        if (_count + n <= CAPACITY) {
            for (size_t i = 0; i < n; i++) {
                _buf[_head] = data[i];
                _head = (_head + 1) % CAPACITY;
            }
            _count += n;
            ok = true;
        }
        xSemaphoreGive(_mutex);
        return ok;
    }

    // Pop exactly n items or nothing. Returns true on success.
    bool pop(T* dest, size_t n) {
        bool ok = false;
        xSemaphoreTake(_mutex, portMAX_DELAY);
        if (_count >= n) {
            for (size_t i = 0; i < n; i++) {
                dest[i] = _buf[_tail];
                _tail = (_tail + 1) % CAPACITY;
            }
            _count -= n;
            ok = true;
        }
        xSemaphoreGive(_mutex);
        return ok;
    }

    size_t available() {
        xSemaphoreTake(_mutex, portMAX_DELAY);
        size_t c = _count;
        xSemaphoreGive(_mutex);
        return c;
    }

    void clear() {
        xSemaphoreTake(_mutex, portMAX_DELAY);
        _head = _tail = _count = 0;
        xSemaphoreGive(_mutex);
    }

private:
    T _buf[CAPACITY];
    size_t _head, _tail, _count;
    SemaphoreHandle_t _mutex;
};

// A received (or missing) packet's worth of codec bytes
struct RxPacket {
    uint8_t payload[PACKET_PAYLOAD_SIZE];
    bool    lost;   // true = gap detected by sequence number; run PLC
};

// ---------------------------------------------------------------------------
// Buffers (capacities are exact multiples of the unit pushed into them)
// ---------------------------------------------------------------------------
static RingBuffer<uint8_t, PACKET_PAYLOAD_SIZE * 6> Radio_Tx_Ring;          // 600 ms
static RingBuffer<RxPacket, RX_QUEUE_PACKETS>       Radio_Rx_Queue;
static RingBuffer<int16_t, C2_SAMPLES_PER_FRAME * 2 * FRAMES_PER_PACKET * 4> Speaker_Ring; // 400 ms stereo

// ---------------------------------------------------------------------------
// Globals
// ---------------------------------------------------------------------------
static uint16_t Local_Node_Id = 0;
static volatile bool Radio_Operation_Flag = false;
static volatile bool Boot_Key_Flag = false;
static volatile bool Transmission_Mode = false;  // false = listen, true = talk

static uint8_t Send_Package[PACKET_SIZE];
static uint8_t Receive_Package[PACKET_SIZE];
static uint8_t Tx_Sequence = 0;

// RX sequence tracking (main loop only)
static bool     Rx_Have_Seq = false;
static uint8_t  Rx_Last_Seq = 0;
static uint16_t Rx_Last_From = 0;
static uint32_t Rx_Packets = 0, Rx_Lost = 0, Rx_Dropped = 0;

static unsigned long last_button_ms = 0;
static unsigned long press_start_ms = 0;
static bool     button_down = false;
static bool     long_press_done = false;
static bool     In_Call = false;            // last CTRL_CALL_STATE from a gateway
static unsigned long last_ctrl_ms = 0;
static uint8_t  last_ctrl_type = 0;
static uint8_t  last_ctrl_arg = 0;
static volatile bool Speaker_Play_Now = false;  // beep: skip the jitter prefill

// ---------------------------------------------------------------------------
// Hardware objects
// ---------------------------------------------------------------------------
#if defined(T3_S3_MVSRBoard_V1_0)
std::shared_ptr<Arduino_IIS_DriveBus> IIS_Bus_In =
    std::make_shared<Arduino_HWIIS>(I2S_NUM_0, MSM261_BCLK, MSM261_WS, MSM261_DATA);
#else
std::shared_ptr<Arduino_IIS_DriveBus> IIS_Bus_In =
    std::make_shared<Arduino_HWIIS>(I2S_NUM_0, -1, MP34DT05TR_LRCLK, MP34DT05TR_DATA);
#endif
std::unique_ptr<Arduino_IIS> IIS_Mic(new Arduino_MEMS(IIS_Bus_In));

std::shared_ptr<Arduino_IIS_DriveBus> IIS_Bus_Out =
    std::make_shared<Arduino_HWIIS>(I2S_NUM_1, MAX98357A_BCLK, MAX98357A_LRCLK, MAX98357A_DATA);
std::unique_ptr<Arduino_IIS> IIS_Speaker(new Arduino_Amplifier(IIS_Bus_Out));

SX1262 radio = new Module(LORA_CS, LORA_DIO1, LORA_RST, LORA_BUSY, SPI);

// ---------------------------------------------------------------------------
// Packet header
// ---------------------------------------------------------------------------
static void prepare_header() {
    Send_Package[0] = PACKET_MAGIC;
    Send_Package[1] = (uint8_t)(Local_Node_Id & 0xFF);
    Send_Package[2] = (uint8_t)(Local_Node_Id >> 8);
    Send_Package[3] = Tx_Sequence++;
}

// ---------------------------------------------------------------------------
// ISRs
// ---------------------------------------------------------------------------
static void IRAM_ATTR Radio_Interrupt(void) { Radio_Operation_Flag = true; }
static void IRAM_ATTR Button_Interrupt(void) { Boot_Key_Flag = true; }

// ---------------------------------------------------------------------------
// Task: Codec2 engine (core 1)
// ---------------------------------------------------------------------------
static void Codec2_Task(void*) {
    struct CODEC2* c2 = codec2_create(CODEC2_MODE_3200);
    if (!c2) {
        Serial.println("Codec2 init failed");
        vTaskDelete(NULL);
        return;
    }
    codec2_set_lpc_post_filter(c2, 1, 0, 0.8f, 0.2f);

    const int n_samp  = codec2_samples_per_frame(c2);
    const int n_bytes = codec2_bytes_per_frame(c2);
    if (n_samp != C2_SAMPLES_PER_FRAME || n_bytes != C2_BYTES_PER_FRAME) {
        Serial.printf("Codec2 geometry mismatch: %d samples, %d bytes\n", n_samp, n_bytes);
        vTaskDelete(NULL);
        return;
    }

    static int16_t sample_buf[C2_SAMPLES_PER_FRAME];
    static int16_t decode_buf[C2_SAMPLES_PER_FRAME];
    static int16_t stereo_buf[C2_SAMPLES_PER_FRAME * 2];
    static uint8_t code_buf[C2_BYTES_PER_FRAME];
    static uint8_t last_good_frame[C2_BYTES_PER_FRAME];
    static bool    have_last_good = false;
    static unsigned long last_good_ms = 0;
    static RxPacket pkt;

    Serial.printf("Audio engine: %d samples/frame, %d bytes/frame, %d frames/packet (%d ms)\n",
                  n_samp, n_bytes, FRAMES_PER_PACKET, FRAMES_PER_PACKET * 20);

    while (true) {
        // --- Talk: mic -> encode -> TX ring ---
        if (Transmission_Mode) {
            if (IIS_Mic->IIS_Read_Data(sample_buf, sizeof(sample_buf))) {
                codec2_encode(c2, code_buf, sample_buf);
                Radio_Tx_Ring.push(code_buf, n_bytes);  // drops whole frame if full
            } else {
                vTaskDelay(1);
            }
            continue;  // I2S read blocks for 20 ms, no further delay needed
        }

        // --- Listen: RX queue -> decode (with PLC) -> speaker ring ---
        if (Speaker_Ring.available() + C2_SAMPLES_PER_FRAME * 2 * FRAMES_PER_PACKET
                > C2_SAMPLES_PER_FRAME * 2 * FRAMES_PER_PACKET * 4) {
            vTaskDelay(pdMS_TO_TICKS(5));   // speaker ring full, let it drain
            continue;
        }
        if (!Radio_Rx_Queue.pop(&pkt, 1)) {
            // Queue is empty ~85 ms of every 100 ms; only forget the PLC
            // history once the other side has clearly stopped talking.
            if (have_last_good && millis() - last_good_ms > PLC_HISTORY_MS) have_last_good = false;
            vTaskDelay(pdMS_TO_TICKS(5));
            continue;
        }

        for (int f = 0; f < FRAMES_PER_PACKET; f++) {
            float scale = 1.0f;
            if (!pkt.lost) {
                memcpy(code_buf, &pkt.payload[f * n_bytes], n_bytes);
                memcpy(last_good_frame, code_buf, n_bytes);
                have_last_good = true;
                last_good_ms = millis();
            } else if (have_last_good) {
                // Packet-loss concealment: repeat the last good frame, fading
                memcpy(code_buf, last_good_frame, n_bytes);
                scale = 0.6f;
            } else {
                memset(decode_buf, 0, sizeof(decode_buf));
                scale = 0.0f;
            }
            if (scale > 0.0f) codec2_decode(c2, decode_buf, code_buf);

            for (int i = 0; i < n_samp; i++) {
                int32_t v = (int32_t)(decode_buf[i] * scale) * AUDIO_GAIN;
                if (v > 32767)  v = 32767;
                if (v < -32768) v = -32768;
                stereo_buf[i * 2] = stereo_buf[i * 2 + 1] = (int16_t)v;
            }
            Speaker_Ring.push(stereo_buf, n_samp * 2);
        }
    }
}

// ---------------------------------------------------------------------------
// Task: speaker playback with jitter prefill and silence on underrun (core 1)
// ---------------------------------------------------------------------------
static void MAX_Play_Task(void*) {
    const size_t chunk = C2_SAMPLES_PER_FRAME * 2;          // one 20 ms stereo frame
    const size_t prefill = chunk * FRAMES_PER_PACKET * JITTER_PREFILL_PKTS;
    static int16_t play_buf[C2_SAMPLES_PER_FRAME * 2];
    static const int16_t silence[C2_SAMPLES_PER_FRAME * 2] = {0};
    bool playing = false;
    int  starved = 0;

    while (true) {
        size_t avail = Speaker_Ring.available();
        if (!playing) {
            if (avail >= prefill || Speaker_Play_Now) { playing = true; starved = 0; Speaker_Play_Now = false; }
            else { vTaskDelay(pdMS_TO_TICKS(5)); continue; }
        }
        if (Speaker_Ring.pop(play_buf, chunk)) {
            starved = 0;
            IIS_Speaker->IIS_Write_Data(play_buf, sizeof(play_buf));
        } else {
            // Keep the I2S DMA fed with zeros instead of replaying stale data
            IIS_Speaker->IIS_Write_Data(silence, sizeof(silence));
            if (++starved >= FRAMES_PER_PACKET * MAX_PLC_PACKETS) playing = false;
        }
    }
}

// ---------------------------------------------------------------------------
// Local tones (never go over the air) and control packets
// ---------------------------------------------------------------------------
static void queue_beep(float hz, int ms) {
    static int16_t buf[C2_SAMPLES_PER_FRAME * 2];
    static float phase = 0.0f;
    const float step = 6.2831853f * hz / IIS_SAMPLE_RATE;
    int frames = (ms + 19) / 20;
    for (int f = 0; f < frames; f++) {
        for (int i = 0; i < C2_SAMPLES_PER_FRAME; i++) {
            int16_t v = (int16_t)(sinf(phase) * 6000.0f);
            phase += step;
            if (phase > 6.2831853f) phase -= 6.2831853f;
            buf[i * 2] = buf[i * 2 + 1] = v;
        }
        Speaker_Ring.push(buf, C2_SAMPLES_PER_FRAME * 2);
    }
    Speaker_Play_Now = true;
}

static void send_ctrl(uint8_t type, uint8_t arg) {
    uint8_t pkt[PKT_CTRL_SIZE] = { PKT_MAGIC_CTRL, (uint8_t)(Local_Node_Id & 0xFF),
                                   (uint8_t)(Local_Node_Id >> 8), type, arg };
    for (int i = 0; i < PKT_CTRL_REPEAT; i++) radio.transmit(pkt, sizeof(pkt));
    if (!Transmission_Mode) radio.startReceive();
}

static void handle_ctrl(const uint8_t* pkt) {
    uint8_t type = pkt[3], arg = pkt[4];
    if (type == last_ctrl_type && arg == last_ctrl_arg && millis() - last_ctrl_ms < 1000) return;  // on-air repeats
    last_ctrl_type = type;
    last_ctrl_arg = arg;
    last_ctrl_ms = millis();
    switch (type) {
        case CTRL_CALL_STATE:
            In_Call = (arg != 0);
            Serial.printf("gateway: %s\n", In_Call ? "CALL CONNECTED" : "call idle");
            queue_beep(In_Call ? 1200.0f : 600.0f, In_Call ? 200 : 120);
            break;
        case CTRL_BEEP:
            queue_beep(1000.0f, 120);    // phone side unkeyed: your turn
            break;
        default:
            break;
    }
}

// ---------------------------------------------------------------------------
// Setup
// ---------------------------------------------------------------------------
void setup() {
    Serial.begin(115200);
    delay(200);

    uint64_t mac = ESP.getEfuseMac();
    Local_Node_Id = (uint16_t)(mac ^ (mac >> 16) ^ (mac >> 32));

    pinMode(BOOT_KEY, INPUT_PULLUP);
    attachInterrupt(BOOT_KEY, Button_Interrupt, FALLING);

    pinMode(MAX98357A_SD_MODE, OUTPUT);
    digitalWrite(MAX98357A_SD_MODE, HIGH);

#if defined(T3_S3_MVSRBoard_V1_0)
    pinMode(MSM261_EN, OUTPUT);
    digitalWrite(MSM261_EN, HIGH);
    IIS_Mic->begin(i2s_mode_t::I2S_MODE_MASTER, ad_iis_data_mode_t::AD_IIS_DATA_IN,
                   i2s_channel_fmt_t::I2S_CHANNEL_FMT_ONLY_RIGHT, IIS_DATA_BIT, IIS_SAMPLE_RATE);
#else
    pinMode(MP34DT05TR_EN, OUTPUT);
    digitalWrite(MP34DT05TR_EN, LOW);
    IIS_Mic->begin(i2s_mode_t::I2S_MODE_PDM, ad_iis_data_mode_t::AD_IIS_DATA_IN,
                   i2s_channel_fmt_t::I2S_CHANNEL_FMT_ONLY_RIGHT, IIS_DATA_BIT, IIS_SAMPLE_RATE);
#endif

    IIS_Speaker->begin(i2s_mode_t::I2S_MODE_MASTER, ad_iis_data_mode_t::AD_IIS_DATA_OUT,
                       i2s_channel_fmt_t::I2S_CHANNEL_FMT_RIGHT_LEFT, IIS_DATA_BIT, IIS_SAMPLE_RATE);

    Serial.print("[SX1262] init FSK ... ");
    SPI.begin(LORA_SCLK, LORA_MISO, LORA_MOSI);
    int state = radio.beginFSK(FSK_FREQ_MHZ, FSK_BITRATE, FSK_FREQ_DEV, FSK_RX_BW,
                               FSK_PWR_DBM, FSK_PREAMBLE, 1.6f);
    if (state != RADIOLIB_ERR_NONE) {
        Serial.printf("failed, code %d\n", state);
        while (true) delay(1000);
    }
    Serial.println("ok");

    uint8_t syncWord[] = {0x2D, 0xD4};
    radio.setSyncWord(syncWord, sizeof(syncWord));
    radio.setCRC(2);
    radio.setWhitening(true);
    radio.setDio1Action(Radio_Interrupt);

    xTaskCreatePinnedToCore(Codec2_Task,   "AudioEngine", 32768, NULL, 5, NULL, 1);
    xTaskCreatePinnedToCore(MAX_Play_Task, "Player",      8192,  NULL, 6, NULL, 1);

    radio.startReceive();
    Serial.printf("node 0x%04X  <<< LISTEN >>>\n", Local_Node_Id);
    HEAP_AUDIT_ARM();   // everything after this point must be heap-free
}

// ---------------------------------------------------------------------------
// Main loop (core 0): button, radio TX, radio RX
// ---------------------------------------------------------------------------
static void enqueue_rx(const uint8_t* pkt) {
    uint16_t from = pkt[1] | (pkt[2] << 8);
    uint8_t  seq  = pkt[3];
    RxPacket rx;

    if (Rx_Have_Seq && from != Rx_Last_From) Rx_Have_Seq = false;  // new talker
    if (Rx_Have_Seq) {
        uint8_t gap = (uint8_t)(seq - Rx_Last_Seq - 1);
        if (gap == 0xFF) { Rx_Dropped++; return; }         // duplicate
        Rx_Lost += gap;
        if (gap > 0 && gap <= MAX_PLC_PACKETS) {           // short gap: conceal
            rx.lost = true;
            for (uint8_t i = 0; i < gap; i++) Radio_Rx_Queue.push(&rx, 1);
        }                                                  // long gap: just resume
    }
    Rx_Have_Seq  = true;
    Rx_Last_Seq  = seq;
    Rx_Last_From = from;

    rx.lost = false;
    memcpy(rx.payload, pkt + PACKET_HEADER_SIZE, PACKET_PAYLOAD_SIZE);
    if (!Radio_Rx_Queue.push(&rx, 1)) Rx_Dropped++;       // queue full: drop newest
    Rx_Packets++;
}

void loop() {
    // --- Button: short press toggles PTT, long press asks the gateway ---
    bool toggle_ptt = false;
    if (Boot_Key_Flag) {
        Boot_Key_Flag = false;
        unsigned long now = millis();
        if (!button_down && now - last_button_ms >= DEBOUNCE_MS) {
            last_button_ms = now;
            button_down = true;
            long_press_done = false;
            press_start_ms = now;
        }
    }
    if (button_down) {
        bool held = digitalRead(BOOT_KEY) == LOW;
        if (held && !long_press_done && millis() - press_start_ms >= LONG_PRESS_MS) {
            long_press_done = true;
            Serial.printf("long press: %s\n", In_Call ? "HANGUP" : "CALL");
            send_ctrl(In_Call ? CTRL_HANGUP : CTRL_CALL, 0);
            queue_beep(800.0f, 80);
        }
        if (!held) {
            button_down = false;
            if (!long_press_done) toggle_ptt = true;
        }
    }
    if (toggle_ptt) {
        {
            Transmission_Mode = !Transmission_Mode;
            Radio_Tx_Ring.clear();
            Radio_Rx_Queue.clear();
            Speaker_Ring.clear();
            Rx_Have_Seq = false;
            if (Transmission_Mode) {
                radio.standby();
                Serial.println(">>> TALK <<<");
            } else {
                Serial.printf("<<< LISTEN >>>  rx=%lu lost=%lu dropped=%lu heap_allocs=%lu min_free=%lu\n",
                              (unsigned long)Rx_Packets, (unsigned long)Rx_Lost,
                              (unsigned long)Rx_Dropped, (unsigned long)HEAP_AUDIT_COUNT(),
                              (unsigned long)esp_get_minimum_free_heap_size());
                Radio_Operation_Flag = false;
                radio.startReceive();
            }
        }
    }

    // --- TX: one packet whenever five frames are ready (~21 ms blocking) ---
    if (Transmission_Mode) {
        if (Radio_Tx_Ring.pop(&Send_Package[PACKET_HEADER_SIZE], PACKET_PAYLOAD_SIZE)) {
            prepare_header();
            radio.transmit(Send_Package, PACKET_SIZE);
        }
        Radio_Operation_Flag = false;   // TX_DONE, nothing to read
        vTaskDelay(1);
        return;
    }

    // --- RX ---
    if (Radio_Operation_Flag) {
        Radio_Operation_Flag = false;
        uint32_t flags = radio.getIrqFlags();
        if ((flags & RADIOLIB_SX126X_IRQ_RX_DONE) && !(flags & RADIOLIB_SX126X_IRQ_CRC_ERR)) {
            size_t len = radio.getPacketLength();
            if ((len == PACKET_SIZE || len == PKT_CTRL_SIZE) &&
                radio.readData(Receive_Package, len) == RADIOLIB_ERR_NONE) {
                uint16_t from = Receive_Package[1] | (Receive_Package[2] << 8);
                if (from != Local_Node_Id) {
                    if (len == PACKET_SIZE && Receive_Package[0] == PACKET_MAGIC) enqueue_rx(Receive_Package);
                    else if (len == PKT_CTRL_SIZE && Receive_Package[0] == PKT_MAGIC_CTRL) handle_ctrl(Receive_Package);
                }
            }
        }
        radio.startReceive();
    }
    vTaskDelay(1);
}
