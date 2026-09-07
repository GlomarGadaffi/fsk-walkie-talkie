#pragma once
// fsk-walkie-talkie (ESP-IDF build) configuration.
// Radio parameters and the wire format come from fsk_proto.h (components/fsk_link).

// --- Audio (names kept as audio_io.c expects; it is tincan's MIT file) ------
#define POC_SAMPLE_RATE_HZ    8000
#define POC_FRAME_SAMPLES     160               // 20 ms, one Codec2 frame
#define POC_MIC_CAPTURE_HZ    16000             // PDM->PCM filter wants >= 16 kHz
#define POC_MIC_DECIMATE      (POC_MIC_CAPTURE_HZ / POC_SAMPLE_RATE_HZ)
#define POC_SPK_DMA_DESC_NUM  4
#define POC_SPK_DMA_FRAME_NUM POC_FRAME_SAMPLES
#define POC_MIC_DMA_DESC_NUM  4
#define POC_MIC_DMA_FRAME_NUM (POC_FRAME_SAMPLES * POC_MIC_DECIMATE)

// --- Receive side --------------------------------------------------------
#define RX_QUEUE_PACKETS      8     // 800 ms of speech, bounded
#define MAX_PLC_PACKETS       2     // conceal up to 200 ms of loss, then silence
#define JITTER_PREFILL_PKTS   2     // 200 ms before playback starts
#define PLC_HISTORY_MS        500   // forget the last good frame after this much silence
#define AUDIO_GAIN            5     // speaker gain, saturating

// --- Button --------------------------------------------------------------
#define DEBOUNCE_MS           50
#define LONG_PRESS_MS         800   // hold: ask a tincan-autopatch gateway to call / hang up

// --- Tasks ---------------------------------------------------------------
#define TASK_STACK_CODEC      32768 // Codec2 uses VLAs
#define TASK_STACK_RADIO      4096
#define TASK_STACK_PLAYER     4096
#define TASK_PRIO_CODEC       5
#define TASK_PRIO_PLAYER      6
#define TASK_PRIO_RADIO       6
