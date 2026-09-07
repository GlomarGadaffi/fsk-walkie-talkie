#pragma once
// ─────────────────────────────────────────────────────────────────────────
//  LilyGO T3-S3 MVSRBoard  —  pin map  (board rev **V1.1**)
//  Source: Xinyuan-LilyGO/T3-S3-MVSRBoard  libraries/private_library/pin_config.h
//
//  Mic  : MP34DT05-A   PDM MEMS  (V1.1)   <- 2-wire: CLK + DATA, no BCLK/WS
//  Amp  : MAX98357A    I2S class-D, 9 dB gain
//
//  ESP32-S3 constraint: PDM RX is only available on I2S0, so the mic takes
//  I2S0 and the speaker (standard I2S TX) moves to I2S1. Two independent
//  controllers -> full-duplex capable, and now full-duplex by default
//  (see media_tasks.cpp; AEC tracked separately, see README roadmap).
//
//  (V1.0 boards instead have an MSM261 *I2S* mic on BCLK 47 / WS 15 / DATA 48;
//   that needs i2s_channel_init_std_mode — see git history for the V1.0 variant.)
//
//  *** This is the DEV board. Keep all board specifics in this one header so
//      the eventual product board is a single-file swap. ***
// ─────────────────────────────────────────────────────────────────────────

// ── Microphone (PDM RX, MP34DT05-A) — must be I2S0 on ESP32-S3 ────────────
#define MVSR_MIC_I2S_PORT     I2S_NUM_0
#define MVSR_MIC_PDM_CLK      GPIO_NUM_15   // PDM clock out  (pin_config: LRCLK)
#define MVSR_MIC_PDM_DIN      GPIO_NUM_48   // PDM data in    (pin_config: DATA)
#define MVSR_MIC_EN           GPIO_NUM_35   // drive HIGH to power the mic

// ── Speaker (I2S TX -> MAX98357A) — moved to I2S1 (I2S0 taken by PDM mic) ──
#define MVSR_SPK_I2S_PORT     I2S_NUM_1
#define MVSR_SPK_BCLK         GPIO_NUM_40
#define MVSR_SPK_LRCLK        GPIO_NUM_41   // WS
#define MVSR_SPK_DATA         GPIO_NUM_39
#define MVSR_SPK_SD_MODE      GPIO_NUM_38   // drive HIGH to enable amp (default gain)

// ── Vibration motor (PWM) — used to simulate the doorbell "actuation" ─────
#define MVSR_MOTOR            GPIO_NUM_46

// ── BOOT button — was push-to-talk; unused now that media is full-duplex ──
//    (mic->RTP and RTP->spk both run continuously, see media_tasks.cpp).
//    Pressed = LOW. Still configured as input w/ pull-up in gpio_init();
//    free for a future mute/override control.
#define MVSR_PTT_BUTTON       GPIO_NUM_0

// ── Optional peripherals (not used by the spike, documented for later) ────
#define MVSR_OLED_SDA         GPIO_NUM_18
#define MVSR_OLED_SCL         GPIO_NUM_17
