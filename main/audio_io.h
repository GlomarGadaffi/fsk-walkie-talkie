#pragma once
#include <stdint.h>
#include <stddef.h>
#include <stdbool.h>
#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

// Bring up both I2S buses (mic RX on I2S0, speaker TX on I2S1) + enable pins.
esp_err_t audio_init(void);

// Blocking-ish I2S transfers. Return the number of SAMPLES moved.
size_t audio_read_mic(int16_t *buf, size_t samples);
size_t audio_write_spk(const int16_t *buf, size_t samples);

void audio_amp_enable(bool on);   // MAX98357A SD_MODE
void audio_mic_enable(bool on);   // mic EN rail

#ifdef __cplusplus
}
#endif
