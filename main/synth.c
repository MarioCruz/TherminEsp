/*
 * TherminEsp synth engine — C port of Wavr's audio-engine.js.
 *
 * Web Audio's node graph becomes a per-sample render loop: phase-accumulator
 * oscillators, an RBJ lowpass biquad per voice (Q=2), linear parameter ramps
 * standing in for linearRampToValueAtTime. The warm mode's
 * DynamicsCompressor is approximated by a soft clip on the delay feedback.
 */
#include "synth.h"

#include <math.h>
#include <stdlib.h>
#include <string.h>

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "esp_check.h"
#include "esp_codec_dev.h"
#include "esp_heap_caps.h"
#include "esp_log.h"

#include "bsp/audio.h"

static const char *TAG = "synth";

#define SAMPLE_RATE   48000
#define BLOCK_FRAMES  240              /* 5 ms per render block */
#define FREQ_MIN      65.0f            /* C2 */
#define FREQ_MAX      1047.0f          /* C6 */
#define FILTER_MIN    200.0f
#define FILTER_MAX    8000.0f
#define GAIN_RAMP_S   0.05f            /* volume/filter ramp, like the JS */
#define DELAY_FRAMES  7200             /* warm mode: 0.15 s at 48 kHz */
#define TWO_PI        (2.0f * (float)M_PI)

typedef struct {
    /* ramped parameters: current value walks toward target */
    float freq_cur, freq_tgt;
    float gain_cur, gain_tgt;
    float cutoff_cur, cutoff_tgt;
    /* oscillator phases (0..2pi) */
    float ph1, ph2, ph3, ph_lfo;
    /* biquad state + coefficients */
    float b0, b1, b2, a1, a2;
    float z1_l, z2_l;
    float cutoff_coeff;                /* cutoff the coeffs were computed for */
    /* warm-mode delay line (mono, allocated on first use) */
    float *delay;
    int delay_pos;
    bool delay_reset_pending;          /* consumed by the render task itself
                                         * (see synth_set_mode) so a mode
                                         * re-entry never memsets a buffer
                                         * the render task is concurrently
                                         * reading/writing from another core */
    bool active;
    bool releasing;                    /* fading to silence, free when done */
} voice_t;

static const struct {
    const char *name;
    int len;
    int notes[12];
} SCALES[SYNTH_SCALE_COUNT] = {
    [SYNTH_SCALE_CHROMATIC]    = { "Chromatic",        12, {0,1,2,3,4,5,6,7,8,9,10,11} },
    [SYNTH_SCALE_MAJOR]        = { "Major",             7, {0,2,4,5,7,9,11} },
    [SYNTH_SCALE_MINOR]        = { "Natural Minor",     7, {0,2,3,5,7,8,10} },
    [SYNTH_SCALE_PENTATONIC]   = { "Pentatonic Major",  5, {0,2,4,7,9} },
    [SYNTH_SCALE_PENT_MINOR]   = { "Pentatonic Minor",  5, {0,3,5,7,10} },
    [SYNTH_SCALE_BLUES]        = { "Blues",             6, {0,3,5,6,7,10} },
    [SYNTH_SCALE_DORIAN]       = { "Dorian",            7, {0,2,3,5,7,9,10} },
    [SYNTH_SCALE_MIXOLYDIAN]   = { "Mixolydian",        7, {0,2,4,5,7,9,10} },
    [SYNTH_SCALE_HARMONIC_MIN] = { "Harmonic Minor",    7, {0,2,3,5,7,8,11} },
    [SYNTH_SCALE_WHOLE_TONE]   = { "Whole Tone",        6, {0,2,4,6,8,10} },
};

static const char *MODE_NAMES[SYNTH_MODE_COUNT] = {
    "FM Synth", "Clean Wave", "Warm Tone", "Pad", "Theremin", "Organ", "Bitcrush",
};

static voice_t s_voices[SYNTH_NUM_VOICES];
static portMUX_TYPE s_lock = portMUX_INITIALIZER_UNLOCKED;
static synth_mode_t s_mode = SYNTH_MODE_FM;
static synth_scale_t s_scale = SYNTH_SCALE_CHROMATIC;
static int s_root = 48;                /* C3 */
static float s_glide = 0.08f;
static bool s_clean_saw = false;       /* CLEAN mode: false = sine, true = sawtooth */
static esp_codec_dev_handle_t s_speaker;

static const char *NOTE_NAMES[12] = { "C","C#","D","D#","E","F","F#","G","G#","A","A#","B" };

/* ---- pitch helpers (straight ports) ---- */

static float midi_to_freq(float midi)
{
    return 440.0f * powf(2.0f, (midi - 69.0f) / 12.0f);
}

static float freq_to_midi(float freq)
{
    return 69.0f + 12.0f * log2f(freq / 440.0f);
}

static float quantize(float freq)
{
    if (s_scale == SYNTH_SCALE_CHROMATIC) {
        return midi_to_freq(roundf(freq_to_midi(freq)));
    }
    float midi = freq_to_midi(freq);
    int root_pc = s_root % 12;
    float best_midi = roundf(midi);
    float best_dist = 1e9f;
    int base_midi = (int)floorf(midi) - 12;
    for (int oct = 0; oct < 3; oct++) {
        for (int i = 0; i < SCALES[s_scale].len; i++) {
            int pc = SCALES[s_scale].notes[i];
            int candidate = base_midi + oct * 12 + ((pc + root_pc) % 12);
            for (int shift = 0; shift <= 12; shift += 12) {
                float d = fabsf((float)(candidate + shift) - midi);
                if (d < best_dist) {
                    best_dist = d;
                    best_midi = (float)(candidate + shift);
                }
            }
        }
    }
    return midi_to_freq(best_midi);
}

static float map_frequency(float norm)
{
    return FREQ_MIN * powf(FREQ_MAX / FREQ_MIN, norm);
}

/* ---- biquad lowpass, RBJ cookbook, Q = 2 (matches the Web Audio filter) ---- */

static void voice_update_biquad(voice_t *v)
{
    if (fabsf(v->cutoff_cur - v->cutoff_coeff) < 1.0f) {
        return;
    }
    v->cutoff_coeff = v->cutoff_cur;
    const float q = 2.0f;
    float w0 = TWO_PI * v->cutoff_cur / SAMPLE_RATE;
    if (w0 > (float)M_PI * 0.99f) {
        w0 = (float)M_PI * 0.99f;
    }
    float alpha = sinf(w0) / (2.0f * q);
    float cw = cosf(w0);
    float a0 = 1.0f + alpha;
    v->b0 = ((1.0f - cw) / 2.0f) / a0;
    v->b1 = (1.0f - cw) / a0;
    v->b2 = v->b0;
    v->a1 = (-2.0f * cw) / a0;
    v->a2 = (1.0f - alpha) / a0;
}

static inline float biquad_process(voice_t *v, float x)
{
    /* transposed direct form II */
    float y = v->b0 * x + v->z1_l;
    v->z1_l = v->b1 * x - v->a1 * y + v->z2_l;
    v->z2_l = v->b2 * x - v->a2 * y;
    return y;
}

static inline float phase_step(float freq)
{
    return TWO_PI * freq / SAMPLE_RATE;
}

static inline void phase_wrap(float *ph)
{
    if (*ph >= TWO_PI) {
        *ph -= TWO_PI;
    }
}

/* naive saw/triangle from a 0..2pi phase */
static inline float saw(float ph)      { return ph / (float)M_PI - 1.0f; }
static inline float tri(float ph)      { float s = saw(ph); return 2.0f * fabsf(s) - 1.0f; }

/* ---- per-voice render: one mono sample at the voice's current state ---- */

static float voice_sample(voice_t *v, synth_mode_t mode)
{
    float f = v->freq_cur;
    float out = 0.0f;

    switch (mode) {
    case SYNTH_MODE_FM: {
        /* modulator at 2f, index ramps with pitch (modGain = f/2 in the JS) */
        float mod = sinf(v->ph2) * (f * 0.5f);
        out = sinf(v->ph1);
        v->ph1 += phase_step(f + mod);
        v->ph2 += phase_step(f * 2.0f);
        break;
    }
    case SYNTH_MODE_CLEAN:
        out = s_clean_saw ? saw(v->ph1) : sinf(v->ph1);
        v->ph1 += phase_step(f);
        break;
    case SYNTH_MODE_WARM: {
        float dry = tri(v->ph1);
        v->ph1 += phase_step(f);
        float wet = v->delay ? v->delay[v->delay_pos] : 0.0f;
        /* feedback via soft clip — stands in for the JS compressor */
        float fb = tanhf(dry + wet * 0.3f);
        if (v->delay) {
            v->delay[v->delay_pos] = fb;
            v->delay_pos = (v->delay_pos + 1) % DELAY_FRAMES;
        }
        out = dry + wet * 0.3f;
        break;
    }
    case SYNTH_MODE_PAD:
        out = 0.4f * (sinf(v->ph1) + sinf(v->ph2) + tri(v->ph3));
        v->ph1 += phase_step(f);
        v->ph2 += phase_step(f * 1.005f);
        v->ph3 += phase_step(f * 0.995f);
        break;
    case SYNTH_MODE_THEREMIN: {
        float vib = sinf(v->ph_lfo) * (f * 0.02f);
        out = sinf(v->ph1);
        v->ph1 += phase_step(f + vib);
        v->ph_lfo += phase_step(5.5f);
        phase_wrap(&v->ph_lfo);
        break;
    }
    case SYNTH_MODE_ORGAN:
        out = 0.5f * sinf(v->ph1) + 0.25f * sinf(v->ph2) + 0.15f * sinf(v->ph3);
        v->ph1 += phase_step(f);
        v->ph2 += phase_step(f * 2.0f);
        v->ph3 += phase_step(f * 3.0f);
        break;
    case SYNTH_MODE_BITCRUSH: {
        float x = saw(v->ph1);
        v->ph1 += phase_step(f);
        out = roundf(x * 8.0f) / 8.0f;
        break;
    }
    default:
        break;
    }
    phase_wrap(&v->ph1);
    phase_wrap(&v->ph2);
    phase_wrap(&v->ph3);
    return out;
}

/* ---- render task ---- */

static void synth_task(void *arg)
{
    int16_t *block = malloc(BLOCK_FRAMES * 2 * sizeof(int16_t));
    float *mix = malloc(BLOCK_FRAMES * sizeof(float));
    if (!block || !mix) {
        ESP_LOGE(TAG, "no memory for render buffers");
        vTaskDelete(NULL);
        return;
    }

    const float freq_step_per_block = (float)BLOCK_FRAMES / SAMPLE_RATE;

    while (true) {
        memset(mix, 0, BLOCK_FRAMES * sizeof(float));

        taskENTER_CRITICAL(&s_lock);
        synth_mode_t mode = s_mode;
        float glide = s_glide;
        taskEXIT_CRITICAL(&s_lock);

        for (int vi = 0; vi < SYNTH_NUM_VOICES; vi++) {
            voice_t *v = &s_voices[vi];
            if (!v->active) {
                continue;
            }

            /* walk ramped params one block's worth toward their targets */
            float freq_rate = (glide > 0.001f)
                ? (FREQ_MAX - FREQ_MIN) * freq_step_per_block / glide
                : 1e9f;
            float gain_rate = 1.0f * freq_step_per_block / GAIN_RAMP_S;
            float cut_rate = (FILTER_MAX - FILTER_MIN) * freq_step_per_block / GAIN_RAMP_S;

            float d = v->freq_tgt - v->freq_cur;
            v->freq_cur += fminf(fabsf(d), freq_rate) * (d > 0 ? 1 : -1);
            d = v->gain_tgt - v->gain_cur;
            v->gain_cur += fminf(fabsf(d), gain_rate) * (d > 0 ? 1 : -1);
            d = v->cutoff_tgt - v->cutoff_cur;
            v->cutoff_cur += fminf(fabsf(d), cut_rate) * (d > 0 ? 1 : -1);
            voice_update_biquad(v);

            if (v->releasing && v->gain_cur < 0.001f) {
                v->active = false;
                v->releasing = false;
                continue;
            }

            /* Consume a pending Warm-mode buffer reset here, on the render
             * task's own thread, right before this voice's WARM samples are
             * rendered — see the comment in synth_set_mode() for why the
             * caller thread never touches the buffer content directly. */
            if (mode == SYNTH_MODE_WARM && v->delay && v->delay_reset_pending) {
                memset(v->delay, 0, DELAY_FRAMES * sizeof(float));
                v->delay_pos = 0;
                v->delay_reset_pending = false;
            }

            for (int i = 0; i < BLOCK_FRAMES; i++) {
                float s = voice_sample(v, mode);
                mix[i] += biquad_process(v, s) * v->gain_cur;
            }
        }

        for (int i = 0; i < BLOCK_FRAMES; i++) {
            float s = mix[i];
            /* gentle limiter so two hot voices can't wrap */
            s = tanhf(s);
            int16_t q = (int16_t)(s * 30000.0f);
            block[2 * i] = q;
            block[2 * i + 1] = q;
        }

        /* blocking write paces the loop at the codec's 48 kHz */
        esp_codec_dev_write(s_speaker, block, BLOCK_FRAMES * 2 * sizeof(int16_t));
    }
}

/* ---- public API ---- */

esp_err_t synth_init(void)
{
    s_speaker = bsp_audio_codec_speaker_init();
    ESP_RETURN_ON_FALSE(s_speaker, ESP_FAIL, TAG, "speaker codec init failed");

    esp_codec_dev_sample_info_t si = {
        .sample_rate = SAMPLE_RATE, .bits_per_sample = 16,
        .channel = 2, .channel_mask = 0,
    };
    ESP_RETURN_ON_FALSE(esp_codec_dev_open(s_speaker, &si) == 0, ESP_FAIL, TAG,
                        "speaker codec open failed");
    esp_codec_dev_set_out_vol(s_speaker, 90);

    /* Render on core 1; LVGL and the rest of the app live on core 0. */
    BaseType_t ok = xTaskCreatePinnedToCore(synth_task, "synth", 4096, NULL,
                                            configMAX_PRIORITIES - 3, NULL, 1);
    ESP_RETURN_ON_FALSE(ok == pdPASS, ESP_FAIL, TAG, "synth task create failed");
    ESP_LOGI(TAG, "render task up: %d Hz, %d-frame blocks", SAMPLE_RATE, BLOCK_FRAMES);
    return ESP_OK;
}

float synth_update_voice(int id, float freq_norm, float vol_norm, float openness)
{
    if (id < 0 || id >= SYNTH_NUM_VOICES) {
        return 0;
    }
    voice_t *v = &s_voices[id];

    float freq = quantize(map_frequency(freq_norm));
    float cutoff = FILTER_MIN + (FILTER_MAX - FILTER_MIN) * openness;

    int voice_count = 0;
    for (int i = 0; i < SYNTH_NUM_VOICES; i++) {
        if (s_voices[i].active && !s_voices[i].releasing) {
            voice_count++;
        }
    }
    if (!v->active) {
        voice_count++;
    }
    float vol = (vol_norm * 0.5f) / fmaxf(1.0f, voice_count * 0.7f);

    taskENTER_CRITICAL(&s_lock);
    if (!v->active) {
        v->freq_cur = freq;          /* new voice starts on pitch, fades in */
        v->gain_cur = 0.0f;
        v->cutoff_cur = cutoff;
        v->cutoff_coeff = -1.0f;
        v->ph1 = v->ph2 = v->ph3 = v->ph_lfo = 0.0f;
        v->z1_l = v->z2_l = 0.0f;
        v->active = true;
    }
    v->releasing = false;
    v->freq_tgt = freq;
    v->gain_tgt = vol;
    v->cutoff_tgt = cutoff;
    taskEXIT_CRITICAL(&s_lock);

    if (s_mode == SYNTH_MODE_WARM && !v->delay) {
        float *d = heap_caps_calloc(DELAY_FRAMES, sizeof(float), MALLOC_CAP_SPIRAM);
        taskENTER_CRITICAL(&s_lock);
        v->delay = d;
        v->delay_pos = 0;
        taskEXIT_CRITICAL(&s_lock);
    }
    return freq;
}

void synth_stop_voice(int id)
{
    if (id < 0 || id >= SYNTH_NUM_VOICES) {
        return;
    }
    taskENTER_CRITICAL(&s_lock);
    if (s_voices[id].active) {
        s_voices[id].gain_tgt = 0.0f;
        s_voices[id].releasing = true;
    }
    taskEXIT_CRITICAL(&s_lock);
}

void synth_set_mode(synth_mode_t mode)
{
    mode = mode % SYNTH_MODE_COUNT;

    /* Warm mode needs a per-voice delay line. Ensure it exists (and starts
     * silent) right when we enter Warm, not just lazily on the next
     * synth_update_voice() call — otherwise switching to Warm mid-note with
     * no immediate parameter update plays dry, and re-entering Warm without
     * this replays a stale ~150 ms tail from whatever was last recirculating
     * in the buffer. heap_caps_calloc never runs under the spinlock.
     *
     * A buffer that already exists is NOT memset from here: the render task
     * reads/writes it every sample, lock-free, from another core, so a
     * caller-thread memset would race it (a mode-switch landing mid-block
     * could tear delay_pos or the buffer contents between the two writers).
     * Instead this only sets a flag; the render task clears the buffer
     * itself, on its own thread, at the top of the block where it starts
     * actually using WARM mode — see synth_task(). */
    if (mode == SYNTH_MODE_WARM && s_mode != SYNTH_MODE_WARM) {
        for (int i = 0; i < SYNTH_NUM_VOICES; i++) {
            voice_t *v = &s_voices[i];
            if (!v->delay) {
                float *d = heap_caps_calloc(DELAY_FRAMES, sizeof(float), MALLOC_CAP_SPIRAM);
                taskENTER_CRITICAL(&s_lock);
                v->delay = d;
                v->delay_pos = 0;
                taskEXIT_CRITICAL(&s_lock);
            } else {
                taskENTER_CRITICAL(&s_lock);
                v->delay_reset_pending = true;
                taskEXIT_CRITICAL(&s_lock);
            }
        }
    }

    taskENTER_CRITICAL(&s_lock);
    s_mode = mode;
    /* reset phases so mode switches don't glitch */
    for (int i = 0; i < SYNTH_NUM_VOICES; i++) {
        s_voices[i].ph1 = s_voices[i].ph2 = s_voices[i].ph3 = 0.0f;
    }
    taskEXIT_CRITICAL(&s_lock);
}

void synth_set_scale(synth_scale_t scale)  { s_scale = scale % SYNTH_SCALE_COUNT; }
void synth_set_root(int midi_note)         { s_root = midi_note; }
void synth_set_glide(float seconds)        { s_glide = seconds; }
void synth_set_clean_wave(bool saw_wave)   { s_clean_saw = saw_wave; }
synth_mode_t synth_get_mode(void)          { return s_mode; }
synth_scale_t synth_get_scale(void)        { return s_scale; }
int synth_get_root(void)                   { return s_root; }
float synth_get_glide(void)                { return s_glide; }
bool synth_get_clean_wave(void)            { return s_clean_saw; }
const char *synth_mode_name(synth_mode_t m)  { return MODE_NAMES[m % SYNTH_MODE_COUNT]; }
const char *synth_scale_name(synth_scale_t s) { return SCALES[s % SYNTH_SCALE_COUNT].name; }

void synth_note_name(float freq, char *buf)
{
    int midi = (int)roundf(freq_to_midi(freq));
    midi = midi < 0 ? 0 : (midi > 127 ? 127 : midi);
    int pc = midi % 12;
    int octave = midi / 12 - 1;
    snprintf(buf, 8, "%s%d", NOTE_NAMES[pc], octave);
}

const char *synth_pitch_class_name(int midi_note)
{
    return NOTE_NAMES[((midi_note % 12) + 12) % 12];
}
