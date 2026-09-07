#include "audio_io.h"
#include "board_mvsr.h"
#include "walkie_config.h"

#include "driver/i2s_std.h"
#include "driver/i2s_pdm.h"
#include "driver/gpio.h"
#include "esp_log.h"

static const char *TAG = "audio_io";

static i2s_chan_handle_t s_spk = NULL;   // TX  -> MAX98357A          (I2S1, std)
static i2s_chan_handle_t s_mic = NULL;   // RX  <- MP34DT05-A PDM mic (I2S0, pdm)

#define AUDIO_TIMEOUT_MS 200

// Speaker: standard I2S TX, 8 kHz mono (MAX98357A handles 8 kHz natively).
static void init_speaker(void)
{
    i2s_chan_config_t cc = I2S_CHANNEL_DEFAULT_CONFIG(MVSR_SPK_I2S_PORT, I2S_ROLE_MASTER);
    // Explicit ring depth instead of the IDF default (6x240 => up to 180 ms of
    // hidden latency). One descriptor = one 20 ms frame at 8 kHz, x4 deep.
    cc.dma_desc_num = POC_SPK_DMA_DESC_NUM;
    cc.dma_frame_num = POC_SPK_DMA_FRAME_NUM;
    ESP_ERROR_CHECK(i2s_new_channel(&cc, &s_spk, NULL));     // TX only

    i2s_std_config_t std = {
        .clk_cfg  = I2S_STD_CLK_DEFAULT_CONFIG(POC_SAMPLE_RATE_HZ),
        .slot_cfg = I2S_STD_PHILIPS_SLOT_DEFAULT_CONFIG(I2S_DATA_BIT_WIDTH_16BIT, I2S_SLOT_MODE_MONO),
        .gpio_cfg = {
            .mclk = I2S_GPIO_UNUSED,
            .bclk = MVSR_SPK_BCLK,
            .ws   = MVSR_SPK_LRCLK,
            .dout = MVSR_SPK_DATA,
            .din  = I2S_GPIO_UNUSED,
            .invert_flags = { .mclk_inv = false, .bclk_inv = false, .ws_inv = false },
        },
    };
    ESP_ERROR_CHECK(i2s_channel_init_std_mode(s_spk, &std));
    ESP_ERROR_CHECK(i2s_channel_enable(s_spk));
}

// Mic: PDM RX (MP34DT05-A). ESP32-S3 has a hardware PDM->PCM filter, so we read
// plain 16-bit PCM. Captured at 16 kHz (PDM2PCM is speced ~16-48 kHz) and
// decimated 2:1 to the 8 kHz G.711 rate in audio_read_mic().
static void init_mic(void)
{
    i2s_chan_config_t cc = I2S_CHANNEL_DEFAULT_CONFIG(MVSR_MIC_I2S_PORT, I2S_ROLE_MASTER);
    // Explicit ring depth — see init_speaker(). One descriptor = one 20 ms
    // frame at the 16 kHz capture rate, x4 deep.
    cc.dma_desc_num = POC_MIC_DMA_DESC_NUM;
    cc.dma_frame_num = POC_MIC_DMA_FRAME_NUM;
    ESP_ERROR_CHECK(i2s_new_channel(&cc, NULL, &s_mic));     // RX only

    i2s_pdm_rx_config_t pdm = {
        .clk_cfg  = I2S_PDM_RX_CLK_DEFAULT_CONFIG(POC_MIC_CAPTURE_HZ),
        .slot_cfg = I2S_PDM_RX_SLOT_DEFAULT_CONFIG(I2S_DATA_BIT_WIDTH_16BIT, I2S_SLOT_MODE_MONO),
        .gpio_cfg = {
            .clk = MVSR_MIC_PDM_CLK,
            .din = MVSR_MIC_PDM_DIN,
            .invert_flags = { .clk_inv = false },
        },
    };
    ESP_ERROR_CHECK(i2s_channel_init_pdm_rx_mode(s_mic, &pdm));
    ESP_ERROR_CHECK(i2s_channel_enable(s_mic));
}

esp_err_t audio_init(void)
{
    gpio_set_direction(MVSR_MIC_EN, GPIO_MODE_OUTPUT);
    gpio_set_direction(MVSR_SPK_SD_MODE, GPIO_MODE_OUTPUT);
    audio_mic_enable(true);
    audio_amp_enable(false);   // amp OFF until a call connects — avoids the
                               // continuous I2S-underrun click while ringing/idle

    init_speaker();
    init_mic();
    ESP_LOGI(TAG, "audio up: PDM mic=I2S%d @ %d Hz ->/%d-> %d Hz, spk=I2S%d @ %d Hz",
             MVSR_MIC_I2S_PORT, POC_MIC_CAPTURE_HZ, POC_MIC_DECIMATE,
             POC_SAMPLE_RATE_HZ, MVSR_SPK_I2S_PORT, POC_SAMPLE_RATE_HZ);
    return ESP_OK;
}

// Read `samples` of 8 kHz PCM: capture samples*DECIMATE at 16 kHz, average down.
size_t audio_read_mic(int16_t *buf, size_t samples)
{
    static int16_t tmp[POC_FRAME_SAMPLES * POC_MIC_DECIMATE];
    const size_t cap = sizeof(tmp) / sizeof(tmp[0]);

    size_t want = samples * POC_MIC_DECIMATE;
    if (want > cap) want = cap;

    size_t got = 0;
    if (i2s_channel_read(s_mic, tmp, want * sizeof(int16_t), &got, AUDIO_TIMEOUT_MS) != ESP_OK)
        return 0;

    size_t in  = got / sizeof(int16_t);
    size_t out = in / POC_MIC_DECIMATE;
    for (size_t i = 0; i < out; i++) {
        int32_t acc = 0;
        for (int d = 0; d < POC_MIC_DECIMATE; d++)
            acc += tmp[i * POC_MIC_DECIMATE + d];
        buf[i] = (int16_t)(acc / POC_MIC_DECIMATE);     // 2-tap average == crude LPF
    }
    return out;
}

size_t audio_write_spk(const int16_t *buf, size_t samples)
{
    size_t put = 0;
    if (i2s_channel_write(s_spk, buf, samples * sizeof(int16_t), &put, AUDIO_TIMEOUT_MS) != ESP_OK)
        return 0;
    return put / sizeof(int16_t);
}

void audio_amp_enable(bool on) { gpio_set_level(MVSR_SPK_SD_MODE, on ? 1 : 0); }
void audio_mic_enable(bool on) { gpio_set_level(MVSR_MIC_EN, on ? 1 : 0); }
