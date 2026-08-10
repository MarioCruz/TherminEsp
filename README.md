# TherminEsp 🎵👋

![Build](https://github.com/MarioCruz/TherminEsp/actions/workflows/build.yml/badge.svg)

**A visual theremin that runs entirely on one chip.** Wave your hand in front of the camera and the ESP32-S31 sees it, tracks it with an on-device neural network, and synthesizes music in real time — no computer, no cloud, no browser. Touch the screen and it's a playable instrument too.

TherminEsp is the fully-embedded successor to [Wavr](https://github.com/MarioCruz/Wavr), my browser-based visual theremin (MediaPipe + Web Audio on a Raspberry Pi 5). Everything Wavr did across a Pi, Chromium, and a webcam now happens inside a single microcontroller.

![TherminEsp UI on the 800×480 touchscreen](docs/screenshot-ui.png)

*The instrument's screen, captured remotely from the running board — the firmware reads the RGB panel's framebuffer and streams it over serial (`CONFIG_THEREMIN_BOOT_SCREENSHOT`).*

| | Wavr (Pi 5) | TherminEsp (ESP32-S31) |
|---|---|---|
| Hand tracking | MediaPipe Hands, in-browser JS | **espdet-pico neural net, on-chip** (~95 ms/frame) |
| Audio synthesis | Web Audio API | **Custom C DSP engine → I2S codec** (48 kHz stereo) |
| Display | Browser tab | **800×480 touchscreen, LVGL** |
| Computer required | Raspberry Pi 5 + Chromium | **None** |

## Hardware

Runs on the **ESP32-S31-Korvo-1** dev board — everything needed is on it:

- **ESP32-S31** — dual-core RISC-V @ 320 MHz + LP core, Wi-Fi 6, BT 5.4, 802.15.4, hardware JPEG codec (chip hit mass production July 2026; this project runs on rev v0.0 silicon)
- 16 MB flash + 16 MB octal PSRAM @ 250 MHz
- **OV3660 camera** on the DVP interface (720p JPEG @ 17 fps)
- **800×480 RGB LCD** with GT1151 capacitive touch
- **ES8389 audio codec** + NS4150B Class-D amp, 48 kHz 16-bit stereo I2S, stereo speakers + dual mics
- WS2812 status LED, 4 ADC buttons, SD slot

<details>
<summary><strong>Full hardware breakdown</strong> (part numbers + pin map, verified against the board's boot log and BSP)</summary>

### SoC — ESP32-S31
- Dual-core 32-bit **RISC-V** + separate **LP (low-power) core**, **320 MHz** (300 MHz rated)
- Silicon **revision v0.0** (early production stepping)
- Radios: **Wi-Fi 6** (2.4 GHz), **Bluetooth 5.4 LE**, **802.15.4** (Thread/Zigbee)
- On-die accelerators: hardware **JPEG codec** (used for the camera decode), **PPA** (pixel-processing accelerator), **2D-DMA**

### Memory & USB
- **16 MB** SPI flash (QIO @ 80 MHz)
- **16 MB** octal PSRAM @ **250 MHz** (128-Mbit octal-DDR die) — holds camera buffers + the neural-net model
- **512 KB** internal SRAM + 31 KB RTC RAM
- **Silicon Labs CP2102N** USB-to-UART bridge (the serial/flash port)

### Audio
- **Codec: ES8389** — stereo codec doing both DAC (speakers) and ADC (mics); on I²C `I2C_NUM_0` (SDA=GPIO0, SCL=GPIO1)
- **Amplifier: NS4150B** Class-D, enabled via **GPIO7** (5.0 V rail, DAC ref 3.3 V) *(part per Espressif Korvo-1 docs — enable-only, not software-readable)*
- **Speakers:** stereo, **4 Ω / 3 W** each *(product spec)*
- **Microphones:** dual analog, through the ES8389 ADC
- **I²S** (`I2S_NUM_0`, ESP is master), default **48 kHz / 16-bit / stereo**:

  | Signal | GPIO | | Signal | GPIO |
  |---|---|---|---|---|
  | MCLK | 2 | | Speaker out (SDOUT) | 5 |
  | BCLK (SCLK) | 3 | | Mic in (DSIN) | 6 |
  | LRCLK (WS) | 4 | | PA enable | 7 |

### Display & touch
- **Panel:** 800×480 **RGB LCD**, RGB565, 16-bit parallel RGB, pixel clock **26 MHz**, 3 framebuffers + tear-avoidance
- **Data lines:** GPIO8–19 + GPIO33–36 (R/G/B lanes); **PCLK=40, DE=43, HSYNC=44, VSYNC=45**; panel-init SPI CS=38, MOSI=60, SCK=61
- Backlight & display-enable are hardwired on (no GPIO)
- **Touch: GT1151** capacitive controller (reports as GT1158), shares the I²C bus (0/1)

### Camera
- **OmniVision OV3660** (3 MP, PID `0x3660`), **DVP parallel** interface
- **D0–D7 = GPIO46–53**, PCLK=54, **XCLK=55 @ 20 MHz**, VSYNC=56, HSYNC=57; SCCB config over the shared I²C (0/1)
- Modes: 1280×720 JPEG @12fps (used), 640×480 YUV @10fps, 240×240 RGB565 @24fps (rejected by the driver — see Lessons)

### Storage
- **microSD**, 4-bit SDMMC: D0–D3=GPIO20–23, CLK=24, CMD=25; active-low power/switch on **GPIO39**
- GPIO20–25 are shared with an SPI-NAND flash footprint (only one populated — this board wires the SD path)

### Misc
- **WS2812** single addressable RGB LED on **GPIO37** (GRB, RMT-driven) — the pitch-color indicator
- **4 buttons** on a resistor ladder read as one ADC input: **ADC1 CH0 = GPIO42** (thresholds ≈ 380 / 820 / 1340 / 1870 mV, S31 software ADC calibration)
- Also on the SoC but unused here: temperature sensor, v3 capacitive touch peripheral, the LP core

</details>

## How you play

| Gesture | Controls | Mapping |
|---|---|---|
| Hand left ↔ right | **Pitch** | C2 (65 Hz) → C6 (1047 Hz), log-mapped, quantized to the selected scale |
| Hand up ↕ down | **Volume** | Up = loud, down = quiet |
| Fist ✊ / open hand ✋ | **Filter** | Bounding-box area drives a lowpass sweep, 200 Hz → 8 kHz |
| No hand | Silence | ~1.2 s of grace (10 missed frames) before the note drops, so brief confidence dips don't stutter |
| Two hands | **Duet** | Each hand gets its own independent voice, matched frame-to-frame so identity mostly survives hands crossing |
| **Touchscreen** | Same pitch/volume | X = pitch, Y = volume — its own voice, so it plays alongside a tracked hand rather than fighting it. Works even with the camera covered |

On-screen: live note name, frequency, volume, and a marker that follows your (primary) hand, plus buttons to cycle through **7 synth modes** (long-press while on Clean Wave to swap sine/sawtooth), **10 musical scales**, **12 root notes**, and **4 glide presets**. The board's 4 physical buttons mirror those same four controls if you'd rather not touch the screen. The status LED shifts from coral to sky-blue as pitch rises (Beach Boys palette, carried over from Wavr). Every setting survives a reboot.

## Architecture

```
            ┌───────────────────────── ESP32-S31 ─────────────────────────┐
 OV3660     │                                                             │
 camera ──▶ │  Core 0                          Core 1                     │
 (720p      │  ├─ camera task (V4L2 dequeue)   ├─ synth task (prio MAX-3) │
  JPEG,     │  ├─ LVGL / touch / UI            │    7 modes · 10 scales   │
  17 fps)   │  └─ latest-frame mailbox ──────▶ │    biquad filter · glide │
            │                                  │    48 kHz blocks ──▶ I2S │──▶ ES8389 ──▶ 🔊
            │       tracker task (prio 5, core 1)                         │
            │       HW JPEG decode → RGB888 → 224×224 downscale           │
            │       → espdet-pico inference (~95 ms)                      │
            │       → EMA smoothing + hysteresis                          │
            │       → pitch / volume / filter → synth voice               │
            └─────────────────────────────────────────────────────────────┘
```

### The synth engine (`main/synth.c`)

A sample-accurate C port of Wavr's Web Audio graph. Phase-accumulator oscillators render 5 ms blocks at 48 kHz; a blocking codec write paces the loop off the I2S DMA clock.

- **FM Synth** — sine carrier, 2× modulator, index tracks pitch (the classic electro-theremin)
- **Clean Wave** — pure sine
- **Warm Tone** — triangle + 150 ms feedback delay with soft-clip (stands in for Web Audio's compressor)
- **Pad** — three detuned oscillators (±0.5%)
- **Theremin** — sine with 5.5 Hz vibrato scaled to pitch
- **Organ** — additive 1st + 2nd + 3rd harmonics
- **Bitcrush** — sawtooth through an 8-step staircase quantizer

All modes share an RBJ lowpass biquad (Q=2) for the fist/open filter, linear parameter ramps to prevent clicks, scale quantization across 3 octaves, and glide (portamento). A boot self-test sweeps every mode C2→C6 through the codec — if you have speakers connected you get a little demo at every power-on.

> 📖 **[Full synth engine deep-dive →](docs/synth-engine.md)** — the render task, DDS oscillators, the biquad, the pitch/scale pipeline, parameter smoothing, polyphony, the concurrency model, and the latency budget, with the math for every mode.

### The hand tracker (`main/tracker.cpp`)

- Espressif's **[hand_detect](https://components.espressif.com/components/espressif/hand_detect)** component (espdet-pico, 224×224 input) via **ESP-DL** — the S31 runs the RISC-V (P4) model at ~95 ms/inference, 7–8 fps
- Camera JPEG → **S31 hardware JPEG decoder** straight to RGB888 → nearest-neighbor downscale with a vertical flip (the camera module mounts upside down)
- **Up to two hands per frame** (top-2 detections by score), each matched to a persistent voice slot by nearest previous position — a small greedy assignment that keeps identity stable frame-to-frame, including a straight-vs-crossed comparison when both hands are already tracked
- EMA smoothing (α=0.5) on position and openness; mirrored X so moving right raises pitch; only the reliable center band of the narrow FOV maps to the full control range (see `ACTIVE_MARGIN`)
- All of the above is runtime-tunable (`tracker_set_tuning()`) instead of baked into `#define`s — a tuning pass no longer costs a reflash
- **Built-in model self-test**: at boot the detector runs on an embedded known-good photo of two hands ([source: Wikimedia Commons, "Human-Hands-Front-Back.jpg"](https://commons.wikimedia.org/wiki/File:Human-Hands-Front-Back.jpg)) and logs the score — instantly separates "model broken" from "camera image bad" forever after

## Building it

You need **ESP-IDF v6.1-beta1** (the ESP32-S31 is a `--preview` target — MicroPython and Arduino don't support this chip yet, as of Aug 2026) and the **esp-dev-kits** repo checked out as a sibling for the Korvo board-support package:

```
GitHub/
├── TherminEsp/                  ← this repo
└── ESPressif/
    └── esp-dev-kits/            ← provides the esp32_s31_korvo BSP
```

```bash
source <idf-install>/activate_idf_v6.1-beta1.sh
cd TherminEsp
idf.py --preview set-target esp32s31
idf.py build
idf.py -p /dev/cu.usbserial-110 flash monitor
```

The first build downloads the managed components (ESP-DL, hand_detect, LVGL, esp_video, esp_codec_dev, …). The hand-detection model is embedded in the app image — which is why `partitions.csv` gives the factory app **8 MB**.

## Project structure

```
main/
├── main.c            boot sequence + synth self-test
├── synth.c/.h        the 7-mode DSP engine (port of Wavr's audio-engine.js)
├── tracker.cpp/.h    JPEG decode → espdet-pico → multi-hand gesture mapping
├── camera.c/.h       V4L2 capture task (720p JPEG)
├── ui.c/.h           LVGL touchscreen UI + hidden camera debug view
├── buttons.c/.h      the board's 4 physical buttons, same controls as the UI
├── settings.c/.h     NVS persistence (mode/scale/root/glide/waveform)
└── testhand_224.rgb  embedded model self-test image
```

## Hard-won lessons (read before hacking)

Things this board taught me the hard way — each one cost a debug cycle:

1. **The DVP driver rejects small RGB565 modes.** `CONFIG_CAMERA_OV3660_DVP_RGB565_BE_240X240_24FPS` exists in Kconfig, but esp-video refuses it at S_FMT (`input width=240, height=240 is not supported`). 1280×720 JPEG is the only mode that streams — decode it with the S31's hardware JPEG engine (it's fast).
2. **The HW JPEG decoder's RGB565 output is byte-swapped** relative to little-endian `uint16_t` reads (this board's pipeline is RGB565_BE end-to-end). Symptom: psychedelic rainbow contours. Fix: decode straight to `JPEG_DECODE_OUT_FORMAT_RGB888` and skip the ambiguity.
3. **The camera mounts upside down** and the V4L2 flip controls are stubbed on this BSP — flip in software.
4. **hand_detect officially supports the S31** even though its README table only lists S3/P4 — the component's CMakeLists maps `esp32s31` to the P4 model. Measured ~95 ms/inference.
5. **The model in flash rodata pushes the app past 4 MB** — grow the factory partition (16 MB flash, use it).
6. **Wi-Fi doesn't fit** in internal RAM alongside camera + LVGL on this chip today (same wall the previous firmware on this board hit) — hence the LCD-native UI instead of a web dashboard.
7. **IDF's C++ flags (`gnu++26 -Werror`)** reject partially-designated initializers on driver config structs — `= {}` then assign fields.
8. **Embed a known-good test input for any on-device ML.** The boot self-test (2 hands @ 0.92 on the reference photo) turns "why doesn't it detect?" from a mystery into a one-line answer: it's your camera image, not the model.
9. **Remote screenshots without a debugger:** `lv_snapshot_take()` deadlocks against the esp_lvgl_adapter's render task — read the RGB panel's framebuffers directly instead (`esp_lcd_rgb_panel_get_frame_buffer`). With triple buffering, dump all three and keep the complete one; base64 over the console UART is slow (~90 s/frame at 115200) but needs zero extra hardware. Enable with `CONFIG_THEREMIN_BOOT_SCREENSHOT` in menuconfig.
10. **A caller thread should never memset a buffer the render task might be mid-read on.** The Warm Tone delay-line fix (1.4) originally cleared the buffer with a plain `memset()` from whichever thread called `synth_set_mode()` — but the render task reads/writes that same buffer every sample, lock-free, on another core, for performance. A mode switch landing mid-block could race the two writers. Fix: the caller only sets a `delay_reset_pending` flag under the lock; the render task clears the buffer itself, on its own thread, at the top of the block where it starts using it — no cross-thread access to the content at all.
11. **Widen the lock to cover the whole read-modify-write, not just the final write.** `ui_cycle_mode/scale/root/glide()` can now be entered from two different tasks (the touchscreen's LVGL task and the physical buttons' iot_button task). Each one reads a current value, computes the next one, and writes it back — locking only the label update at the end left that read-modify-write exposed to a lost-update race (worst offender: `s_glide_idx`, a plain unsynchronized `int`). Fix: take the display lock — already a recursive mutex — around the *entire* function body, not just the LVGL calls at the end. Free when already called from the LVGL task (just a refcount bump); a real, correct serialization the rest of the time.

## Status

- ✅ Synth engine — all 7 modes verified on hardware, every boot; root note, glide (4 presets), and Clean Wave's sine/sawtooth are all playable now, not just wired into the engine
- ✅ Touch play — the screen is a playable instrument, on its own dedicated voice so it never fights a tracked hand for the same note
- ✅ Live hand detection — working end to end (camera aim + lighting were the last blocker; solved by propping the board so the camera faces the player, not the ceiling)
- ✅ **Multi-hand duet** — up to two tracked hands drive two independent voices, matched frame-to-frame by nearest previous position so identity mostly survives hands crossing
- ✅ Physical controls — the board's 4-button ADC ladder cycles mode/scale/root/glide, same as the touchscreen buttons
- ✅ Settings persistence — mode/scale/root/glide/waveform survive a reboot (NVS, debounced writes)
- ✅ Tracker thresholds are runtime-tunable (`tracker_set_tuning()`) instead of reflash-to-adjust — nothing calls it yet (console commands or a debug screen are the natural next step)
- ✅ CI — GitHub Actions builds the firmware on every push ([workflow](.github/workflows/build.yml))
- 🔧 **The 2026-08-10 batch above builds clean but hasn't been flashed to hardware yet** (board was disconnected) — see **[TODO.md](TODO.md)** for the verification checklist and what's still genuinely open (a couple of parking-lot ideas, dual-hand UI, live tuning UI)

## License

MIT — see [LICENSE](LICENSE). Same license as Wavr.

## Credits

- [Wavr](https://github.com/MarioCruz/Wavr) — the original browser theremin this ports
- Espressif's [ESP-DL](https://github.com/espressif/esp-dl), [hand_detect](https://components.espressif.com/components/espressif/hand_detect), [esp-video](https://components.espressif.com/components/espressif/esp_video), and the ESP32-S31-Korvo BSP from [esp-dev-kits](https://github.com/espressif/esp-dev-kits)
- Test image: ["Human-Hands-Front-Back.jpg"](https://commons.wikimedia.org/wiki/File:Human-Hands-Front-Back.jpg) via Wikimedia Commons (public domain, by Evan-Amos)
- Built with [Claude Code](https://claude.com/claude-code) — Claude helped integrate Espressif's [hand_detect](https://components.espressif.com/components/espressif/hand_detect) component: confirming ESP32-S31 support (the component's CMake maps the S31 to its RISC-V/P4 model even though the docs don't list it), wiring the espdet-pico detector into the camera pipeline, and adding the embedded-image model self-test that proved the neural net runs correctly on this chip
