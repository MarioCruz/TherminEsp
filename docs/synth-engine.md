# TherminEsp Synth Engine — Technical Deep-Dive

The synth engine (`main/synth.c`, `main/synth.h`) is a real-time software synthesizer that turns gesture parameters into 48 kHz stereo audio, entirely in C on the ESP32-S31's CPU. It's a from-scratch port of Wavr's [`audio-engine.js`](https://github.com/MarioCruz/Wavr/blob/main/static/js/audio-engine.js), which was built on the Web Audio API — a graph of `OscillatorNode` / `BiquadFilterNode` / `GainNode` objects. There is no such graph on a microcontroller, so every node became explicit per-sample math inside one render loop.

This document explains how it works, from the output backend up through the DSP.

---

## 1. Signal path at a glance

```
gesture params (freq_norm, vol_norm, openness)   ← camera tracker or touch UI
        │  synth_update_voice()  [caller's thread]
        ▼
   per-voice targets (freq_tgt, gain_tgt, cutoff_tgt)
        │
        ▼  synth_task()  [dedicated render task, core 1]
   ┌─────────────────────────────────────────────┐
   │ every 5 ms block (240 frames):              │
   │   ramp params toward targets                │
   │   for each active voice:                    │
   │     oscillator (DDS) → biquad LPF → × gain  │
   │   sum voices → tanh limiter → int16         │
   └─────────────────────────────────────────────┘
        │  esp_codec_dev_write()  (blocking)
        ▼
   I2S → ES8389 codec DAC → NS4150B amp → speakers
```

Two threads touch the engine: **callers** (the hand tracker and the touch UI) push new targets via `synth_update_voice()`; the **render task** consumes them and produces audio. They're decoupled by a spinlock and a target/current split (§7).

---

## 2. Output backend — no PWM, a real DAC

The board has an **ES8389 audio codec**, so audio leaves the chip as finished 16-bit PCM samples over I2S, not as PWM or sigma-delta on a GPIO. Setup (`synth_init`):

```c
s_speaker = bsp_audio_codec_speaker_init();
esp_codec_dev_sample_info_t si = {
    .sample_rate = 48000, .bits_per_sample = 16, .channel = 2, .channel_mask = 0,
};
esp_codec_dev_open(s_speaker, &si);
esp_codec_dev_set_out_vol(s_speaker, 90);
```

The render task then blocks on `esp_codec_dev_write()` each block. That blocking call is doing double duty: it delivers audio **and** it is the engine's clock. The write only returns when the codec's I2S DMA has room for the next block, so the render loop is paced by the 48 kHz hardware itself — no timers, no `vTaskDelay`, no drift.

---

## 3. The render task

```c
static void synth_task(void *arg)
{
    int16_t *block = malloc(BLOCK_FRAMES * 2 * sizeof(int16_t));  // stereo interleaved
    float   *mix   = malloc(BLOCK_FRAMES * sizeof(float));        // mono accumulator

    while (true) {
        memset(mix, 0, ...);
        // snapshot shared controls under the lock
        // for each active voice: ramp params, render BLOCK_FRAMES samples into mix[]
        // convert mix[] → int16 stereo in block[]
        esp_codec_dev_write(s_speaker, block, ...);              // blocks → paces the loop
    }
}
```

- **Block size:** `BLOCK_FRAMES = 240` samples = **5 ms** at 48 kHz. This is the core latency/overhead trade-off: smaller blocks = lower latency but more per-block overhead and lock traffic; 5 ms is comfortably below perceptual latency while keeping overhead low.
- **Internal precision is `float`.** All DSP runs in 32-bit float; quantization to `int16` happens only at the very end. This mirrors Web Audio (which is float end-to-end) and avoids intermediate rounding artifacts.
- **Task placement:** created with `xTaskCreatePinnedToCore(..., configMAX_PRIORITIES - 3, ..., core 1)`. It lives on **core 1**; LVGL, touch, and camera live on core 0. High priority means it preempts almost everything, but because it spends most of its time *blocked* inside the codec write, it yields the core to the hand tracker (also core 1) for free — the tracker runs during the DMA drain.

---

## 4. The voice model

A "voice" is one independent sounding note. There are `SYNTH_NUM_VOICES = 3`: voices 0/1 are up to two camera-tracked hands (a real duet — see the tracker's `assign_detections()`), and voice 2 (`SYNTH_VOICE_TOUCH`) is the touchscreen, kept separate so touch and a tracked hand sound together instead of fighting over the same voice.

```c
typedef struct {
    float freq_cur, freq_tgt;      // ramped: pitch in Hz
    float gain_cur, gain_tgt;      // ramped: linear amplitude 0..~0.5
    float cutoff_cur, cutoff_tgt;  // ramped: filter cutoff in Hz
    float ph1, ph2, ph3, ph_lfo;   // oscillator phase accumulators (0..2π)
    float b0, b1, b2, a1, a2;      // biquad coefficients
    float z1_l, z2_l;              // biquad state
    float cutoff_coeff;            // cutoff the coeffs were last computed for
    float *delay; int delay_pos;   // warm-mode delay line (lazy-allocated in PSRAM)
    bool active;                   // is this voice sounding?
    bool releasing;                // fading to silence, free when gain hits ~0
} voice_t;
```

**Lifecycle:**

- **Attack** — first `synth_update_voice(id, …)` on an idle voice sets `active = true`, seeds `freq_cur` to the target pitch (so it starts *on* pitch rather than sliding up from zero), and sets `gain_cur = 0`. The gain then ramps up over 50 ms, a click-free fade-in.
- **Sustain** — subsequent calls just update the `*_tgt` targets; the render task ramps toward them.
- **Release** — `synth_stop_voice(id)` sets `gain_tgt = 0` and `releasing = true`. The render task fades gain out; once `gain_cur < 0.001`, the voice is marked inactive. This ~50 ms fade is what prevents the click you'd get from cutting a waveform mid-cycle.

The **target/current split** (`_tgt` vs `_cur`) is the whole concurrency and smoothing story in one pattern: callers only ever write targets; the render task only ever walks current values toward them (§7). It decouples the two threads *and* gives smooth parameter motion for free.

---

## 5. Oscillators — direct digital synthesis (DDS)

Every tone starts from a **phase accumulator**. A phase variable walks around the unit circle; the waveform is a function of that phase:

```c
static inline float phase_step(float freq) { return TWO_PI * freq / SAMPLE_RATE; }
// each sample:  v->ph1 += phase_step(f);   out = sinf(v->ph1);
```

`freq` sets how far the phase advances per sample — it's a frequency register in the DDS sense. After each sample the phase is wrapped back into `[0, 2π)`:

```c
static inline void phase_wrap(float *ph) { if (*ph >= TWO_PI) *ph -= TWO_PI; }
```

A single subtract suffices because the per-sample step is always far below 2π (even 2 kHz gives a step of only ~0.26 rad).

Two non-sine waveforms are derived from the phase directly:

```c
static inline float saw(float ph) { return ph / (float)M_PI - 1.0f; }          // -1 → +1 ramp
static inline float tri(float ph) { float s = saw(ph); return 2.0f*fabsf(s)-1.0f; } // V shape
```

These are **naive** (non-band-limited) — they alias at high frequencies. Within the C2–C6 playing range (65–1047 Hz) the aliasing sits low enough to be a non-issue, and the harshness is even on-brand for the bitcrush voice.

Each voice has four phase accumulators (`ph1..ph3` for up to three oscillators, `ph_lfo` for a modulator) so multi-oscillator modes stay independent.

---

## 6. The seven modes

All live in `voice_sample()`, which returns one mono sample for the current voice state. `f = v->freq_cur`.

### FM Synth (default)
```c
float mod = sinf(v->ph2) * (f * 0.5f);   // modulator at 2f, index scales with pitch
out = sinf(v->ph1);
v->ph1 += phase_step(f + mod);            // carrier frequency modulated per-sample
v->ph2 += phase_step(f * 2.0f);
```
Classic 2-operator FM: a sine **carrier** whose instantaneous frequency is pushed around by a sine **modulator** running at 2× the pitch. The modulation index (`f * 0.5`) grows with pitch, so the timbre brightens as you play higher — the electro-theremin sound.

### Clean Wave
```c
out = sinf(v->ph1);  v->ph1 += phase_step(f);
```
A single pure sine. Nothing else. The reference tone.

### Warm Tone
```c
float dry = tri(v->ph1);  v->ph1 += phase_step(f);
float wet = v->delay ? v->delay[v->delay_pos] : 0.0f;
float fb  = tanhf(dry + wet * 0.3f);      // soft-clipped feedback
if (v->delay) { v->delay[v->delay_pos] = fb; v->delay_pos = (v->delay_pos+1) % DELAY_FRAMES; }
out = dry + wet * 0.3f;
```
A triangle through a **150 ms feedback delay line** (`DELAY_FRAMES = 7200` samples, lazily allocated in PSRAM the first time this mode runs). The `tanhf` on the feedback path soft-clips it — this stands in for Wavr's `DynamicsCompressor`, keeping the recirculating signal from blowing up while adding gentle saturation. Surf-guitar warmth.

### Pad
```c
out = 0.4f * (sinf(v->ph1) + sinf(v->ph2) + tri(v->ph3));
v->ph1 += phase_step(f);            // three detuned oscillators
v->ph2 += phase_step(f * 1.005f);   // +0.5%
v->ph3 += phase_step(f * 0.995f);   // -0.5%
```
Three oscillators detuned by ±0.5%. The slow beating between them gives the lush, drifting chorus of an ambient pad.

### Theremin
```c
float vib = sinf(v->ph_lfo) * (f * 0.02f);  // ±2% vibrato
out = sinf(v->ph1);
v->ph1    += phase_step(f + vib);           // carrier pitch wavers
v->ph_lfo += phase_step(5.5f);              // LFO at 5.5 Hz
```
A pure sine with **frequency vibrato**: a slow 5.5 Hz LFO sways the pitch ±2%. That continuous waver is exactly what gives a real theremin its singing, voice-like character. (Same mechanism as FM — one oscillator modulating another's frequency — but at a musical *wobble* rate rather than an audio rate.)

### Organ
```c
out = 0.5f*sinf(v->ph1) + 0.25f*sinf(v->ph2) + 0.15f*sinf(v->ph3);
v->ph1 += phase_step(f);        // fundamental
v->ph2 += phase_step(f * 2.0f); // 2nd harmonic
v->ph3 += phase_step(f * 3.0f); // 3rd harmonic
```
Additive synthesis: fundamental + 2nd + 3rd harmonics at decreasing amplitudes — a compact Hammond-ish drawbar stack.

### Bitcrush
```c
float x = saw(v->ph1);  v->ph1 += phase_step(f);
out = roundf(x * 8.0f) / 8.0f;   // quantize amplitude to 17 levels (~4-bit)
```
A sawtooth whose **amplitude** is quantized to 17 discrete steps — roughly 4-bit depth instead of the full float resolution. The staircasing injects harsh quantization harmonics: gritty lo-fi crunch. This is *bit-depth* reduction, not PWM; the crushed samples still go out the clean DAC.

---

## 7. Parameter smoothing and glide

Raw gesture parameters are jumpy, and jumping a frequency or gain instantly causes zipper noise and clicks. Web Audio solves this with `linearRampToValueAtTime`; here the equivalent is the **target/current ramp**, applied once per block:

```c
float freq_rate = (glide > 0.001f)
    ? (FREQ_MAX - FREQ_MIN) * freq_step_per_block / glide   // Hz allowed to move this block
    : 1e9f;                                                 // glide off ⇒ snap
float gain_rate = 1.0f              * freq_step_per_block / GAIN_RAMP_S;   // 50 ms full-scale
float cut_rate  = (FILTER_MAX-FILTER_MIN) * freq_step_per_block / GAIN_RAMP_S;

float d = v->freq_tgt - v->freq_cur;
v->freq_cur += fminf(fabsf(d), freq_rate) * (d > 0 ? 1 : -1);   // step, clamped
// …same pattern for gain_cur and cutoff_cur
```

Each block (5 ms), each current value moves toward its target by at most a rate-limited step:

- **Glide (portamento)** — `s_glide` (default 0.08 s) is the time to traverse the *entire* pitch range. Small glide = notes snap; larger glide = pitch slides between notes. Set glide to 0 and pitch snaps instantly (`freq_rate` becomes huge).
- **Gain/cutoff** — fixed 50 ms full-scale ramp (`GAIN_RAMP_S`), enough to kill clicks without audible lag.

Note the ramp runs at **block rate (200 Hz)**, not per-sample, so parameters move in 5 ms piecewise-linear steps. On very fast sweeps this is a theoretical zipper source, but at 5 ms granularity it's inaudible in practice.

---

## 8. The filter — RBJ lowpass biquad

Each voice has one lowpass **biquad** (a 2-pole IIR filter), giving the fist/open-hand tone control. Coefficients follow the [RBJ Audio EQ Cookbook](https://www.w3.org/TR/audio-eq-cookbook/) lowpass formulas at fixed **Q = 2** — matching Web Audio's default `BiquadFilterNode`:

```c
float w0    = TWO_PI * cutoff / SAMPLE_RATE;
float alpha = sinf(w0) / (2.0f * q);
float cw    = cosf(w0);
float a0    = 1.0f + alpha;
v->b0 = ((1 - cw)/2) / a0;  v->b1 = (1 - cw) / a0;  v->b2 = v->b0;
v->a1 = (-2*cw) / a0;       v->a2 = (1 - alpha) / a0;
```

Two optimizations matter:

1. **Coefficients are recomputed only when the cutoff actually moves** (`if (fabsf(cutoff_cur - cutoff_coeff) < 1.0f) return;`). The trig (`sinf`/`cosf`) is the expensive part; skipping it when the filter is parked saves real cycles.
2. The **per-sample inner loop is cheap** — transposed direct form II, 5 multiplies + 4 adds:

```c
static inline float biquad_process(voice_t *v, float x) {
    float y   = v->b0 * x + v->z1_l;
    v->z1_l   = v->b1 * x - v->a1 * y + v->z2_l;
    v->z2_l   = v->b2 * x - v->a2 * y;
    return y;
}
```

Cutoff is driven by `openness` (bounding-box area from the tracker, or fully open on touch), mapped linearly to **200 Hz … 8 kHz**.

---

## 9. The pitch pipeline

`freq_norm ∈ [0,1]` (hand X) becomes an audible, in-key frequency in three steps:

```c
float freq = quantize(map_frequency(freq_norm));
```

### Exponential mapping
```c
static float map_frequency(float norm) { return FREQ_MIN * powf(FREQ_MAX/FREQ_MIN, norm); }
```
Pitch is exponential, not linear — equal hand movement = equal *musical interval*, spanning C2 (65 Hz) to C6 (1047 Hz), four octaves.

### Scale quantization
```c
static float quantize(float freq) {
    if (s_scale == CHROMATIC) return midi_to_freq(roundf(freq_to_midi(freq)));
    // else: search ±octaves for the nearest note in the active scale, transposed by root
}
```
The raw frequency is converted to a (fractional) MIDI note, then snapped to the nearest note of the selected **scale** (10 available — major, minor, pentatonic, blues, dorian, …), transposed by the **root note** (`s_root`, default C3). Chromatic snaps to the nearest semitone; the others confine you to musically consonant notes so you can't play a "wrong" note. Frequency↔MIDI is the standard equal-temperament pair:

```c
midi_to_freq(m) = 440 * 2^((m - 69)/12)
freq_to_midi(f) = 69 + 12 * log2(f / 440)
```

`synth_note_name()` reuses `freq_to_midi` to produce the on-screen note label ("A4", "C#3").

---

## 10. Polyphony, mixing, and limiting

Per-voice gain is scaled down as more voices sound, so a duet doesn't clip:

```c
float vol = (vol_norm * 0.5f) / fmaxf(1.0f, voice_count * 0.7f);
```

Voices are summed into the float `mix[]` accumulator, then the **whole mix** passes through a `tanh` soft-limiter before quantizing:

```c
float s = tanhf(mix[i]);          // soft-clip: smooth saturation, never wraps
int16_t q = (int16_t)(s * 30000); // scale to int16 (headroom below 32767)
block[2*i] = block[2*i+1] = q;    // duplicate mono → L + R
```

`tanh` is chosen over hard clipping because it saturates *gracefully* — loud transients round off instead of wrapping into a nasty click. Output is mono duplicated to both stereo channels.

---

## 11. Concurrency model

The engine is lock-light by design. A single FreeRTOS spinlock (`portMUX_TYPE s_lock`) guards the short critical sections where a caller thread and the render task could race:

- `synth_update_voice()` writes a voice's `*_tgt` values and (on attack) its phase/state under the lock, and now also scans `active`/`releasing` across all voices under the lock to compute the polyphony gain divisor — that scan used to run unlocked, racing the render task's own writes to those same fields. The render task snapshots `s_mode`/`s_glide` and reads voice state under the same lock.
- Because callers touch **targets** and the render task walks **current** values, the actual audio math never contends with the control path — the lock is held only for a handful of field writes, never across the DSP loop.
- The warm-mode delay buffer is lazily allocated by the caller and published under the lock; the render task reads the pointer with a `v->delay ?` guard, and the pointer only ever transitions once (NULL → buffer), so there's no torn-read hazard. **Clearing** an already-allocated buffer (re-entering Warm mode) is a different story: the caller thread never touches the buffer's *content* directly, because the render task reads/writes it every sample from another core. Instead `synth_set_mode()` sets a `delay_reset_pending` flag under the lock, and the render task clears the buffer itself — on its own thread, at the top of the block where it starts using WARM mode — before that voice's first Warm sample renders.

`s_scale` / `s_root` are plain single-word writes read only on the caller's own thread (inside `quantize`, called from `synth_update_voice`), so they need no lock.

---

## 12. Latency budget

| Stage | Latency |
|---|---|
| Control update → picked up by render | ≤ 1 block (5 ms) |
| Block render + codec DMA | ~5–10 ms |
| Glide/gain smoothing | 50 ms ramp (by design, not lag) |
| **Synth control-to-sound total** | **~10–15 ms** |

The synth is *not* the bottleneck in the instrument — hand-to-sound latency is dominated by the ~95 ms neural-net inference in the tracker. On the touch path, where there's no inference, the ~10 ms synth latency is what you feel, and it plays tight.

---

## 13. Known rough edges

- ~~**Naive (aliasing) oscillators**~~ — **Fixed.** `saw_blep()` / `tri_blep()` apply PolyBLEP band-limiting to the sawtooth and triangle waveforms (Clean Wave, Warm Tone, Pad). Bitcrush intentionally keeps a naive saw for lo-fi character.
- **Block-rate parameter ramps** — smoothing steps every 5 ms, not per-sample; inaudible in practice but not mathematically click-proof on extreme sweeps.
- **Warm-mode delay is per-voice and never freed** until reboot — a deliberate simplification (it's cheap PSRAM), not a leak that grows.
- ~~**Only hand 0 gets an on-screen marker/note/freq/volume display.**~~ — **Fixed.** Both hands now get independent markers (coral for hand 0, teal for hand 1). The readout panel still tracks hand 0 only. Per-source presence tracking means releasing touch while a camera hand is tracked no longer shows a stale hint.

---

## File map

| File | Contents |
|---|---|
| `main/synth.h` | Public API: `synth_init`, `synth_update_voice`, `synth_stop_voice`, mode/scale setters, `synth_note_name`; the `synth_mode_t` / `synth_scale_t` enums |
| `main/synth.c` | Everything above — voices, oscillators, filter, pitch pipeline, render task |

The public API is deliberately tiny: callers say "voice `id` wants this pitch/volume/openness" (`synth_update_voice`) or "stop" (`synth_stop_voice`), and set global mode/scale/root/glide. Everything else is internal.
