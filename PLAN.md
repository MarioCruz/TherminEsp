# TherminEsp — Wavr, but self-contained on an ESP32-S31

Port the [Wavr](https://github.com/MarioCruz/Wavr) visual theremin from "Pi 5 + browser does everything" to a fully on-device instrument: the ESP32-S31's camera watches your hand, the chip runs hand detection and synthesizes audio itself, and any phone/laptop on the LAN gets the Beach-Boys dashboard.

## Findings (researched 2026-08-07)

### The connected board
- **Chip:** ESP32-S31, revision v0.0 — dual-core RISC-V @ 300 MHz + LP core, Wi-Fi 6, BT 5.4 LE, 802.15.4
- **Memory:** 16 MB flash, PSRAM present (confirmed via eFuse), 512 KB SRAM on-die
- **Multimedia hardware:** DVP camera interface (8–16 bit), hardware JPEG codec, PPA, 2D-DMA, LCD out
- **USB bridge:** Silicon Labs CP2102N on `/dev/cu.usbserial-110`, MAC `30:ed:a0:f3:de:74`
- Chip entered **mass production 2026-07-27** — two weeks old; ours is rev v0.0 (early silicon)

### Software support status (as of Aug 2026)
| Stack | Status |
|---|---|
| **ESP-IDF** | Preview target since v5.5.5; best in **v6.1-beta1 — already installed on this Mac** (`source ~/.espressif/tools/activate_idf_v6.1-beta1.sh`), lists `esp32s31` under `--preview` |
| **MicroPython** | **Not supported yet** — S31 absent from the esp32 port's chip list |
| **Arduino core** | **Not supported yet** — open feature request espressif/arduino-esp32#12485, "needs investigation" |
| **Camera** | `esp-video` / esp32-camera component supports S31 DVP; requires PSRAM (we have it) |
| **AI vision** | **ESP-DL `hand_detect` (espdet-pico) officially supports ESP32-S31** — real-time hand bounding-box detection on-device |
| **esptool** | v5.3.1 (installed) detects and flashes the S31 fine |

**Conclusion: the project must be C/C++ on ESP-IDF v6.1-beta1.** No Python on the chip for now; revisit when MicroPython lands.

## Architecture

```
             ┌────────────────────── ESP32-S31 ──────────────────────┐
 OV2640/     │  Core 1: capture → espdet-pico hand bbox → gesture    │
 OV3660 ──▶  │          mapper (x→pitch, y→volume, area→filter)      │
 (DVP)       │  Core 0: synth engine (FM/DDS, scales, glide,         │
             │          biquad filter) → I2S @ 44.1 kHz              │──▶ MAX98357A ──▶ 🔊
             │  Wi-Fi 6: async HTTP + WebSocket                      │
             │   ├─ dashboard (ported Wavr UI, LittleFS)             │
             │   ├─ telemetry: note/freq/vol/filter                  │
             │   └─ MJPEG preview via hardware JPEG encoder          │
             └───────────────────────────────────────────────────────┘
```

### Gesture mapping vs Wavr
| Wavr (MediaPipe, 21 landmarks) | TherminEsp (espdet-pico, bounding box) |
|---|---|
| Hand X → pitch | bbox center X → pitch (same) |
| Hand Y → volume | bbox center Y → volume (same) |
| Fingertip-distance openness → filter | **bbox area/aspect heuristic** (open hand ≈ bigger box); stretch goal: hand-keypoint model when ESP-DL ships one for S31 |
| Multi-hand duet | espdet returns multiple boxes — feasible if FPS holds |

## Hardware decision — RESOLVED (2026-08-07)
The connected board **is an ESP32-S31-Korvo-1** (confirmed from the boot log of the firmware running on it). Everything needed is already on it:
- **OV3660 camera on DVP** — modes in the BSP: 1280×720 JPEG @ 12 fps, **240×240 RGB565 @ 24 fps** (ideal espdet-pico input, no JPEG decode), 640×480 YUV422 @ 10 fps
- **800×480 RGB LCD + GT1151 capacitive touch** — the theremin UI can be *local LVGL on the board itself*; the web dashboard becomes optional
- **ES8389 codec, I2S 48 kHz 16-bit stereo**, speakers + mics — synth output ready
- 16 MB flash + 16 MB octal PSRAM @ 250 MHz, SD slot (16 GB card inserted), WS2812 LED (GPIO37), 4 ADC buttons

**Proven init code exists:** Mario's `~/GitHub/ESPressif/wedding-recorder` project (currently flashed on the board) plus the `esp32_s31_korvo` BSP at `~/GitHub/ESPressif/esp-dev-kits/examples/esp32-s31-korvo/examples/common_components/esp32_s31_korvo` already bring up camera, codec, LCD, touch, SD, and buttons on IDF v6.1-beta1. Reuse the BSP and crib the working patterns wholesale.

⚠️ Flashing TherminEsp replaces `wedding_recorder` on the board — its exact build is preserved at `wedding-recorder/build/` (ELF SHA verified matching), so it's one `idf.py flash` to restore. The SD card holds 36 recorded wedding messages — don't reformat it.

## Phases — status 2026-08-08 (built overnight)
0. ✅ **Toolchain smoke test** — built/flashed on IDF v6.1-beta1, PSRAM 16 MB detected, LCD/LED/codec all init clean
1. ✅ **Synth engine** (`main/synth.c`) — all 7 modes + 10 scales + glide + biquad ported; boot self-test sweeps every mode C2→C6 on hardware, all pass
2. ✅ **UI** (`main/ui.c`) — went LCD-native instead of web (Wi-Fi doesn't fit RAM alongside camera+graphics): Beach Boys palette, touch play surface (X=pitch Y=volume), note/freq/vol readouts, mode + scale cycle buttons
3. ✅ **Camera** (`main/camera.c`) — 240×240 RGB565 rejected by the DVP driver (`input width=240 height=240 not supported`); runs 1280×720 JPEG at 17 fps with S31 hardware JPEG decode. Partition grown to 8 MB for the in-rodata model
4. ✅ **Hand tracking** (`main/tracker.cpp`) — espdet-pico (S31 uses the P4 RISC-V model per the component's CMake) at **7–8 fps, ~100 ms/inference**; EMA smoothing, 4-frame miss hysteresis, bbox-area → filter openness; drives synth voice 0 + on-screen marker + LED hue
5. ⏳ **Camera image quality + human verification** — the ONLY open issue. Debug session 2026-08-08 ~00:40:
   - Model self-test (embedded known-good photo, `main/testhand_224.rgb`): **2 hands, score 0.92 — model + full pipeline proven on S31**
   - Live camera frames captured off the board were dim, low-contrast, badly out of focus → detector correctly finds nothing
   - Two capture bugs found & fixed on the way: frames arrive upside down (camera mounts inverted; sensor flip controls stubbed — flipped in software during downscale, same as wedding recorder), and the HW JPEG decoder was switched to direct RGB888 output after RGB565 byte-order garbage (rainbow-contour frame dump)
   - Live preview of the detector input now drawn bottom-left of the play surface for instant visual debugging
   - **To do (physical)**: good room light on the scene, peel any lens film, screw-adjust the OV3660 lens focus until the preview is sharp, hand 1–2 ft away; then connect speakers and judge feel; then tune AREA_CLOSED/AREA_OPEN and consider multi-hand

## Risks
- **Preview-level IDF target:** some S31 drivers/examples may be incomplete or unbuildable — file issues, track ESP-IDF v6.1 stable
- **Inference FPS unknown on S31:** if detection drops below ~10 fps the instrument feels laggy → lower resolution, parameter smoothing; audio synthesis runs independently so sound never stutters
- **Openness heuristic** is weaker than MediaPipe's landmark-based fist detection — acceptable degradation, keypoint model is the stretch fix
- **Rev v0.0 silicon** may carry errata fixed in production stepping
