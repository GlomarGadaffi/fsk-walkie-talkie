// fsk-walkie-talkie — ESP-IDF build. Voice over narrowband GFSK on the
// LilyGo T3-S3 (SX1262) with the MVSRBoard back plate (PDM mic, MAX98357A).
//
// Tasks:
//   radio  : SX1262 RX -> packet queue (audio) / control handling; drains
//            the TX ring into packets when talking
//   codec  : talk: mic -> Codec2 -> TX ring;  listen: packet queue -> Codec2
//            (+PLC) -> speaker ring
//   player : speaker ring -> I2S with jitter prefill, zeros on underrun
//   main   : BOOT button. Short press toggles talk/listen, hold LONG_PRESS_MS
//            asks a tincan-autopatch gateway to call (or hang up).
#include <string.h>
#include <math.h>

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/queue.h"
#include "freertos/semphr.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "nvs_flash.h"
#include "driver/gpio.h"
#include "driver/spi_master.h"

#include "walkie_config.h"
#include "board_mvsr.h"
#include "audio_io.h"
#include "fsk_link.h"
#include "fsk_proto.h"
#include "codec2.h"

static const char *TAG = "walkie";

// T3-S3 SX1262 pins (LilyGo pin_config.h) — the MVSR header only covers audio.
#define LORA_SPI_HOST SPI2_HOST
#define LORA_SCLK 5
#define LORA_MOSI 6
#define LORA_MISO 3
#define LORA_CS   7
#define LORA_RST  8
#define LORA_DIO1 33
#define LORA_BUSY 34
#define LED_PIN   GPIO_NUM_37

static inline uint32_t now_ms() { return (uint32_t)(esp_timer_get_time() / 1000); }

// ---------------------------------------------------------------------------
// All-or-nothing ring buffer (mutex protected)
// ---------------------------------------------------------------------------
template <typename T, size_t CAPACITY>
class RingBuffer {
public:
    RingBuffer() { _m = xSemaphoreCreateMutex(); }
    bool push(const T *d, size_t n) {
        bool ok = false;
        xSemaphoreTake(_m, portMAX_DELAY);
        if (_count + n <= CAPACITY) {
            for (size_t i = 0; i < n; i++) { _buf[_head] = d[i]; _head = (_head + 1) % CAPACITY; }
            _count += n; ok = true;
        }
        xSemaphoreGive(_m);
        return ok;
    }
    bool pop(T *d, size_t n) {
        bool ok = false;
        xSemaphoreTake(_m, portMAX_DELAY);
        if (_count >= n) {
            for (size_t i = 0; i < n; i++) { d[i] = _buf[_tail]; _tail = (_tail + 1) % CAPACITY; }
            _count -= n; ok = true;
        }
        xSemaphoreGive(_m);
        return ok;
    }
    size_t available() { xSemaphoreTake(_m, portMAX_DELAY); size_t c = _count; xSemaphoreGive(_m); return c; }
    void clear() { xSemaphoreTake(_m, portMAX_DELAY); _head = _tail = _count = 0; xSemaphoreGive(_m); }
private:
    T _buf[CAPACITY];
    size_t _head = 0, _tail = 0, _count = 0;
    SemaphoreHandle_t _m;
};

struct RxPacket {
    uint8_t payload[PKT_PAYLOAD_SIZE];
    bool lost;
};

static RingBuffer<uint8_t, PKT_PAYLOAD_SIZE * 6> s_tx_ring;                         // 600 ms
static RingBuffer<int16_t, POC_FRAME_SAMPLES * FRAMES_PER_PACKET * 4> s_spk_ring;  // 400 ms mono
static QueueHandle_t s_rx_q;                                                        // RxPacket

static volatile bool s_talk = false;          // false = listen
static volatile bool s_play_now = false;      // beep: skip jitter prefill
static volatile bool s_in_call = false;       // last CTRL_CALL_STATE from a gateway

// ---------------------------------------------------------------------------
// Local tones (never on air)
// ---------------------------------------------------------------------------
static void queue_beep(float hz, int ms)
{
    static int16_t buf[POC_FRAME_SAMPLES];
    static float phase = 0.0f;
    const float step = 6.2831853f * hz / POC_SAMPLE_RATE_HZ;
    for (int f = 0; f < (ms + 19) / 20; f++) {
        for (int i = 0; i < POC_FRAME_SAMPLES; i++) {
            buf[i] = (int16_t)(sinf(phase) * 6000.0f);
            phase += step;
            if (phase > 6.2831853f) phase -= 6.2831853f;
        }
        s_spk_ring.push(buf, POC_FRAME_SAMPLES);
    }
    s_play_now = true;
}

// ---------------------------------------------------------------------------
// Radio task
// ---------------------------------------------------------------------------
static void send_ctrl(uint8_t type, uint8_t arg)
{
    uint8_t pkt[PKT_CTRL_SIZE] = { PKT_MAGIC_CTRL, (uint8_t)(fsk_link_node_id() & 0xFF),
                                   (uint8_t)(fsk_link_node_id() >> 8), type, arg };
    for (int i = 0; i < PKT_CTRL_REPEAT; i++) fsk_link_transmit(pkt, sizeof(pkt));
}

static void handle_ctrl(const uint8_t *pkt)
{
    static uint32_t last_ms = 0;
    static uint8_t last_type = 0, last_arg = 0;
    uint8_t type = pkt[3], arg = pkt[4];
    if (type == last_type && arg == last_arg && now_ms() - last_ms < 1000) return;  // on-air repeats
    last_type = type; last_arg = arg; last_ms = now_ms();
    switch (type) {
        case CTRL_CALL_STATE:
            s_in_call = (arg != 0);
            ESP_LOGI(TAG, "gateway: %s", s_in_call ? "CALL CONNECTED" : "call idle");
            queue_beep(s_in_call ? 1200.0f : 600.0f, s_in_call ? 200 : 120);
            break;
        case CTRL_BEEP:
            queue_beep(1000.0f, 120);   // phone side unkeyed: your turn
            break;
        default:
            break;
    }
}

static void radio_task(void *)
{
    static fsk_link_pkt_t pkt;
    static uint8_t tx_pkt[PKT_AUDIO_SIZE];
    uint8_t tx_seq = 0;
    bool have_seq = false;
    uint8_t last_seq = 0;
    uint16_t last_from = 0;
    uint32_t rx = 0, lost = 0, dropped = 0;
    bool was_talk = false;

    for (;;) {
        if (s_talk != was_talk) {
            was_talk = s_talk;
            have_seq = false;
            if (!s_talk) ESP_LOGI(TAG, "<<< LISTEN >>>  rx=%lu lost=%lu dropped=%lu",
                                  (unsigned long)rx, (unsigned long)lost, (unsigned long)dropped);
        }

        // ---- talk: ship a packet whenever five frames are ready ----
        if (s_talk) {
            if (s_tx_ring.pop(&tx_pkt[PKT_HEADER_SIZE], PKT_PAYLOAD_SIZE)) {
                tx_pkt[0] = PKT_MAGIC_AUDIO;
                tx_pkt[1] = (uint8_t)(fsk_link_node_id() & 0xFF);
                tx_pkt[2] = (uint8_t)(fsk_link_node_id() >> 8);
                tx_pkt[3] = tx_seq++;
                fsk_link_transmit(tx_pkt, PKT_AUDIO_SIZE);
            } else {
                vTaskDelay(pdMS_TO_TICKS(5));
            }
            continue;
        }

        // ---- listen ----
        if (!fsk_link_receive(&pkt, 50)) continue;
        uint16_t from = pkt.data[1] | (pkt.data[2] << 8);
        if (from == fsk_link_node_id()) continue;

        if (pkt.len == PKT_CTRL_SIZE && pkt.data[0] == PKT_MAGIC_CTRL) { handle_ctrl(pkt.data); continue; }
        if (pkt.len != PKT_AUDIO_SIZE || pkt.data[0] != PKT_MAGIC_AUDIO) continue;

        uint8_t seq = pkt.data[3];
        RxPacket rp;
        if (have_seq && from != last_from) have_seq = false;           // new talker
        if (have_seq) {
            uint8_t gap = (uint8_t)(seq - last_seq - 1);
            if (gap == 0xFF) { dropped++; continue; }                  // duplicate
            lost += gap;
            if (gap > 0 && gap <= MAX_PLC_PACKETS) {                   // short gap: conceal
                rp.lost = true;
                for (uint8_t i = 0; i < gap; i++) xQueueSend(s_rx_q, &rp, 0);
            }
        }
        have_seq = true; last_seq = seq; last_from = from;
        rp.lost = false;
        memcpy(rp.payload, pkt.data + PKT_HEADER_SIZE, PKT_PAYLOAD_SIZE);
        if (xQueueSend(s_rx_q, &rp, 0) != pdTRUE) dropped++;           // queue full: drop newest
        rx++;
    }
}

// ---------------------------------------------------------------------------
// Codec task
// ---------------------------------------------------------------------------
static void codec_task(void *)
{
    struct CODEC2 *c2 = codec2_create(CODEC2_MODE_3200);
    if (!c2 || codec2_samples_per_frame(c2) != C2_SAMPLES_PER_FRAME ||
        codec2_bytes_per_frame(c2) != C2_BYTES_PER_FRAME) {
        ESP_LOGE(TAG, "codec2 init failed");
        vTaskDelete(NULL);
        return;
    }
    codec2_set_lpc_post_filter(c2, 1, 0, 0.8f, 0.2f);

    static int16_t pcm[C2_SAMPLES_PER_FRAME];
    static uint8_t code[C2_BYTES_PER_FRAME];
    static uint8_t last_good[C2_BYTES_PER_FRAME];
    static RxPacket rp;
    bool have_last_good = false;
    uint32_t last_good_ms = 0;
    const size_t spk_cap = POC_FRAME_SAMPLES * FRAMES_PER_PACKET * 4;

    for (;;) {
        if (s_talk) {
            if (audio_read_mic(pcm, POC_FRAME_SAMPLES) == POC_FRAME_SAMPLES) {
                codec2_encode(c2, code, pcm);
                s_tx_ring.push(code, C2_BYTES_PER_FRAME);   // drops whole frame if full
            } else {
                vTaskDelay(1);
            }
            continue;   // I2S read paces us at 20 ms
        }

        if (s_spk_ring.available() + POC_FRAME_SAMPLES * FRAMES_PER_PACKET > spk_cap) {
            vTaskDelay(pdMS_TO_TICKS(5));     // speaker ring full, let it drain
            continue;
        }
        if (xQueueReceive(s_rx_q, &rp, pdMS_TO_TICKS(5)) != pdTRUE) {
            if (have_last_good && now_ms() - last_good_ms > PLC_HISTORY_MS) have_last_good = false;
            continue;
        }
        for (int f = 0; f < FRAMES_PER_PACKET; f++) {
            float scale = 1.0f;
            if (!rp.lost) {
                memcpy(code, &rp.payload[f * C2_BYTES_PER_FRAME], C2_BYTES_PER_FRAME);
                memcpy(last_good, code, C2_BYTES_PER_FRAME);
                have_last_good = true;
                last_good_ms = now_ms();
            } else if (have_last_good) {
                memcpy(code, last_good, C2_BYTES_PER_FRAME);   // repeat-last-frame PLC
                scale = 0.6f;
            } else {
                scale = 0.0f;
            }
            if (scale > 0.0f) codec2_decode(c2, pcm, code);
            for (int i = 0; i < C2_SAMPLES_PER_FRAME; i++) {
                int32_t v = (scale > 0.0f) ? (int32_t)(pcm[i] * scale) * AUDIO_GAIN : 0;
                if (v > 32767) v = 32767;
                if (v < -32768) v = -32768;
                pcm[i] = (int16_t)v;
            }
            s_spk_ring.push(pcm, C2_SAMPLES_PER_FRAME);
        }
    }
}

// ---------------------------------------------------------------------------
// Player task: jitter prefill, zeros on underrun
// ---------------------------------------------------------------------------
static void player_task(void *)
{
    static int16_t buf[POC_FRAME_SAMPLES];
    static const int16_t silence[POC_FRAME_SAMPLES] = {0};
    const size_t prefill = POC_FRAME_SAMPLES * FRAMES_PER_PACKET * JITTER_PREFILL_PKTS;
    bool playing = false;
    int starved = 0;

    for (;;) {
        if (!playing) {
            if (s_spk_ring.available() >= prefill || s_play_now) { playing = true; starved = 0; s_play_now = false; }
            else { audio_write_spk(silence, POC_FRAME_SAMPLES); continue; }   // keep DMA fed, paces 20 ms
        }
        if (s_spk_ring.pop(buf, POC_FRAME_SAMPLES)) {
            starved = 0;
            audio_write_spk(buf, POC_FRAME_SAMPLES);
        } else {
            audio_write_spk(silence, POC_FRAME_SAMPLES);
            if (++starved >= FRAMES_PER_PACKET * MAX_PLC_PACKETS) playing = false;
        }
    }
}

// ---------------------------------------------------------------------------
// Main: button
// ---------------------------------------------------------------------------
static void set_talk(bool talk)
{
    s_talk = talk;
    s_tx_ring.clear();
    xQueueReset(s_rx_q);
    s_spk_ring.clear();
    gpio_set_level(LED_PIN, talk ? 1 : 0);
    if (talk) ESP_LOGI(TAG, ">>> TALK <<<");
}

extern "C" void app_main(void)
{
    ESP_ERROR_CHECK(nvs_flash_init());
    gpio_set_direction(MVSR_PTT_BUTTON, GPIO_MODE_INPUT);
    gpio_set_pull_mode(MVSR_PTT_BUTTON, GPIO_PULLUP_ONLY);
    gpio_set_direction(LED_PIN, GPIO_MODE_OUTPUT);
    gpio_set_level(LED_PIN, 0);

    ESP_ERROR_CHECK(audio_init());
    audio_amp_enable(true);   // player always feeds the DMA, so no idle click

    fsk_link_pins_t pins = { LORA_SPI_HOST, LORA_SCLK, LORA_MISO, LORA_MOSI,
                             LORA_CS, LORA_DIO1, LORA_RST, LORA_BUSY };
    if (fsk_link_init(&pins) != 0) {
        ESP_LOGE(TAG, "radio init failed -- halting");
        return;
    }

    s_rx_q = xQueueCreate(RX_QUEUE_PACKETS, sizeof(RxPacket));
    xTaskCreatePinnedToCore(radio_task,  "radio",  TASK_STACK_RADIO,  NULL, TASK_PRIO_RADIO,  NULL, 0);
    xTaskCreatePinnedToCore(codec_task,  "codec",  TASK_STACK_CODEC,  NULL, TASK_PRIO_CODEC,  NULL, 1);
    xTaskCreatePinnedToCore(player_task, "player", TASK_STACK_PLAYER, NULL, TASK_PRIO_PLAYER, NULL, 1);
    ESP_LOGI(TAG, "node 0x%04X  <<< LISTEN >>>", fsk_link_node_id());

    bool down = false, long_done = false;
    uint32_t press_ms = 0, last_edge_ms = 0;
    for (;;) {
        bool held = gpio_get_level(MVSR_PTT_BUTTON) == 0;
        uint32_t t = now_ms();
        if (held && !down && t - last_edge_ms >= DEBOUNCE_MS) {
            down = true; long_done = false; press_ms = t; last_edge_ms = t;
        }
        if (down && held && !long_done && t - press_ms >= LONG_PRESS_MS) {
            long_done = true;
            ESP_LOGI(TAG, "long press: %s", s_in_call ? "HANGUP" : "CALL");
            if (s_talk) set_talk(false);          // radio must be listening to hear the reply
            send_ctrl(s_in_call ? CTRL_HANGUP : CTRL_CALL, 0);
            queue_beep(800.0f, 80);
        }
        if (down && !held && t - last_edge_ms >= DEBOUNCE_MS) {
            down = false; last_edge_ms = t;
            if (!long_done) set_talk(!s_talk);
        }
        vTaskDelay(pdMS_TO_TICKS(10));
    }
}
