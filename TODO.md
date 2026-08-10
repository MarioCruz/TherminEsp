# TherminEsp — TODO

Ranked punch-list from the 2026-08-10 code review. Each item says what's wrong, where, how to fix it, and roughly how big the change is. Items within a tier are ordered by feel-improvement per line of code.

---

## Tier 1 — Bugs (small, high value)

### 1.1 Marker Y doesn't match the volume you hear
- **Where:** `main/tracker.cpp`, the `ui_hand_update(...)` call in `tracker_task`
- **Problem:** After the center-band remap landed, X passes the *remapped* `freq_norm` but Y still passes raw `ema_y`, while the audible volume uses `remap_active(1 - ema_y)`. The on-screen dot's height no longer corresponds to the volume mapping.
- **Fix:** Pass `1.0f - vol` as the Y argument so the marker reflects exactly what the synth was told. One line.
- **Verify:** Hand at mid-frame → marker vertically centered; raise hand → marker and volume % move together.
- **Effort:** ~1 line

### 1.2 Camera and touch fight over voice 0
- **Where:** `main/ui.c` (`surface_event_cb`), `main/tracker.cpp` (`tracker_task`)
- **Problem:** Both controllers call `synth_update_voice(0, …)`. Touching while the camera tracks makes them overwrite each other several times a second; releasing the touch calls `synth_stop_voice(0)` and kills the camera's note.
- **Fix:** Touch drives **voice 1**, camera keeps voice 0. The engine is already 2-voice and auto-scales the mix (`voice_count` logic in `synth_update_voice`). Update the two `synth_update_voice(0/…)` and `synth_stop_voice(0)` calls in `ui.c` to use `1`.
- **Bonus:** This makes camera + touch a two-voice duet for free.
- **Verify:** Track a hand, then touch-drag simultaneously — two independent notes; release touch — camera note keeps playing.
- **Effort:** ~3 lines

### 1.3 Boot spams 5 error lines for the known-dead 240×240 mode
- **Where:** `main/camera.c`, `camera_start()`
- **Problem:** Every boot first tries 240×240 RGB565, which this DVP driver rejects (proven), printing `E`-level noise that looks like a failure before falling back to 720p JPEG.
- **Fix:** Open 1280×720 JPEG directly. Keep the RGB565 attempt behind a comment (or a Kconfig probe option) documenting *why* it's skipped, so the knowledge isn't lost when a future esp-video release adds small-mode support.
- **Verify:** Clean boot log — no `dvp_video`/`S_FMT` errors.
- **Effort:** ~10 lines

### 1.4 Warm Tone delay-line edge cases
- **Where:** `main/synth.c` (`synth_update_voice` alloc block, `voice_sample` WARM case, `synth_set_mode`)
- **Problem A:** The 150 ms delay buffer is only allocated inside `synth_update_voice` *when the mode is already WARM*. Switching to WARM mid-note (mode button) with no subsequent update call leaves `v->delay` NULL → mode silently plays dry.
- **Problem B:** The buffer keeps stale audio; re-entering WARM replays a ~150 ms ghost of the previous session.
- **Fix:** (A) allocate in `synth_set_mode` when entering WARM for any active voice (alloc outside the critical section, publish inside — same pattern as today). (B) `memset` the buffer and reset `delay_pos` in `synth_set_mode` when entering WARM.
- **Verify:** Mode-cycle into Warm Tone while holding a note → reverb tail appears immediately, no ghost audio.
- **Effort:** ~15 lines

### 1.5 Oversized JPEG frames dropped with zero diagnostics
- **Where:** `main/tracker.cpp`, `tracker_on_frame()`
- **Problem:** Frames larger than `JPEG_IN_MAX` (512 KB) are silently skipped. Camera V4L2 buffers are 900 KB, so a high-detail scene could starve tracking with no log trail.
- **Fix:** Rate-limited warning (e.g. log first occurrence + every 100th) with the frame size. Optionally bump `JPEG_IN_MAX` to 768 KB — PSRAM is cheap here.
- **Verify:** Artificial repro is hard; the log line existing is the point.
- **Effort:** ~6 lines

---

## Tier 2 — Wavr feature parity (engine done, UI missing)

### 2.1 Root-note selector
- **Where:** `main/ui.c`; engine hook `synth_set_root()` already exists (default C3 = MIDI 48)
- **Fix:** A third cycle-button in the metrics panel ("Root: C3") stepping through the 12 pitch classes (C…B). Cycle midi 48–59; label from the same name table as `synth_note_name`.
- **Effort:** ~25 lines

### 2.2 Glide control
- **Where:** `main/ui.c`; engine hook `synth_set_glide()` exists (locked at 80 ms today)
- **Fix:** Cycle-button through presets — Snap (0 s) / Fast (0.08) / Slow (0.3) / Drift (0.8) — matching Wavr's slider range without needing an LVGL slider. (A slider works too; presets are less screen space.)
- **Effort:** ~20 lines

### 2.3 Clean-mode waveform choice (sine/sawtooth)
- **Where:** `main/synth.h` (API), `main/synth.c` (`voice_sample` CLEAN case), `main/ui.c`
- **Problem:** Wavr's Clean mode had a sine/sawtooth toggle; the C port hardcodes sine.
- **Fix:** `synth_set_clean_wave(bool saw)`; CLEAN case picks `sinf(ph1)` or `saw(ph1)`. UI: tapping the mode button while already on Clean Wave toggles the waveform (label "Clean Sine"/"Clean Saw") — no new button needed.
- **Effort:** ~20 lines

### 2.4 Multi-hand duet
- **Where:** `main/tracker.cpp`
- **Problem:** The detector already returns multiple boxes; the tracker keeps only the best one. Voice 1 sits unused (until 1.2 gives it to touch — duet then needs voice assignment logic).
- **Fix:** Take the top two boxes by score; associate them to voices by X-position continuity (left hand ↔ lower voice keeps identity when hands cross is *not* guaranteed — nearest-to-previous-EMA matching is enough). Per-voice EMA + miss counters (turn the scalars into a 2-element struct array).
- **Watch out:** with 1.2 done, decide precedence — e.g. camera uses voices 0+1, touch borrows voice 1 only when only one hand is tracked.
- **Effort:** ~60 lines, the only Tier-2 item with real design decisions

---

## Tier 3 — Hardware sitting unused

### 3.1 Physical buttons → mode/scale/root/glide
- **Where:** new `main/buttons.c`; BSP already exposes the 4-button ADC ladder (`bsp/button.h`, GPIO42/ADC1-CH0, thresholds calibrated)
- **Fix:** `iot_button` callbacks: BTN0 = mode cycle, BTN1 = scale cycle, BTN2 = root cycle, BTN3 = glide cycle. Reuse the exact cycle functions the UI buttons call (factor them out of `ui.c` first so both paths share one implementation).
- **Effort:** ~50 lines

### 3.2 NVS persistence of settings
- **Where:** new `main/settings.c`; hooks in the cycle functions
- **Fix:** Persist mode/scale/root/glide (+ clean waveform) to NVS, debounced (e.g. commit 2 s after last change to spare flash). Load in `app_main` before `ui_init` so the UI labels start correct. The `nvs` partition already exists in `partitions.csv`.
- **Effort:** ~60 lines

### 3.3 Mic ideas (parking lot — no commitment)
- ES8389 record path is proven (wedding recorder used it). Candidates: clap-to-start attract mode; input-level LED pulse; a chromatic tuner screen. None blocks anything; pick one when the instrument feels done.

### 3.4 SD card ideas (parking lot)
- Session recorder: write the synth's PCM blocks to WAV on demand — the wedding recorder's SD + WAV plumbing is reference code. Would make performances shareable.

---

## Tier 4 — Repo & robustness

### 4.1 LICENSE file — **do first, it's 2 minutes**
- **Problem:** Public repo, no LICENSE = all-rights-reserved by default; README implies openness, Wavr is MIT.
- **Fix:** Add MIT LICENSE (match Wavr), name Mario Cruz, year 2026. Mention in README credits.

### 4.2 CI build check
- **Fix:** GitHub Actions workflow: `espressif/esp-idf-ci-action` pinned to the IDF v6.1 container, `idf.py --preview set-target esp32s31 && idf.py build`. Caveats: needs the esp-dev-kits BSP checked out as sibling (add a checkout step for `espressif/esp-dev-kits` at the right path) and the S31 preview target must exist in the container tag chosen — verify before trusting the badge.
- **Effort:** ~40 lines of YAML + one debugging round

### 4.3 Runtime-tunable tracker thresholds
- **Problem:** `SCORE_THR`, `AREA_CLOSED/OPEN`, `ACTIVE_MARGIN`, `MISS_LIMIT`, `EMA_ALPHA` are compile-time; every tuning round tonight cost a full flash cycle.
- **Fix (minimal):** move them to a `tracker_tuning_t` struct with a `tracker_set_tuning()`; add a hidden debug screen (long-press the title?) with +/- steppers. Persist via 3.2.
- **Fix (nicer, later):** serial console commands — IDF `esp_console` on the UART — so tuning happens live from the Mac without touching the board.
- **Effort:** minimal ~40 lines; console version ~100

### 4.4 On-screen camera-failure indicator
- **Problem:** If `tracker_init`/`camera_start` fails, the only evidence is a serial log line; the UI looks normal and a user just thinks tracking is broken.
- **Fix:** `ui_set_camera_state(bool ok)` — small gray "camera off — touch to play" chip in the hint area when the pipeline didn't come up.
- **Effort:** ~15 lines

---

## Suggested batches

| Batch | Items | Rough size |
|---|---|---|
| **A — quick wins** (one sitting) | 4.1, 1.1, 1.2, 1.3, 1.5 | ~25 lines + LICENSE |
| **B — Wavr parity** | 2.1, 2.2, 2.3, 1.4 | ~80 lines |
| **C — physical UX** | 3.1, 3.2, 4.4 | ~125 lines |
| **D — duet** | 2.4 (after B) | ~60 lines |
| **E — tooling** | 4.2, 4.3 | independent, anytime |

Done items should move to the README's Status section rather than accumulating here.
