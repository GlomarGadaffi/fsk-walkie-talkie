# fsk-walkie-talkie

Voice over narrowband FSK on a LilyGo T3-S3 (ESP32-S3 + SX1262) with the
[T3-S3-MVSRBoard](https://lilygo.cc/en-us/products/t3-s3-mvsr) back plate
(PDM mic + MAX98357A speaker amp). Codec2 mode 3200, 19.2 kbps GFSK.

Press BOOT to talk, press BOOT again to listen. Two or more boards on the
same channel form a simplex net.

## Numbers

| | |
|---|---|
| Codec | Codec2 3200: 160 samples (20 ms) → 8 bytes |
| Packet | 4-byte header + 5 frames (40 bytes) = 44 bytes = 100 ms of speech |
| Header | magic `0xC2`, 16-bit node id, 8-bit sequence |
| Radio | 914.6 MHz, GFSK 19.2 kbps, ±10 kHz deviation, 39 kHz RX BW, sync `2D D4`, CRC-16, whitening |
| Air time | ~18 ms payload, ~21 ms with preamble/sync/length/CRC → ~21 % TX duty cycle |
| Latency | ~100 ms packetisation + 200 ms jitter prefill + 21 ms air ≈ 320 ms mouth to ear |

## What the code actually does

**Talk path (core 1 → core 0):** the codec task blocks on the I2S mic for one
20 ms frame, encodes it and pushes 8 bytes into the TX ring. The main loop
pops 40 bytes at a time, stamps the header and calls a blocking `transmit()`.

**Listen path:** the SX1262 DIO1 interrupt sets a flag; the main loop checks
`RX_DONE` and not `CRC_ERR`, rejects anything that isn't exactly 44 bytes with
the magic byte, drops its own node id, and hands the payload to a packet queue.
Sequence numbers (tracked per talker) detect duplicates, which are dropped,
and gaps: up to two missing packets are replaced by "lost" markers; longer
gaps just resume.

**Decode:** the codec task pops packets, decodes the five frames, and pushes
mono→stereo samples with saturating gain into the speaker ring. A lost marker
re-decodes the last good frame at reduced level (repeat-last-frame PLC);
beyond two lost packets the output goes silent. The last-good history is
forgotten after 500 ms without packets so a new burst never starts with a
stale frame.

**Playback:** the speaker task waits until two packets (200 ms) are buffered
before starting, then feeds one 20 ms chunk to I2S at a time. If the ring runs
dry it writes zeros instead of stalling, and after 200 ms of starvation it
returns to prefill so the next burst starts with a fresh jitter margin.

**Ring buffers** are mutex-protected and all-or-nothing: a push either lands
completely or not at all, so codec frame boundaries can never shift when a
buffer fills. Every capacity is an exact multiple of what is pushed into it.

## Phone patch

With a [tincan-autopatch](https://github.com/GlomarGadaffi/tincan-autopatch)
gateway on the channel, the walkie net reaches a pocket-dial phone system.
**Hold BOOT for 0.8 s** to ask the gateway to dial its preset extension; hold
again during the call to hang up. The gateway broadcasts call state, so the
walkie double-beeps when the call connects and low-beeps when it ends, and
sounds a 1 kHz roger beep whenever the phone side stops talking (your turn).
Beeps are synthesised locally from 5-byte control packets (magic `0xC3`),
never sent through Codec2. A short press still toggles push-to-talk.

## Building (ESP-IDF, no Arduino)

The primary build is plain ESP-IDF v6.0. RadioLib comes through the IDF
component manager (declared by `components/fsk_link`), Codec2 is fetched, the
MVSR mic/amp path is tincan's MIT `audio_io.c`. Nothing from LilyGo's GPL
`Arduino_DriveBus` is involved.

```sh
./scripts/fetch_idf_deps.sh      # Codec2 sources into components/codec2/upstream (LGPL, not vendored)
idf.py set-target esp32s3
idf.py build flash monitor
```

Image is ~310 KB. `components/fsk_link` (SX1262 GFSK link with an ESP32-S3
HAL for RadioLib, plus `fsk_proto.h`, the one definition of the wire format)
and `components/codec2` are also consumed by
[tincan-autopatch](https://github.com/GlomarGadaffi/tincan-autopatch) via
`EXTRA_COMPONENT_DIRS`, the way tincan consumes tincan-core.

Layout:

```
main/                 app_main.cpp (tasks + button), audio_io.c/.h + board_mvsr.h (from tincan), walkie_config.h
components/fsk_link/  RadioLib on esp_driver_spi/gpio/esp_timer; C API; fsk_proto.h
components/codec2/    CMake wrapper, mode 3200 only; upstream/ is fetched
src/, platformio.ini  legacy Arduino build (below), kept until the IDF build is air-tested
```

## Building (legacy Arduino / PlatformIO)

Requires [PlatformIO](https://platformio.org/). RadioLib and sh123's
`esp32_codec2_arduino` are pulled automatically. LilyGo's `Arduino_DriveBus`
I2S wrapper is not on the registry and is GPL-3.0, so it is fetched rather
than vendored:

```sh
./scripts/fetch_deps.sh      # sparse-clones lib/Arduino_DriveBus from LilyGo's repo
pio run -t upload
pio device monitor
```

Without git/bash: copy `libraries/Arduino_DriveBus` from
<https://github.com/Xinyuan-LilyGO/T3-S3-MVSRBoard> into `lib/`.

For the older V1.0 back plate (MSM261 I2S mic instead of the PDM MP34DT05TR)
uncomment `-D T3_S3_MVSRBoard_V1_0` in `platformio.ini`.

Serial output prints the node id at boot, `>>> TALK <<<` / `<<< LISTEN >>>`
on each toggle, and rx/lost/dropped counters when returning to listen.

## Spectrum

The defaults transmit +22 dBm narrowband GFSK on 914.6 MHz. That does **not**
qualify for the FCC Part 15.247 unlicensed digital-modulation allowance
(which needs ≥500 kHz occupied bandwidth or frequency hopping); unlicensed it
would fall under 15.249, whose field-strength limit is far below this power.
It is legal for a licensed amateur on the 33 cm band. Know your rules, set
`FSK_FREQ_MHZ` / `FSK_PWR_DBM` accordingly, and identify.

## Lineage and licence

Started from LilyGo's `SX126x_Walkie_Talkie` example in the MVSRBoard repo
(GPL-3.0), which uses unsynchronised `std::vector`s across cores and a
200-byte payload. This rewrite keeps the hardware setup and replaces the
buffering, framing and receive path. Note the derivation when choosing how to
licence downstream work.

## Not yet verified on hardware

Both builds compile; neither this revision's sequence/PLC/jitter path nor the
ESP-IDF port has been air-tested. Please open an issue with the serial
counters if it misbehaves.

## Heap use in the hot path: none

Everything the talk/listen loop touches is allocated once at start-up and
never again:

- Ring buffers, codec scratch buffers and packet staging are `static`.
- Codec2 3200 allocates its state and FFT tables in `codec2_create`. Its
  encode/decode path uses fixed and variable-length *stack* arrays; the FFTs
  are 512 and 128 points, so kiss_fft never enters its generic-radix or
  temp-buffer branches, and the in-place FFT wrapper copies through a stack
  buffer.
- RadioLib's `transmit(uint8_t*, len)`, `getPacketLength()` and
  `readData(uint8_t*, len)` write straight to the SX1262 buffer. Only the
  `String` overloads, which this code never calls, allocate.
- I2S reads and writes go to DMA buffers created in `begin()`.

To verify rather than trust that, build the audit variant:

```sh
pio run -e t3s3_sx1262_heapaudit -t upload
```

It links with `--wrap=malloc,calloc,realloc` and starts counting at the end
of `setup()`. Every return to listen prints
`heap_allocs=<n> min_free=<bytes>`; `n` must stay 0 across talk/listen
cycles. The wrapper catches C++ `new` and Codec2's `codec2_malloc` as well
(checked in the linked ELF).
