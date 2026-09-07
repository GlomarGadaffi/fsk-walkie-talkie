// Pin map for LilyGo T3-S3 (V1.2) + T3-S3-MVSRBoard back plate.
// Values match LilyGo's private_library/pin_config.h; written fresh here so
// the project builds without copying that GPL header.
#pragma once

// ---- Select back-plate revision (V1.1 is the one currently sold) ----------
// #define T3_S3_MVSRBoard_V1_0
#ifndef T3_S3_MVSRBoard_V1_0
#define T3_S3_MVSRBoard_V1_1
#endif

// ---- Radio: SX1262 variant of the T3-S3 ---------------------------------
#define LORA_CS   7
#define LORA_RST  8
#define LORA_SCLK 5
#define LORA_MOSI 6
#define LORA_MISO 3
#define LORA_DIO1 33
#define LORA_BUSY 34

// ---- Buttons / LED ------------------------------------------------------
#define BOOT_KEY 0
#define LED_1    37

// ---- Microphone ---------------------------------------------------------
#if defined(T3_S3_MVSRBoard_V1_0)
// MSM261S4030H0R, I2S
#define MSM261_EN   35
#define MSM261_BCLK 47
#define MSM261_WS   15
#define MSM261_DATA 48
#elif defined(T3_S3_MVSRBoard_V1_1)
// MP34DT05TR, PDM
#define MP34DT05TR_LRCLK 15
#define MP34DT05TR_DATA  48
#define MP34DT05TR_EN    35
#else
#error "Select T3_S3_MVSRBoard_V1_0 or T3_S3_MVSRBoard_V1_1"
#endif

// ---- Speaker amp: MAX98357A, I2S ----------------------------------------
#define MAX98357A_BCLK    40
#define MAX98357A_LRCLK   41
#define MAX98357A_DATA    39
#define MAX98357A_SD_MODE 38
