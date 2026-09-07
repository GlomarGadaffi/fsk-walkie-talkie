#pragma once
// RadioLib hardware abstraction for ESP32-S3 on plain ESP-IDF (no Arduino).
// SPI via esp_driver_spi (polling transfers, RadioLib drives CS itself),
// GPIO via esp_driver_gpio with the shared ISR service, time via esp_timer.
#include <RadioLib.h>
#include "driver/gpio.h"
#include "driver/spi_master.h"
#include "esp_timer.h"
#include "esp_rom_sys.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

#define HAL_LOW     0
#define HAL_HIGH    1
#define HAL_INPUT   0
#define HAL_OUTPUT  1
#define HAL_RISING  2
#define HAL_FALLING 3

class EspS3Hal : public RadioLibHal {
public:
    EspS3Hal(spi_host_device_t host, int sck, int miso, int mosi)
        : RadioLibHal(HAL_INPUT, HAL_OUTPUT, HAL_LOW, HAL_HIGH, HAL_RISING, HAL_FALLING),
          _host(host), _sck(sck), _miso(miso), _mosi(mosi) {}

    void init() override {
        static bool isrService = false;
        if (!isrService) {
            gpio_install_isr_service(0);
            isrService = true;
        }
        spiBegin();
    }
    void term() override { spiEnd(); }

    void pinMode(uint32_t pin, uint32_t mode) override {
        if (pin == RADIOLIB_NC) return;
        gpio_config_t cfg = {};
        cfg.pin_bit_mask = 1ULL << pin;
        cfg.mode = (mode == HAL_OUTPUT) ? GPIO_MODE_OUTPUT : GPIO_MODE_INPUT;
        cfg.pull_up_en = GPIO_PULLUP_DISABLE;
        cfg.pull_down_en = GPIO_PULLDOWN_DISABLE;
        cfg.intr_type = GPIO_INTR_DISABLE;
        gpio_config(&cfg);
    }
    void digitalWrite(uint32_t pin, uint32_t value) override {
        if (pin == RADIOLIB_NC) return;
        gpio_set_level((gpio_num_t)pin, value);
    }
    uint32_t digitalRead(uint32_t pin) override {
        if (pin == RADIOLIB_NC) return 0;
        return gpio_get_level((gpio_num_t)pin);
    }
    void attachInterrupt(uint32_t pin, void (*cb)(void), uint32_t mode) override {
        if (pin == RADIOLIB_NC) return;
        gpio_set_intr_type((gpio_num_t)pin, mode == HAL_RISING ? GPIO_INTR_POSEDGE : GPIO_INTR_NEGEDGE);
        gpio_isr_handler_add((gpio_num_t)pin, [](void *arg) { ((void (*)(void))arg)(); }, (void *)cb);
        gpio_intr_enable((gpio_num_t)pin);
    }
    void detachInterrupt(uint32_t pin) override {
        if (pin == RADIOLIB_NC) return;
        gpio_intr_disable((gpio_num_t)pin);
        gpio_isr_handler_remove((gpio_num_t)pin);
    }

    void delay(RadioLibTime_t ms) override {
        if (ms == 0) { taskYIELD(); return; }
        vTaskDelay(pdMS_TO_TICKS(ms) ? pdMS_TO_TICKS(ms) : 1);
    }
    void delayMicroseconds(RadioLibTime_t us) override { esp_rom_delay_us(us); }
    void yield() override { taskYIELD(); }
    RadioLibTime_t millis() override { return (RadioLibTime_t)(esp_timer_get_time() / 1000); }
    RadioLibTime_t micros() override { return (RadioLibTime_t)esp_timer_get_time(); }
    long pulseIn(uint32_t, uint32_t, RadioLibTime_t) override { return 0; }

    void spiBegin() override {
        if (_dev) return;
        spi_bus_config_t bus = {};
        bus.sclk_io_num = _sck;
        bus.miso_io_num = _miso;
        bus.mosi_io_num = _mosi;
        bus.quadwp_io_num = -1;
        bus.quadhd_io_num = -1;
        bus.max_transfer_sz = 0;   // default; polling (no DMA) transfers stay under 64 B
        ESP_ERROR_CHECK(spi_bus_initialize(_host, &bus, SPI_DMA_DISABLED));
        spi_device_interface_config_t dev = {};
        dev.clock_speed_hz = 8 * 1000 * 1000;
        dev.mode = 0;
        dev.spics_io_num = -1;     // RadioLib toggles CS through digitalWrite
        dev.queue_size = 1;
        dev.flags = SPI_DEVICE_NO_DUMMY;
        ESP_ERROR_CHECK(spi_bus_add_device(_host, &dev, &_dev));
    }
    void spiBeginTransaction() override { spi_device_acquire_bus(_dev, portMAX_DELAY); }
    void spiTransfer(uint8_t *out, size_t len, uint8_t *in) override {
        spi_transaction_t t = {};
        t.length = len * 8;
        t.tx_buffer = out;
        t.rx_buffer = in;
        spi_device_polling_transmit(_dev, &t);
    }
    void spiEndTransaction() override { spi_device_release_bus(_dev); }
    void spiEnd() override {
        if (!_dev) return;
        spi_bus_remove_device(_dev);
        spi_bus_free(_host);
        _dev = nullptr;
    }

private:
    spi_host_device_t _host;
    int _sck, _miso, _mosi;
    spi_device_handle_t _dev = nullptr;
};
