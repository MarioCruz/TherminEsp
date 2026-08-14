# TherminEsp — TODO

**No known bugs.** Three concurrency fixes (delay allocation race, NVS glide clamp, phase_wrap negative handling) shipped in the 2026-08-14 batch alongside the feature work below. No `TODO`/`FIXME` markers left in the source.

---

## Tier 1 — Needs hands-on play (blocked on a person, not on code)

The board boots clean and every subsystem initializes, but these behaviors can only be confirmed by actually playing the instrument. Roughly in order of "most likely to reveal something."

### 1.1 Two-hand duet
The newest and least-exercised code path (`assign_detections()` in `main/tracker.cpp`).
- Both hands in frame → two independent notes, each tracking its own hand
- **Cross your hands** and watch for identity swapping — the nearest-previous-position matching is a heuristic, and a swap would be audible as the two voices trading pitches
- Take one hand out of frame → that voice alone should fade, the other keeps playing
- Each hand now gets its own on-screen marker (coral for hand 0, teal for hand 1) — verify they track correctly

### 1.2 Settings persistence across a power-cycle
Change mode/scale/root/glide, wait ~3 s (the NVS debounce is 2 s), then pull power and reboot. All four should come back. Watch the boot log for `settings` warnings if they don't.

### 1.3 Warm Tone mode entry
Switch into Warm Tone while a note is sounding. Should get the reverb tail immediately, with **no ~150 ms ghost of a previous session's audio** — that's what the `delay_reset_pending` fix (README lesson #10) was for, and it has never been specifically exercised.

### 1.4 Touch + camera together
Track a hand, then touch-drag simultaneously → two independent notes. Release touch → the camera's note must keep playing and the display should still show the camera hand's readout (the display fight fix means the hint no longer incorrectly appears).

### 1.5 PolyBLEP audio quality
Play Clean Saw and Pad in the C5–C6 range. Listen for aliasing artifacts compared to the sine — the PolyBLEP should make the sawtooth noticeably cleaner than a naive saw would be at those frequencies.

### 1.6 Physical button long-press
While on Clean Wave mode, long-press the MODE button — it should toggle sine/sawtooth (same as the touchscreen button long-press). Short-press should still cycle modes.

### 1.7 The smaller ones
- Marker Y position visually matches the volume you hear as you raise/lower your hand
- Cover the lens or unplug the camera before boot → the "camera off — touch to play" hint appears instead of the normal hint

---

## Tier 2 — Known gaps worth closing

### ~~2.1 Touch and camera fight over the on-screen display~~ ✅ Done
Per-source presence tracking (`s_touch_active`, `s_hand0_active`, `s_hand1_active`) with a `hint_update()` helper. The hint only shows when all sources are idle.

### ~~2.2 Second on-screen marker for hand 1~~ ✅ Done
Teal marker for hand 1, coral for hand 0. Both update position independently. Readout panel still tracks hand 0 only (simpler, and hand 0 is the "primary" hand).

### 2.3 `tracker_set_tuning()` is built but unreachable
- **Where:** `main/tracker.h` / `main/tracker.cpp` (the API exists and works); nothing calls it
- **Problem:** The six tracker thresholds were made runtime-settable specifically so tuning wouldn't cost a reflash — but with no caller, tuning *still* costs a reflash. The feature is currently theoretical.
- **Fix, option A (recommended while tethered):** serial console commands via IDF's `esp_console` — least code, and lets tuning happen live from a Mac terminal over the existing UART while watching `CONFIG_THEREMIN_TRACE_FRAMES` output.
- **Fix, option B (matters once untethered):** a hidden debug screen (long-press the title, which already toggles the camera preview) with +/- steppers.
- **Either way:** persist through `main/settings.c` so a good tuning survives reboot.
- **Effort:** ~100 lines (console) / ~80 lines (screen)

---

## Tier 3 — Polish

### ~~3.1 README architecture diagram is out of date~~ ✅ Done
Updated to show all tasks (camera, LVGL/touch/UI, buttons, settings), 3 voices, both markers, PolyBLEP, duet matching.

### ~~3.2 PolyBLEP oscillators~~ ✅ Done
`saw_blep()` and `tri_blep()` replace the naive versions. Applied to Clean Wave (saw), Warm Tone (tri), and Pad (tri). Bitcrush intentionally keeps the naive aliasing saw.

### ~~3.3 Physical button long-press~~ ✅ Done
`BUTTON_LONG_PRESS_START` on `BSP_BUTTON_MODE` calls `ui_toggle_clean_wave()`.

---

## Parking lot (no commitment — pick up when the instrument feels done)

### Mic ideas
The ES8389 record path is proven (the wedding-recorder project used it). Candidates: clap-to-start attract mode, an input-level LED pulse, a chromatic tuner screen.

### SD card session recorder
Write the synth's PCM blocks to a WAV on demand — the wedding-recorder project's SD + WAV plumbing is reference code. Would make performances shareable.

### Hand keypoints instead of a bounding box
The current fist/open detection is a bounding-box-area heuristic, which is why it's the weakest gesture. If Espressif ever ships a hand-*keypoint* model for the S31 (MediaPipe-style landmarks), that would restore real finger-level fist detection — the one place TherminEsp is genuinely worse than Wavr. Worth re-checking the ESP-DL model zoo periodically.
