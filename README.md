# fsk-walkie-talkie

only operate with the utmost respect for the RF spectrum...

voice over narrowband FSK on a lilygo T3-S3 (ESP32-S3 + SX1262) with the
[T3-S3-MVSRBoard](https://lilygo.cc/en-us/products/t3-s3-mvsr) back plate
(pdm mic + MAX98357A amp). codec2 3200, 19.2 kbps GFSK. meshtastic users hate
this one weird trick.

press BOOT to talk, press BOOT again to listen. two or more boards on the
same channel and you have a net.

## numbers

| | |
|---|---|
| codec | codec2 3200: 160 samples (20 ms) → 8 bytes |
| packet | 4-byte header + 5 frames (40 bytes) = 44 bytes = 100 ms of speech |
| header | magic `0xC2`, 16-bit node id, 8-bit sequence |
| radio | 914.6 MHz, GFSK 19.2 kbps, ±10 kHz deviation, 39 kHz RX BW, sync `2D D4`, CRC-16, whitening |
| air time | ~18 ms payload, ~21 ms with preamble/sync/length/CRC → ~21 % TX duty cycle |
| latency | ~100 ms packetisation + 200 ms jitter prefill + 21 ms air ≈ 320 ms mouth to ear |

## what it actually does

**talk path:** the codec task blocks on the mic for one 20 ms frame, encodes
it, pushes 8 bytes into the TX ring. the radio task pops 40 bytes at a time,
stamps the header and does a blocking transmit.

**listen path:** DIO1 interrupt → check `RX_DONE` and not `CRC_ERR` → reject
anything that isn't exactly 44 bytes with the magic byte → drop our own node
id → hand the payload to a packet queue. sequence numbers (per talker) catch
duplicates (dropped) and gaps: up to two missing packets become "lost"
markers, longer gaps just resume.

**decode:** pop packets, decode five frames, saturating gain, into the speaker
ring. a lost marker re-decodes the last good frame at reduced level
(repeat-last-frame PLC); after two lost packets it goes quiet. the last-good
history is forgotten after 500 ms of nothing so a new burst never opens with a
stale frame.

**playback:** waits for two packets (200 ms) before starting, then feeds one
20 ms chunk to i2s at a time. ring runs dry → writes zeros instead of
stalling, and after 200 ms of that it goes back to prefill so the next burst
gets a fresh jitter margin.

**ring buffers** are mutex-protected and all-or-nothing: a push lands
completely or not at all, so a full buffer can never shift the codec frame
boundary. every capacity is an exact multiple of what gets pushed into it.
(the original leaked partial pushes and misaligned every frame after that.
cursed.)

## phone patch

with a [tincan-autopatch](https://github.com/GlomarGadaffi/tincan-autopatch)
gateway on the channel, the net reaches a pocket-dial phone system. **hold
BOOT for 0.8 s** to make the gateway dial its preset extension; hold again
during the call to hang up. the gateway broadcasts call state, so the walkie
double-beeps on connect, low-beeps on idle, and gives you a 1 kHz roger beep
whenever the phone side stops talking (your turn). beeps are synthesised
locally from 5-byte control packets (magic `0xC3`), never through codec2. a
short press still toggles PTT.

## building (esp-idf, no arduino)

the real build. esp-idf v6.0. radiolib comes through the idf component
manager (declared by `components/fsk_link`), codec2 gets fetched, the MVSR
mic/amp path is tincan's MIT `audio_io.c`. nothing from lilygo's GPL
`Arduino_DriveBus` is involved.

```sh
./scripts/fetch_idf_deps.sh      # codec2 sources into components/codec2/upstream (LGPL, not vendored)
idf.py set-target esp32s3
idf.py build flash monitor
```

image is ~310 KB. `components/fsk_link` (SX1262 GFSK link with an ESP32-S3
HAL for radiolib, plus `fsk_proto.h`, the one and only definition of the wire
format) and `components/codec2` are also consumed by tincan-autopatch via
`EXTRA_COMPONENT_DIRS`, same way tincan consumes tincan-core.

```
main/                 app_main.cpp (tasks + button), audio_io.c/.h + board_mvsr.h (from tincan), walkie_config.h
components/fsk_link/  radiolib on esp_driver_spi/gpio/esp_timer; C API; fsk_proto.h
components/codec2/    cmake wrapper, mode 3200 only; upstream/ is fetched
src/, platformio.ini  legacy arduino build (below), kept until the idf build is air-tested
```

## building (legacy arduino / platformio)

the old way, 828 KB of framework for the same thing. radiolib and sh123's
`esp32_codec2_arduino` pull automatically. lilygo's `Arduino_DriveBus` i2s
wrapper isn't on the registry and is GPL-3.0, so it's fetched, not vendored:

```sh
./scripts/fetch_deps.sh      # sparse-clones lib/Arduino_DriveBus from lilygo's repo
pio run -t upload
pio device monitor
```

no git/bash: copy `libraries/Arduino_DriveBus` from
<https://github.com/Xinyuan-LilyGO/T3-S3-MVSRBoard> into `lib/`.

older V1.0 back plate (MSM261 i2s mic instead of the pdm MP34DT05TR):
uncomment `-D T3_S3_MVSRBoard_V1_0` in `platformio.ini`.

serial prints the node id at boot, `>>> TALK <<<` / `<<< LISTEN >>>` on each
toggle, and rx/lost/dropped counters when returning to listen.

## heap use in the hot path: none

everything the talk/listen loop touches is allocated once at start-up and
never again:

- ring buffers, codec scratch and packet staging are `static`.
- codec2 3200 allocates its state and FFT tables in `codec2_create`. the
  encode/decode path uses fixed and variable-length *stack* arrays; the FFTs
  are 512 and 128 points so kiss_fft never enters its generic-radix or
  temp-buffer branches, and the in-place wrapper copies through a stack
  buffer.
- radiolib's `transmit(uint8_t*, len)`, `getPacketLength()` and
  `readData(uint8_t*, len)` go straight to the SX1262 buffer. only the
  `String` overloads allocate, and nothing here calls them.
- i2s reads/writes hit DMA buffers made in `begin()`.

don't trust me, build the audit variant:

```sh
pio run -e t3s3_sx1262_heapaudit -t upload
```

it links with `--wrap=malloc,calloc,realloc` and starts counting at the end
of `setup()`. every return to listen prints `heap_allocs=<n> min_free=<bytes>`;
`n` must stay 0 across talk/listen cycles. the wrapper catches c++ `new` and
codec2's `codec2_malloc` too (checked in the linked ELF).

## spectrum

the defaults are +22 dBm narrowband GFSK on 914.6 MHz. that does **not**
qualify for the FCC part 15.247 unlicensed digital-modulation allowance
(needs ≥500 kHz occupied bandwidth or hopping); unlicensed it'd fall under
15.249, whose field-strength limit is nowhere near this power. it's fine for
a licensed amateur on 33 cm. know your rules, set `FSK_FREQ_MHZ` /
`FSK_PWR_DBM` accordingly, and identify.

## lineage and licence

started from lilygo's `SX126x_Walkie_Talkie` example in the MVSRBoard repo
(GPL-3.0), which uses unsynchronised `std::vector`s across cores and a
200-byte payload. this rewrite keeps the hardware bring-up idea and replaces
the buffering, framing and receive path. the idf build touches none of
lilygo's code; the arduino build still links their GPL i2s wrapper. keep that
in mind when picking a licence downstream.

## not yet verified on hardware

both builds compile. neither this revision's sequence/PLC/jitter path nor the
esp-idf port has been on air. open an issue with the serial counters if it
misbehaves. still cursed, probably runs.
