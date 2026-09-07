#include "fsk_link.h"
#include "fsk_proto.h"
#include "esp_s3_hal.hpp"

#include "esp_log.h"
#include "esp_mac.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"

static const char *TAG = "fsk_link";

static EspS3Hal *s_hal = nullptr;
static Module   *s_mod = nullptr;
static SX1262   *s_radio = nullptr;
static SemaphoreHandle_t s_lock = nullptr;    // serialises RadioLib calls
static SemaphoreHandle_t s_irq = nullptr;     // DIO1 -> receive()
static volatile int64_t  s_last_rx_us = -1;
static uint16_t s_node_id = 0;

static void IRAM_ATTR dio1_isr(void)
{
    BaseType_t woken = pdFALSE;
    xSemaphoreGiveFromISR(s_irq, &woken);
    if (woken) portYIELD_FROM_ISR();
}

uint16_t fsk_link_node_id(void) { return s_node_id; }

int fsk_link_init(const fsk_link_pins_t *p)
{
    uint8_t mac[6] = {};
    esp_read_mac(mac, ESP_MAC_WIFI_STA);
    s_node_id = (uint16_t)((mac[0] ^ mac[2] ^ mac[4]) | ((mac[1] ^ mac[3] ^ mac[5]) << 8));

    s_lock = xSemaphoreCreateMutex();
    s_irq  = xSemaphoreCreateBinary();

    s_hal   = new EspS3Hal((spi_host_device_t)p->spi_host, p->sck, p->miso, p->mosi);
    s_mod   = new Module(s_hal, p->cs, p->dio1, p->rst, p->busy);
    s_radio = new SX1262(s_mod);

    int st = s_radio->beginFSK(FSK_FREQ_MHZ, FSK_BITRATE_KBPS, FSK_FREQ_DEV_KHZ,
                               FSK_RX_BW_KHZ, FSK_PWR_DBM, FSK_PREAMBLE_BITS, 1.6f);
    if (st != RADIOLIB_ERR_NONE) {
        ESP_LOGE(TAG, "beginFSK failed: %d", st);
        return st;
    }
    uint8_t sync[] = FSK_SYNC_WORD;
    s_radio->setSyncWord(sync, sizeof(sync));
    s_radio->setCRC(2);
    s_radio->setWhitening(true);
    s_radio->setDio1Action(dio1_isr);
    st = s_radio->startReceive();
    ESP_LOGI(TAG, "SX1262 FSK up: %.1f MHz %.1f kbps, node 0x%04X",
             FSK_FREQ_MHZ, FSK_BITRATE_KBPS, s_node_id);
    return st;
}

int fsk_link_transmit(const uint8_t *data, size_t len)
{
    xSemaphoreTake(s_lock, portMAX_DELAY);
    int st = s_radio->transmit(const_cast<uint8_t *>(data), len);
    s_radio->startReceive();
    xSemaphoreGive(s_lock);
    // transmit() consumed TX_DONE itself; a DIO1 edge may still have posted
    // the semaphore, receive() tolerates that (no RX_DONE flag -> false).
    return st;
}

bool fsk_link_receive(fsk_link_pkt_t *pkt, uint32_t timeout_ms)
{
    if (xSemaphoreTake(s_irq, pdMS_TO_TICKS(timeout_ms)) != pdTRUE) return false;

    bool ok = false;
    xSemaphoreTake(s_lock, portMAX_DELAY);
    uint32_t flags = s_radio->getIrqFlags();
    if ((flags & RADIOLIB_SX126X_IRQ_RX_DONE) && !(flags & RADIOLIB_SX126X_IRQ_CRC_ERR)) {
        size_t len = s_radio->getPacketLength();
        if (len > 0 && len <= sizeof(pkt->data) &&
            s_radio->readData(pkt->data, len) == RADIOLIB_ERR_NONE) {
            pkt->len = (uint8_t)len;
            pkt->rssi_dbm = (int16_t)s_radio->getRSSI();
            s_last_rx_us = esp_timer_get_time();
            ok = true;
        }
    }
    s_radio->startReceive();
    xSemaphoreGive(s_lock);
    return ok;
}

uint32_t fsk_link_ms_since_rx(void)
{
    int64_t t = s_last_rx_us;
    if (t < 0) return UINT32_MAX;
    return (uint32_t)((esp_timer_get_time() - t) / 1000);
}
