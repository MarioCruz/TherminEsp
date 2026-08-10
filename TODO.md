# TherminEsp — TODO

Original punch-list from the 2026-08-10 code review. All 15 scoped items (Tiers 1–4 except the parking-lot ideas below) are implemented and build clean — see the README's Status section for what shipped and any caveats. This file now tracks only what's still open.

---

## Pending

### Hardware verification
Flashed and boot-verified on the physical board on 2026-08-10. Confirmed from the boot log and a short live session:
- ✅ Clean boot log, no `dvp_video` errors (1.3)
- ✅ Self-test sweeps all 7 modes and the model self-test (0.92) both still pass
- ✅ `buttons: 4 physical buttons registered` — the ADC ladder + `iot_button` wiring (3.1) initializes correctly
- ✅ Mode/scale/root/glide all cycled live (seen in the serial log) — though touch and the physical buttons log identically by design, so this alone doesn't prove which input source was used; add a source tag if that distinction matters later

Still open — needs deliberate hands-on checks, not just a boot log:
- Marker Y tracks volume (1.1)
- Touch + a tracked hand sound together, independently (1.2)
- Warm Tone has no dry/ghost-audio glitch on mode entry (1.4) — the flag-based fix (see README lesson #10) hasn't been exercised specifically
- A power-cycle to confirm settings actually persist (3.2)
- Two hands in frame → two independent voices, no identity-swap glitching when hands cross (2.4)
- Unplug the camera cable (or cover the lens at init) → the "camera off" hint appears (4.4)
- First real CI run on GitHub Actions — blocked on the GitHub token lacking `workflow` scope (see below); the workflow author also flagged specific lines likely to need a fix-up (see `.github/workflows/build.yml` comments) (4.2)

### Push blocked: GitHub token missing `workflow` scope
`.github/workflows/build.yml` is committed locally (as of `987aa2a`) but held back from `git push` — GitHub rejects OAuth app tokens without the `workflow` scope touching files under `.github/workflows/`. A `gh auth refresh -h github.com -s workflow` device-code flow was started; once authorized, add the file back and push:
```
git add .github/workflows/build.yml
git commit -m "Add CI workflow"
git push
```

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
