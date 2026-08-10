# TherminEsp — TODO

**No known bugs.** Everything two review passes turned up is fixed, built clean, and flashed (`880dcce`). No `TODO`/`FIXME` markers left in the source. What follows is verification that needs a human, one deliberately-deferred defect, and things worth building next.

---

## Tier 1 — Needs hands-on play (blocked on a person, not on code)

The board boots clean and every subsystem initializes, but these behaviors can only be confirmed by actually playing the instrument. Roughly in order of "most likely to reveal something."

### 1.1 Two-hand duet
The newest and least-exercised code path (`assign_detections()` in `main/tracker.cpp`).
- Both hands in frame → two independent notes, each tracking its own hand
- **Cross your hands** and watch for identity swapping — the nearest-previous-position matching is a heuristic, and a swap would be audible as the two voices trading pitches
- Take one hand out of frame → that voice alone should fade, the other keeps playing

### 1.2 Settings persistence across a power-cycle
Change mode/scale/root/glide, wait ~3 s (the NVS debounce is 2 s), then pull power and reboot. All four should come back. Watch the boot log for `settings` warnings if they don't.

### 1.3 Warm Tone mode entry
Switch into Warm Tone while a note is sounding. Should get the reverb tail immediately, with **no ~150 ms ghost of a previous session's audio** — that's what the `delay_reset_pending` fix (README lesson #10) was for, and it has never been specifically exercised.

### 1.4 Touch + camera together
Track a hand, then touch-drag simultaneously → two independent notes. Release touch → the camera's note must keep playing. (Note the known display bug in 2.1 below will make the *screen* lie here even when the *audio* is correct.)

### 1.5 The smaller ones
- Marker Y position visually matches the volume you hear as you raise/lower your hand
- Cover the lens or unplug the camera before boot → the "camera off — touch to play" hint appears instead of the normal hint

---

## Tier 2 — Known gaps worth closing

### 2.1 Touch and camera fight over the on-screen display
- **Where:** `main/ui.c` — `ui_hand_update()` (camera path) and `surface_event_cb()` (touch path)
- **Problem:** Both drive the *same* `s_hint_label` visibility and `s_marker`. Whichever wrote last wins, so **releasing touch while a hand is still camera-tracked shows "touch to play" even though the camera's voice is still sounding** — the display contradicts the audio. Not a crash, not a lost note, just wrong.
- **Fix:** Track presence per source rather than "last writer wins" — hide the hint if (camera hand present **OR** touch active), show it only when both are idle. Naturally bundles with 2.2 since both touch the same state.
- **Effort:** ~30 lines

### 2.2 Second on-screen marker for hand 1
- **Where:** `main/ui.c`, `main/ui.h`, `main/tracker.cpp` (the `slot == 0` guard in the detection loop)
- **Problem:** Both tracked hands *sound*, but only hand 0 gets a marker and the note/freq/volume readouts. A deliberate scope cut to keep the duet change reviewable — now worth finishing.
- **Fix:** A second marker object (different color — the Beach Boys palette's turquoise pairs with the existing coral), and a decision about the readout panel: either show two compact rows, or keep the big readout for hand 0 and give hand 1 just a marker. The second option is far less screen surgery.
- **Effort:** ~50 lines, plus a layout judgment call

### 2.3 `tracker_set_tuning()` is built but unreachable
- **Where:** `main/tracker.h` / `main/tracker.cpp` (the API exists and works); nothing calls it
- **Problem:** The six tracker thresholds were made runtime-settable specifically so tuning wouldn't cost a reflash — but with no caller, tuning *still* costs a reflash. The feature is currently theoretical.
- **Fix, option A (recommended while tethered):** serial console commands via IDF's `esp_console` — least code, and lets tuning happen live from a Mac terminal over the existing UART while watching `CONFIG_THEREMIN_TRACE_FRAMES` output.
- **Fix, option B (matters once untethered):** a hidden debug screen (long-press the title, which already toggles the camera preview) with +/- steppers.
- **Either way:** persist through `main/settings.c` so a good tuning survives reboot.
- **Effort:** ~100 lines (console) / ~80 lines (screen)

---

## Tier 3 — Polish

### 3.1 README architecture diagram is out of date
The ASCII diagram predates the buttons and settings tasks and doesn't show the duet driving two separate voices. Not wrong so much as incomplete. Cosmetic, but it's the first thing a reader looks at.

### 3.2 PolyBLEP oscillators
`saw()` and `tri()` in `main/synth.c` are naive and alias in the top octave. Harmless across the C2–C6 playing range and arguably on-brand for Bitcrush, but a PolyBLEP upgrade would clean up Clean Saw and Pad noticeably. See `docs/synth-engine.md` §13.

### 3.3 Physical button long-press
The touchscreen mode button long-presses to toggle Clean Wave's sine/saw. The physical buttons only do short-press cycling, so that toggle is touchscreen-only — `iot_button` supports long-press events, so this is a small addition to `main/buttons.c`.

---

## Parking lot (no commitment — pick up when the instrument feels done)

### Mic ideas
The ES8389 record path is proven (the wedding-recorder project used it). Candidates: clap-to-start attract mode, an input-level LED pulse, a chromatic tuner screen.

### SD card session recorder
Write the synth's PCM blocks to a WAV on demand — the wedding-recorder project's SD + WAV plumbing is reference code. Would make performances shareable.

### Hand keypoints instead of a bounding box
The current fist/open detection is a bounding-box-area heuristic, which is why it's the weakest gesture. If Espressif ever ships a hand-*keypoint* model for the S31 (MediaPipe-style landmarks), that would restore real finger-level fist detection — the one place TherminEsp is genuinely worse than Wavr. Worth re-checking the ESP-DL model zoo periodically.
