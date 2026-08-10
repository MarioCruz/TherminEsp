# TherminEsp — TODO

Original punch-list from the 2026-08-10 code review. All 15 scoped items (Tiers 1–4 except the parking-lot ideas below) are implemented and build clean — see the README's Status section for what shipped and any caveats. This file now tracks only what's still open.

---

## Pending

### Hardware verification
Everything below built clean (`idf.py build`, zero warnings/errors in project code) but has **not yet been flashed and verified on the physical board** — it was disconnected when this batch was implemented. Needs, in order: flash, watch the boot log (self-test sweep, settings restore, button init, camera/tracker init), then hands-on checks:
- Marker Y tracks volume (1.1)
- Touch + a tracked hand sound together, independently (1.2)
- Clean boot log, no `dvp_video` errors (1.3)
- Warm Tone has no dry/ghost-audio glitch on mode entry (1.4)
- Root/glide/clean-wave buttons — touchscreen AND the 4 physical buttons — plus a power-cycle to confirm settings actually persist (2.1, 2.2, 2.3, 3.1, 3.2)
- Two hands in frame → two independent voices, no identity-swap glitching when hands cross (2.4)
- Unplug the camera cable (or cover the lens at init) → the "camera off" hint appears (4.4)
- First real CI run on GitHub Actions — the workflow author flagged specific lines likely to need a fix-up (see `.github/workflows/build.yml` comments) (4.2)

### 4.3 nicer version — live tuning without a reflash
The tunable-threshold struct (`tracker_set_tuning()`) exists but nothing calls it yet. Two ways to actually use it:
- Serial console commands via IDF's `esp_console` component, so tuning happens live from a Mac terminal over the existing UART — no new UI needed.
- A hidden on-device debug screen (long-press the title?) with +/- steppers, persisted through `settings.c`.
Console is less code and more useful mid-development; the on-device screen matters more once the board isn't tethered to a laptop.

---

## Parking lot (no commitment — pick up when the instrument feels done)

### Mic ideas
ES8389 record path is proven (the wedding-recorder project used it). Candidates: clap-to-start attract mode, an input-level LED pulse, a chromatic-tuner screen.

### SD card ideas
Session recorder: write the synth's PCM blocks to a WAV file on demand. The wedding-recorder project's SD + WAV plumbing is reference code. Would make performances shareable.

### Dual-hand / touch-plus-camera UI
Multi-hand duet (2.4) drives two independent voices, but only hand 0 gets an on-screen marker/note/freq/volume display — a deliberate scope cut to keep that change reviewable. The known effect goes a bit further than "which hand's numbers show": `ui_hand_update()` (camera) and `surface_event_cb()` (touch) both drive the *same* hint-label/marker visibility state, so releasing touch while a hand is still camera-tracked shows "touch to play" even though the camera's voice keeps sounding — the display can say nothing is happening while it audibly isn't true. Not a crash, not a lost note, just a wrong hint. A real fix needs the hint/marker to track "is voice 0 active OR voice 2 (touch) active" independently rather than "whichever path wrote last," alongside the second marker for hand 1.
