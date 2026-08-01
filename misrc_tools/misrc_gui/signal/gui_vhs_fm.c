/*
 * MISRC GUI - VHS RF Preview Plugin (no GNU Radio dependency)
 *
 * Implements a software FM demod + sync-locked monochrome preview path:
 * raw RF int16 -> quadrature downconversion -> FM discriminator -> deemphasis
 * -> adaptive map to u8 -> H/V sync frame builder -> 720x480 preview.
 */

#include "gui_vhs_fm.h"
#include "../visualization/gui_text.h"
#include "../../common/threading.h"

#include <stdlib.h>
#include <string.h>
#include <stdio.h>
#include <math.h>

#ifndef M_PI
#define M_PI 3.14159265358979323846
#endif

#define VHS_PREVIEW_WIDTH 720
#define VHS_PREVIEW_FIELD_LINES 240
#define VHS_PREVIEW_HEIGHT (VHS_PREVIEW_FIELD_LINES * 2)
#define VHS_PREVIEW_WINDOW_BYTES 1600000
#define VHS_PREVIEW_MIN_WINDOW_BYTES 900000
#define VHS_PREVIEW_MAX_FPS 15.0

#define VHS_BASE_SAMPLE_RATE 40000000.0
#define VHS_EXPECTED_LINE_HZ 15734.264

typedef struct {
    int start;
    int end;
} run_t;

typedef struct {
    bool locked;
    int threshold;
    int hsync_count;
    int vsync_count;
    float line_period_us;
    float decode_ms;
    char reason[96];
} rf_preview_meta_t;

typedef struct {
    double sample_rate;
    int hsync_min_run;
    int hsync_max_run;
    int vsync_min_run;
    int vsync_max_run;
    int hsync_diff_min;
    int hsync_diff_max;
    int cluster_gap;
    int line_tolerance;
    int active_offset;
    int active_samples;
    int post_vsync_skip_lines;
    int field_lines;
    int output_width;
    int output_height;
    double expected_line_samples;
} rf_preview_cfg_t;

static inline float clampf(float v, float lo, float hi) {
    if (v < lo) return lo;
    if (v > hi) return hi;
    return v;
}

static int cmp_u8(const void *a, const void *b) {
    const uint8_t va = *(const uint8_t *)a;
    const uint8_t vb = *(const uint8_t *)b;
    return (va > vb) - (va < vb);
}

static int cmp_i32(const void *a, const void *b) {
    const int va = *(const int *)a;
    const int vb = *(const int *)b;
    return (va > vb) - (va < vb);
}

static uint8_t percentile_sorted_u8(const uint8_t *sorted, size_t count, double frac) {
    if (!sorted || count == 0) return 0;
    size_t idx = (size_t)((double)(count - 1) * frac);
    if (idx >= count) idx = count - 1;
    return sorted[idx];
}

static float median_of_ints(const int *values, int count) {
    if (!values || count <= 0) return 0.0f;
    int *tmp = (int *)malloc((size_t)count * sizeof(int));
    if (!tmp) return 0.0f;
    memcpy(tmp, values, (size_t)count * sizeof(int));
    qsort(tmp, (size_t)count, sizeof(int), cmp_i32);
    float out = 0.0f;
    if (count & 1) {
        out = (float)tmp[count / 2];
    } else {
        out = 0.5f * (float)(tmp[count / 2 - 1] + tmp[count / 2]);
    }
    free(tmp);
    return out;
}

static void build_cfg(rf_preview_cfg_t *cfg, uint32_t sample_rate_hz) {
    if (!cfg) return;
    if (sample_rate_hz == 0) sample_rate_hz = MISRC_SAMPLE_RATE;

    double scale = (double)sample_rate_hz / VHS_BASE_SAMPLE_RATE;

    cfg->sample_rate = (double)sample_rate_hz;
    cfg->hsync_min_run = (int)llround(135.0 * scale);
    cfg->hsync_max_run = (int)llround(330.0 * scale);
    cfg->vsync_min_run = (int)llround(620.0 * scale);
    cfg->vsync_max_run = (int)llround(1600.0 * scale);
    cfg->hsync_diff_min = (int)llround(2350.0 * scale);
    cfg->hsync_diff_max = (int)llround(2750.0 * scale);
    cfg->cluster_gap = (int)llround(2100.0 * scale);
    cfg->line_tolerance = (int)llround(220.0 * scale);
    cfg->active_offset = (int)llround(428.0 * scale);
    cfg->active_samples = (int)llround(2160.0 * scale);
    cfg->post_vsync_skip_lines = 11;
    cfg->field_lines = VHS_PREVIEW_FIELD_LINES;
    cfg->output_width = VHS_PREVIEW_WIDTH;
    cfg->output_height = VHS_PREVIEW_HEIGHT;
    cfg->expected_line_samples = cfg->sample_rate / VHS_EXPECTED_LINE_HZ;

    if (cfg->hsync_min_run < 8) cfg->hsync_min_run = 8;
    if (cfg->vsync_min_run < 16) cfg->vsync_min_run = 16;
    if (cfg->line_tolerance < 4) cfg->line_tolerance = 4;
    if (cfg->active_offset < 1) cfg->active_offset = 1;
    if (cfg->active_samples < cfg->output_width) cfg->active_samples = cfg->output_width;
}

static int find_runs(const uint8_t *mask, size_t count, int min_run, int max_run,
                     run_t *out, int out_cap) {
    if (!mask || !out || out_cap <= 0 || count == 0) return 0;

    int runs = 0;
    size_t i = 0;
    while (i < count) {
        if (!mask[i]) {
            i++;
            continue;
        }
        size_t start = i;
        while (i < count && mask[i]) i++;
        int len = (int)(i - start);
        if (len >= min_run && len <= max_run) {
            if (runs < out_cap) {
                out[runs].start = (int)start;
                out[runs].end = (int)i;
                runs++;
            } else {
                break;
            }
        }
    }
    return runs;
}

static float measure_line_period(const int *starts, int count, const rf_preview_cfg_t *cfg) {
    if (!starts || count < 2 || !cfg) return (float)cfg->expected_line_samples;
    int *diffs = (int *)malloc((size_t)count * sizeof(int));
    if (!diffs) return (float)cfg->expected_line_samples;
    int diff_count = 0;
    for (int i = 1; i < count; i++) {
        int d = starts[i] - starts[i - 1];
        if (d >= cfg->hsync_diff_min && d <= cfg->hsync_diff_max) {
            diffs[diff_count++] = d;
        }
    }
    float period = diff_count > 0 ? median_of_ints(diffs, diff_count) : (float)cfg->expected_line_samples;
    free(diffs);
    return period;
}

static int find_stable_hsync_sequence(const int *starts, int start_count, int after,
                                      double period_init, const rf_preview_cfg_t *cfg,
                                      int *out_sequence, int out_cap) {
    if (!starts || start_count <= 0 || !cfg || !out_sequence || out_cap <= 0) return 0;

    int *candidates = (int *)malloc((size_t)start_count * sizeof(int));
    if (!candidates) return 0;
    int candidate_count = 0;
    for (int i = 0; i < start_count; i++) {
        if (starts[i] > after) {
            candidates[candidate_count++] = starts[i];
        }
    }

    const int required = cfg->field_lines + cfg->post_vsync_skip_lines;
    if (candidate_count < required) {
        free(candidates);
        return 0;
    }

    int attempts = candidate_count < 20 ? candidate_count : 20;
    for (int i = 0; i < attempts; i++) {
        int seq_count = 1;
        out_sequence[0] = candidates[i];
        int cursor = i + 1;
        double period = period_init;
        double expected = (double)candidates[i] + period;
        int misses = 0;

        while (cursor < candidate_count && seq_count < required + 2 && seq_count < out_cap) {
            int position = candidates[cursor];
            double delta = (double)position - expected;

            if (fabs(delta) <= (double)cfg->line_tolerance) {
                out_sequence[seq_count++] = position;
                double measured = (double)position - (double)out_sequence[seq_count - 2];
                period = 0.98 * period + 0.02 * measured;
                expected = (double)position + period;
                misses = 0;
                cursor++;
            } else if ((double)position < expected - (double)cfg->line_tolerance) {
                cursor++;
            } else {
                expected += period;
                misses++;
                if (misses > 1) break;
            }
        }

        if (seq_count >= required) {
            free(candidates);
            return seq_count;
        }
    }

    free(candidates);
    return 0;
}

static bool build_preview_frame_once(const uint8_t *data, size_t count,
                                     const rf_preview_cfg_t *cfg, uint8_t *out_frame,
                                     rf_preview_meta_t *meta) {
    if (!data || !cfg || !out_frame || !meta) return false;

    meta->locked = false;
    meta->threshold = 0;
    meta->hsync_count = 0;
    meta->vsync_count = 0;
    meta->line_period_us = (float)(cfg->expected_line_samples / cfg->sample_rate * 1e6);
    snprintf(meta->reason, sizeof(meta->reason), "analisando");

    if (count < VHS_PREVIEW_MIN_WINDOW_BYTES) {
        snprintf(meta->reason, sizeof(meta->reason), "janela curta");
        return false;
    }

    size_t sampled_count = (count + 511) / 512;
    uint8_t *sampled = (uint8_t *)malloc(sampled_count);
    uint8_t *mask = (uint8_t *)malloc(count);
    if (!sampled || !mask) {
        free(sampled);
        free(mask);
        snprintf(meta->reason, sizeof(meta->reason), "sem memoria");
        return false;
    }

    size_t si = 0;
    for (size_t i = 0; i < count; i += 512) {
        sampled[si++] = data[i];
    }
    sampled_count = si;
    qsort(sampled, sampled_count, sizeof(uint8_t), cmp_u8);

    uint8_t p03 = percentile_sorted_u8(sampled, sampled_count, 0.03);
    uint8_t p12 = percentile_sorted_u8(sampled, sampled_count, 0.12);
    uint8_t p98 = percentile_sorted_u8(sampled, sampled_count, 0.98);

    int threshold;
    if ((int)p12 - (int)p03 >= 12) {
        threshold = (int)llround(0.5 * ((double)p03 + (double)p12));
    } else {
        threshold = (int)llround((double)p03 + fmax(10.0, ((double)p98 - (double)p03) * 0.13));
    }
    if (threshold < 4) threshold = 4;
    if (threshold > 120) threshold = 120;
    meta->threshold = threshold;

    for (size_t i = 0; i < count; i++) {
        mask[i] = (data[i] < (uint8_t)threshold) ? 1 : 0;
    }

    int max_h_runs = (int)(count / (size_t)cfg->hsync_min_run) + 8;
    int max_v_runs = (int)(count / (size_t)cfg->vsync_min_run) + 8;
    run_t *hsync_runs = (run_t *)malloc((size_t)max_h_runs * sizeof(run_t));
    run_t *vsync_runs = (run_t *)malloc((size_t)max_v_runs * sizeof(run_t));
    if (!hsync_runs || !vsync_runs) {
        free(sampled);
        free(mask);
        free(hsync_runs);
        free(vsync_runs);
        snprintf(meta->reason, sizeof(meta->reason), "sem memoria runs");
        return false;
    }

    int hsync_count = find_runs(mask, count, cfg->hsync_min_run, cfg->hsync_max_run, hsync_runs, max_h_runs);
    int vsync_count = find_runs(mask, count, cfg->vsync_min_run, cfg->vsync_max_run, vsync_runs, max_v_runs);
    meta->hsync_count = hsync_count;
    meta->vsync_count = vsync_count;

    if (hsync_count < 100) {
        snprintf(meta->reason, sizeof(meta->reason), "poucos H-sync (%d)", hsync_count);
        free(sampled);
        free(mask);
        free(hsync_runs);
        free(vsync_runs);
        return false;
    }

    int *hsync_starts = (int *)malloc((size_t)hsync_count * sizeof(int));
    if (!hsync_starts) {
        free(sampled);
        free(mask);
        free(hsync_runs);
        free(vsync_runs);
        snprintf(meta->reason, sizeof(meta->reason), "sem memoria hsync");
        return false;
    }
    for (int i = 0; i < hsync_count; i++) {
        hsync_starts[i] = hsync_runs[i].start;
    }

    float line_period = measure_line_period(hsync_starts, hsync_count, cfg);
    meta->line_period_us = (float)(line_period / cfg->sample_rate * 1e6);

    // Find V-sync cluster with enough tail for a complete field.
    int chosen_vertical_end = -1;
    if (vsync_count > 0) {
        int cluster_start = 0;
        for (int i = 1; i <= vsync_count; i++) {
            bool end_cluster = (i == vsync_count) ||
                               ((vsync_runs[i].start - vsync_runs[i - 1].start) >= cfg->cluster_gap);
            if (!end_cluster) continue;

            int cluster_len = i - cluster_start;
            if (cluster_len >= 3) {
                int vertical_end = vsync_runs[i - 1].end;
                int needed_after = (int)((double)(cfg->post_vsync_skip_lines + cfg->field_lines + 3) * (double)line_period);
                if ((int)count - vertical_end > needed_after) {
                    chosen_vertical_end = vertical_end;
                    break;
                }
            }
            cluster_start = i;
        }
    }

    bool has_vertical_lock = true;
    int post_vskip_lines = cfg->post_vsync_skip_lines;
    if (chosen_vertical_end < 0) {
        // Fallback: no valid V-sync cluster, but still try a rolling H-lock frame.
        has_vertical_lock = false;
        post_vskip_lines = 0;
        chosen_vertical_end = -1;
    }

    int seq_cap = cfg->field_lines + cfg->post_vsync_skip_lines + 16;
    int *sequence = (int *)malloc((size_t)seq_cap * sizeof(int));
    if (!sequence) {
        free(sampled);
        free(mask);
        free(hsync_runs);
        free(vsync_runs);
        free(hsync_starts);
        snprintf(meta->reason, sizeof(meta->reason), "sem memoria seq");
        return false;
    }

    int seq_count = find_stable_hsync_sequence(hsync_starts, hsync_count, chosen_vertical_end,
                                               (double)line_period, cfg, sequence, seq_cap);
    int required = post_vskip_lines + cfg->field_lines;
    if (seq_count < required && has_vertical_lock) {
        // Second chance: if V-sync anchor failed to produce a full field, retry H-only rolling lock.
        has_vertical_lock = false;
        post_vskip_lines = 0;
        seq_count = find_stable_hsync_sequence(hsync_starts, hsync_count, -1,
                                               (double)line_period, cfg, sequence, seq_cap);
        required = cfg->field_lines;
    }
    if (seq_count < required) {
        snprintf(meta->reason, sizeof(meta->reason), "lock H insuficiente (%d)", seq_count);
        free(sampled);
        free(mask);
        free(hsync_runs);
        free(vsync_runs);
        free(hsync_starts);
        free(sequence);
        return false;
    }

    uint8_t lut[256];
    uint8_t p99 = percentile_sorted_u8(sampled, sampled_count, 0.99);
    int black = threshold + 4;
    if ((int)p12 > black) black = (int)p12;
    int white = black + 24;
    if ((int)p99 > white) white = (int)p99;
    int range = white - black;
    if (range < 1) range = 1;
    for (int v = 0; v < 256; v++) {
        if (v <= black) lut[v] = 0;
        else if (v >= white) lut[v] = 255;
        else lut[v] = (uint8_t)(((v - black) * 255) / range);
    }

    memset(out_frame, 0, (size_t)cfg->output_width * (size_t)cfg->output_height);
    const double src_step = (double)cfg->active_samples / (double)cfg->output_width;

    for (int y = 0; y < cfg->field_lines; y++) {
        int hpos = sequence[post_vskip_lines + y];
        int start = hpos + cfg->active_offset;
        int end = start + cfg->active_samples;

        if (start < 0 || end > (int)count) {
            snprintf(meta->reason, sizeof(meta->reason), "linha %d incompleta", y);
            free(sampled);
            free(mask);
            free(hsync_runs);
            free(vsync_runs);
            free(hsync_starts);
            free(sequence);
            return false;
        }

        int row0 = (y * 2) * cfg->output_width;
        int row1 = row0 + cfg->output_width;
        for (int x = 0; x < cfg->output_width; x++) {
            int idx = start + (int)((double)x * src_step);
            if (idx >= end) idx = end - 1;
            uint8_t px = lut[data[idx]];
            out_frame[row0 + x] = px;
            out_frame[row1 + x] = px;
        }
    }

    meta->locked = has_vertical_lock;
    snprintf(meta->reason, sizeof(meta->reason), has_vertical_lock ? "LOCK H+V" : "LOCK H (rolling)");

    free(sampled);
    free(mask);
    free(hsync_runs);
    free(vsync_runs);
    free(hsync_starts);
    free(sequence);
    return true;
}

static bool decode_with_auto_polarity(vhs_fm_decoder_t *decoder, uint8_t *out_frame,
                                      const rf_preview_cfg_t *cfg, rf_preview_meta_t *meta) {
    if (!decoder || !decoder->rf_linear || !out_frame || !cfg || !meta) return false;

    rf_preview_meta_t direct = {0};
    bool ok_direct = build_preview_frame_once(decoder->rf_linear, VHS_PREVIEW_WINDOW_BYTES, cfg, out_frame, &direct);
    if (ok_direct) {
        *meta = direct;
        return true;
    }

    uint8_t *inv = (uint8_t *)malloc(VHS_PREVIEW_WINDOW_BYTES);
    if (!inv) {
        *meta = direct;
        return false;
    }
    for (size_t i = 0; i < VHS_PREVIEW_WINDOW_BYTES; i++) {
        inv[i] = (uint8_t)(255 - decoder->rf_linear[i]);
    }

    rf_preview_meta_t inverted = {0};
    bool ok_inv = build_preview_frame_once(inv, VHS_PREVIEW_WINDOW_BYTES, cfg, out_frame, &inverted);
    free(inv);

    if (ok_inv) {
        *meta = inverted;
        return true;
    }

    *meta = (inverted.hsync_count > direct.hsync_count) ? inverted : direct;
    return false;
}

static inline float biquad_process(const biquad_coeffs_t *c, biquad_state_t *s, float x) {
    float y = c->b0 * x + c->b1 * s->x1 + c->b2 * s->x2 - c->a1 * s->y1 - c->a2 * s->y2;
    s->x2 = s->x1;
    s->x1 = x;
    s->y2 = s->y1;
    s->y1 = y;
    return y;
}

static void biquad_reset(biquad_state_t *s) {
    if (!s) return;
    s->x1 = 0.0f; s->x2 = 0.0f; s->y1 = 0.0f; s->y2 = 0.0f;
}

static void biquad_set_lowpass(biquad_coeffs_t *c, float fs, float fc, float q) {
    float w0 = 2.0f * (float)M_PI * fc / fs;
    float cosw0 = cosf(w0);
    float sinw0 = sinf(w0);
    float alpha = sinw0 / (2.0f * q);
    float b0 = (1.0f - cosw0) * 0.5f;
    float b1 = 1.0f - cosw0;
    float b2 = (1.0f - cosw0) * 0.5f;
    float a0 = 1.0f + alpha;
    float a1 = -2.0f * cosw0;
    float a2 = 1.0f - alpha;
    c->b0 = b0 / a0;
    c->b1 = b1 / a0;
    c->b2 = b2 / a0;
    c->a1 = a1 / a0;
    c->a2 = a2 / a0;
}

static void biquad_set_highpass(biquad_coeffs_t *c, float fs, float fc, float q) {
    float w0 = 2.0f * (float)M_PI * fc / fs;
    float cosw0 = cosf(w0);
    float sinw0 = sinf(w0);
    float alpha = sinw0 / (2.0f * q);
    float b0 = (1.0f + cosw0) * 0.5f;
    float b1 = -(1.0f + cosw0);
    float b2 = (1.0f + cosw0) * 0.5f;
    float a0 = 1.0f + alpha;
    float a1 = -2.0f * cosw0;
    float a2 = 1.0f - alpha;
    c->b0 = b0 / a0;
    c->b1 = b1 / a0;
    c->b2 = b2 / a0;
    c->a1 = a1 / a0;
    c->a2 = a2 / a0;
}

static void vhs_reconfigure_demod(vhs_fm_decoder_t *decoder, uint32_t sample_rate_hz) {
    if (!decoder) return;
    if (sample_rate_hz == 0) sample_rate_hz = MISRC_SAMPLE_RATE;

    decoder->preview_sample_rate_hz = sample_rate_hz;

    const float fs = (float)sample_rate_hz;
    const float center_hz = VHS_FM_CENTER_HZ;

    float omega = 2.0f * (float)M_PI * center_hz / fs;
    decoder->osc_step_cos = cosf(omega);
    decoder->osc_step_sin = sinf(omega);
    decoder->osc_cos = 1.0f;
    decoder->osc_sin = 0.0f;

    // Quadrature LPF after downconversion (roughly ±2.5 MHz baseband).
    float iq_cutoff = fminf(2.5e6f, fs * 0.18f);
    decoder->iq_lpf_alpha = expf(-2.0f * (float)M_PI * iq_cutoff / fs);

    // VHS deemphasis ~1.25 us.
    float tau = 1.25e-6f;
    decoder->deemph_alpha = expf(-1.0f / (fs * tau));

    // Pre-filter around VHS RF band (2..7.5 MHz).
    biquad_set_highpass(&decoder->bp_coeffs[0], fs, 2.0e6f, 0.7071f);
    biquad_set_lowpass(&decoder->bp_coeffs[1], fs, 7.5e6f, 0.7071f);
    biquad_reset(&decoder->bp_stage1);
    biquad_reset(&decoder->bp_stage2);

    decoder->i_lpf = 0.0f;
    decoder->q_lpf = 0.0f;
    decoder->prev_i = 0.0f;
    decoder->prev_q = 0.0f;
    decoder->demod_lpf_state1 = 0.0f;
    decoder->demod_mean = 0.0f;
    decoder->demod_dev = 0.1f;
}

void gui_vhs_fm_set_sample_rate(vhs_fm_decoder_t *decoder, uint32_t sample_rate_hz) {
    vhs_reconfigure_demod(decoder, sample_rate_hz);
}

static void vhs_ring_write(vhs_fm_decoder_t *decoder, const uint8_t *src, size_t len) {
    if (!decoder || !decoder->rf_window || !src || len == 0) return;

    const size_t cap = VHS_PREVIEW_WINDOW_BYTES;
    if (len > cap) return;  // Current caller contract appends small chunks only.

    size_t head = decoder->rf_window_head;
    size_t first = cap - head;
    if (first > len) first = len;
    memcpy(decoder->rf_window + head, src, first);

    size_t rem = len - first;
    if (rem > 0) {
        memcpy(decoder->rf_window, src + first, rem);
    }

    head = (head + len) % cap;

    if (decoder->rf_window_fill < cap) {
        size_t free_space = cap - decoder->rf_window_fill;
        if (len >= free_space) decoder->rf_window_fill = cap;
        else decoder->rf_window_fill += len;
    }

    decoder->rf_window_head = head;
}

static void vhs_ring_snapshot_linear(vhs_fm_decoder_t *decoder) {
    if (!decoder || !decoder->rf_window || !decoder->rf_linear) return;
    if (decoder->rf_window_fill < VHS_PREVIEW_WINDOW_BYTES) return;

    size_t head = decoder->rf_window_head;
    size_t tail = VHS_PREVIEW_WINDOW_BYTES - head;
    memcpy(decoder->rf_linear, decoder->rf_window + head, tail);
    if (head > 0) {
        memcpy(decoder->rf_linear + tail, decoder->rf_window, head);
    }
}

bool gui_vhs_fm_init(vhs_fm_decoder_t *decoder) {
    if (!decoder) return false;
    memset(decoder, 0, sizeof(*decoder));

    decoder->state.format = VHS_FM_FORMAT_PAL;
    decoder->frame_width = VHS_PREVIEW_WIDTH;
    decoder->field_height = VHS_PREVIEW_FIELD_LINES;
    atomic_init(&decoder->frame_height, VHS_PREVIEW_HEIGHT);
    atomic_init(&decoder->display_ready, 0);
    atomic_init(&decoder->display_height, VHS_PREVIEW_HEIGHT);
    decoder->front_height = VHS_PREVIEW_HEIGHT;

    // Scope buffers (kept for future debug mode).
    for (int i = 0; i < VHS_FM_SCOPE_COUNT; i++) {
        decoder->scope.buffer[i] = (float *)calloc(VHS_FM_SCOPE_BUFFER_SIZE, sizeof(float));
        if (!decoder->scope.buffer[i]) {
            gui_vhs_fm_cleanup(decoder);
            return false;
        }
    }
    decoder->scope.active_trace = VHS_FM_SCOPE_DEMOD;
    decoder->scope.zoom_scale = 1.0f;
    decoder->scope.y_scale = 1.0f;
    decoder->scope.trigger_enabled = false;

    decoder->frame_buffer = (uint8_t *)calloc((size_t)VHS_PREVIEW_WIDTH * VHS_PREVIEW_HEIGHT, 1);
    decoder->display_front = (uint8_t *)calloc((size_t)VHS_PREVIEW_WIDTH * VHS_PREVIEW_HEIGHT, 1);
    decoder->display_back = (uint8_t *)calloc((size_t)VHS_PREVIEW_WIDTH * VHS_PREVIEW_HEIGHT, 1);
    if (!decoder->frame_buffer || !decoder->display_front || !decoder->display_back) {
        gui_vhs_fm_cleanup(decoder);
        return false;
    }

    decoder->rf_window = (uint8_t *)calloc(VHS_PREVIEW_WINDOW_BYTES, 1);
    decoder->rf_linear = (uint8_t *)calloc(VHS_PREVIEW_WINDOW_BYTES, 1);
    if (!decoder->rf_window || !decoder->rf_linear) {
        gui_vhs_fm_cleanup(decoder);
        return false;
    }

    Color *rgba = (Color *)calloc((size_t)VHS_PREVIEW_WIDTH * VHS_PREVIEW_HEIGHT, sizeof(Color));
    if (!rgba) {
        gui_vhs_fm_cleanup(decoder);
        return false;
    }
    decoder->frame_image.data = rgba;
    decoder->frame_image.width = VHS_PREVIEW_WIDTH;
    decoder->frame_image.height = VHS_PREVIEW_HEIGHT;
    decoder->frame_image.mipmaps = 1;
    decoder->frame_image.format = PIXELFORMAT_UNCOMPRESSED_R8G8B8A8;
    decoder->texture_valid = false;

    decoder->display_mode = VHS_FM_DISPLAY_VIDEO;
    decoder->tune_offset = 0.0f;
    decoder->tune_gain = 1.0f;
    decoder->last_preview_emit_s = 0.0;
    decoder->last_locked = false;
    decoder->last_threshold = 0;
    decoder->last_hsync_count = 0;
    decoder->last_vsync_count = 0;
    decoder->last_line_period_us = 0.0f;
    decoder->last_decode_ms = 0.0f;
    snprintf(decoder->last_reason, sizeof(decoder->last_reason), "aguardando RF");

    vhs_reconfigure_demod(decoder, MISRC_SAMPLE_RATE);
    return true;
}

void gui_vhs_fm_cleanup(vhs_fm_decoder_t *decoder) {
    if (!decoder) return;

    if (decoder->texture_valid) {
        UnloadTexture(decoder->frame_texture);
        decoder->texture_valid = false;
    }

    for (int i = 0; i < VHS_FM_SCOPE_COUNT; i++) {
        free(decoder->scope.buffer[i]);
        decoder->scope.buffer[i] = NULL;
    }

    free(decoder->frame_buffer);
    free(decoder->display_front);
    free(decoder->display_back);
    decoder->frame_buffer = NULL;
    decoder->display_front = NULL;
    decoder->display_back = NULL;

    free(decoder->rf_window);
    free(decoder->rf_linear);
    decoder->rf_window = NULL;
    decoder->rf_linear = NULL;
    decoder->rf_window_head = 0;
    decoder->rf_window_fill = 0;

    free(decoder->frame_image.data);
    decoder->frame_image.data = NULL;

    memset(decoder, 0, sizeof(*decoder));
}

void gui_vhs_fm_reset(vhs_fm_decoder_t *decoder) {
    if (!decoder) return;

    decoder->scope.write_idx = 0;
    decoder->scope.buffer_full = false;
    decoder->scope.triggered = false;
    for (int i = 0; i < VHS_FM_SCOPE_COUNT; i++) {
        if (decoder->scope.buffer[i]) {
            memset(decoder->scope.buffer[i], 0, VHS_FM_SCOPE_BUFFER_SIZE * sizeof(float));
        }
    }

    if (decoder->display_front) memset(decoder->display_front, 0, (size_t)VHS_PREVIEW_WIDTH * VHS_PREVIEW_HEIGHT);
    if (decoder->display_back) memset(decoder->display_back, 0, (size_t)VHS_PREVIEW_WIDTH * VHS_PREVIEW_HEIGHT);
    if (decoder->frame_buffer) memset(decoder->frame_buffer, 0, (size_t)VHS_PREVIEW_WIDTH * VHS_PREVIEW_HEIGHT);
    if (decoder->rf_window) memset(decoder->rf_window, 0, VHS_PREVIEW_WINDOW_BYTES);
    if (decoder->rf_linear) memset(decoder->rf_linear, 0, VHS_PREVIEW_WINDOW_BYTES);

    decoder->rf_window_head = 0;
    decoder->rf_window_fill = 0;
    decoder->last_preview_emit_s = 0.0;
    decoder->last_locked = false;
    decoder->last_threshold = 0;
    decoder->last_hsync_count = 0;
    decoder->last_vsync_count = 0;
    decoder->last_line_period_us = 0.0f;
    decoder->last_decode_ms = 0.0f;
    snprintf(decoder->last_reason, sizeof(decoder->last_reason), "reset");

    atomic_store(&decoder->display_ready, 0);
    atomic_store(&decoder->display_height, VHS_PREVIEW_HEIGHT);
    decoder->front_height = VHS_PREVIEW_HEIGHT;

    vhs_reconfigure_demod(decoder, decoder->preview_sample_rate_hz ? decoder->preview_sample_rate_hz : MISRC_SAMPLE_RATE);
}

void gui_vhs_fm_process_buffer(vhs_fm_decoder_t *decoder, const int16_t *buf, size_t count) {
    if (!decoder || !buf || count < 32) return;

    if (decoder->preview_sample_rate_hz == 0) {
        vhs_reconfigure_demod(decoder, MISRC_SAMPLE_RATE);
    }

    const float alpha_iq = decoder->iq_lpf_alpha;
    const float alpha_de = decoder->deemph_alpha;
    const float one_minus_iq = 1.0f - alpha_iq;
    const float one_minus_de = 1.0f - alpha_de;

    float osc_cos = decoder->osc_cos;
    float osc_sin = decoder->osc_sin;
    const float step_cos = decoder->osc_step_cos;
    const float step_sin = decoder->osc_step_sin;

    float i_lpf = decoder->i_lpf;
    float q_lpf = decoder->q_lpf;
    float prev_i = decoder->prev_i;
    float prev_q = decoder->prev_q;
    float deemph = decoder->demod_lpf_state1;
    float mean = decoder->demod_mean;
    float dev = decoder->demod_dev;
    if (dev < 1e-4f) dev = 1e-4f;

    uint8_t chunk[4096];
    size_t chunk_count = 0;

    for (size_t i = 0; i < count; i++) {
        float x = (float)buf[i];

        // Prefilter to RF band of interest.
        x = biquad_process(&decoder->bp_coeffs[0], &decoder->bp_stage1, x);
        x = biquad_process(&decoder->bp_coeffs[1], &decoder->bp_stage2, x);

        // Quadrature downconversion around VHS FM center.
        float i_raw = x * osc_cos;
        float q_raw = x * osc_sin;
        i_lpf = alpha_iq * i_lpf + one_minus_iq * i_raw;
        q_lpf = alpha_iq * q_lpf + one_minus_iq * q_raw;

        // Complex discriminator (phase derivative approximation).
        float di = i_lpf - prev_i;
        float dq = q_lpf - prev_q;
        float denom = i_lpf * i_lpf + q_lpf * q_lpf + 1e-9f;
        float fm = (i_lpf * dq - q_lpf * di) / denom;
        prev_i = i_lpf;
        prev_q = q_lpf;

        // VHS deemphasis.
        deemph = alpha_de * deemph + one_minus_de * fm;

        // Adaptive normalization into roughly sync-tip/white-friendly range.
        float delta = deemph - mean;
        mean += 0.0006f * delta;
        dev += 0.0006f * (fabsf(delta) - dev);
        if (dev < 1e-4f) dev = 1e-4f;

        float normalized = delta / (dev * 3.2f);
        float tuned = (normalized + decoder->tune_offset) * decoder->tune_gain;
        tuned = clampf(tuned, -1.0f, 1.0f);
        uint8_t u = (uint8_t)lroundf((tuned + 1.0f) * 127.5f);

        chunk[chunk_count++] = u;
        if (chunk_count == sizeof(chunk)) {
            vhs_ring_write(decoder, chunk, chunk_count);
            chunk_count = 0;
        }

        // Advance oscillator (complex rotation).
        float next_cos = osc_cos * step_cos - osc_sin * step_sin;
        float next_sin = osc_sin * step_cos + osc_cos * step_sin;
        osc_cos = next_cos;
        osc_sin = next_sin;

        // Periodic normalization to bound floating-point drift.
        if ((i & 1023u) == 0u) {
            float mag = sqrtf(osc_cos * osc_cos + osc_sin * osc_sin);
            if (mag > 1e-6f) {
                float inv = 1.0f / mag;
                osc_cos *= inv;
                osc_sin *= inv;
            } else {
                osc_cos = 1.0f;
                osc_sin = 0.0f;
            }
        }

        // Keep debug scope traces alive for future scope view.
        if (decoder->scope.buffer[VHS_FM_SCOPE_RAW]) {
            int idx = decoder->scope.write_idx;
            decoder->scope.buffer[VHS_FM_SCOPE_RAW][idx] = (float)buf[i] / 2048.0f;
            decoder->scope.buffer[VHS_FM_SCOPE_DEMOD][idx] = deemph;
            decoder->scope.write_idx = (idx + 1) % VHS_FM_SCOPE_BUFFER_SIZE;
            if (decoder->scope.write_idx == 0) decoder->scope.buffer_full = true;
        }
    }

    if (chunk_count > 0) {
        vhs_ring_write(decoder, chunk, chunk_count);
    }

    decoder->osc_cos = osc_cos;
    decoder->osc_sin = osc_sin;
    decoder->i_lpf = i_lpf;
    decoder->q_lpf = q_lpf;
    decoder->prev_i = prev_i;
    decoder->prev_q = prev_q;
    decoder->demod_lpf_state1 = deemph;
    decoder->demod_mean = mean;
    decoder->demod_dev = dev;

    if (decoder->rf_window_fill < VHS_PREVIEW_WINDOW_BYTES) {
        return;
    }

    double now_s = (double)get_time_us() / 1000000.0;
    if (decoder->last_preview_emit_s > 0.0) {
        double dt = now_s - decoder->last_preview_emit_s;
        if (dt < (1.0 / VHS_PREVIEW_MAX_FPS)) {
            return;
        }
    }

    vhs_ring_snapshot_linear(decoder);

    rf_preview_cfg_t cfg;
    build_cfg(&cfg, decoder->preview_sample_rate_hz);

    uint64_t t0 = get_time_us();
    rf_preview_meta_t meta = {0};
    bool frame_ok = decode_with_auto_polarity(decoder, decoder->frame_buffer, &cfg, &meta);
    uint64_t t1 = get_time_us();
    meta.decode_ms = (float)(t1 - t0) / 1000.0f;
    decoder->last_locked = meta.locked;
    decoder->last_threshold = meta.threshold;
    decoder->last_hsync_count = meta.hsync_count;
    decoder->last_vsync_count = meta.vsync_count;
    decoder->last_line_period_us = meta.line_period_us;
    decoder->last_decode_ms = meta.decode_ms;
    snprintf(decoder->last_reason, sizeof(decoder->last_reason), "%s", meta.reason);
    if (frame_ok) {
        memcpy(decoder->display_back, decoder->frame_buffer,
               (size_t)VHS_PREVIEW_WIDTH * VHS_PREVIEW_HEIGHT);
        atomic_store(&decoder->display_height, VHS_PREVIEW_HEIGHT);
        atomic_store(&decoder->display_ready, 1);
    }

    decoder->last_preview_emit_s = now_s;
}

void gui_vhs_fm_swap_buffers(vhs_fm_decoder_t *decoder) {
    if (!decoder) return;
    if (atomic_exchange(&decoder->display_ready, 0)) {
        int frame_h = atomic_load(&decoder->display_height);
        if (frame_h <= 0 || frame_h > VHS_PREVIEW_HEIGHT) frame_h = VHS_PREVIEW_HEIGHT;
        memcpy(decoder->display_front, decoder->display_back,
               (size_t)VHS_PREVIEW_WIDTH * (size_t)frame_h);
        decoder->front_height = frame_h;
    }
}

static void render_scope_fallback(vhs_fm_decoder_t *decoder, Rectangle bounds) {
    DrawRectangleRec(bounds, (Color){18, 18, 24, 255});
    DrawRectangleLinesEx(bounds, 1.0f, (Color){60, 60, 72, 255});
    if (!decoder || !decoder->scope.buffer[VHS_FM_SCOPE_DEMOD] || !decoder->scope.buffer_full) {
        const char *msg = "VHS RF: aguardando demod";
        int w = MeasureText(msg, 16);
        DrawText(msg, (int)(bounds.x + bounds.width * 0.5f - w * 0.5f),
                 (int)(bounds.y + bounds.height * 0.5f - 8), 16, (Color){180, 180, 180, 255});
        return;
    }

    float *trace = decoder->scope.buffer[VHS_FM_SCOPE_DEMOD];
    int start = decoder->scope.write_idx;
    int width = (int)bounds.width;
    int height = (int)bounds.height;
    if (width < 4 || height < 4) return;

    float cx = bounds.y + bounds.height * 0.5f;
    float prev_x = bounds.x;
    float prev_y = cx;
    for (int x = 0; x < width; x++) {
        int idx = (start + x) % VHS_FM_SCOPE_BUFFER_SIZE;
        float s = clampf(trace[idx] * 8.0f, -1.0f, 1.0f);
        float y = cx - s * (bounds.height * 0.42f);
        float px = bounds.x + (float)x;
        if (x > 0) {
            DrawLineV((Vector2){prev_x, prev_y}, (Vector2){px, y}, (Color){120, 235, 120, 255});
        }
        prev_x = px;
        prev_y = y;
    }
}

void gui_vhs_fm_render_frame(vhs_fm_decoder_t *decoder, float x, float y, float width, float height) {
    if (!decoder || !decoder->display_front) {
        DrawRectangle((int)x, (int)y, (int)width, (int)height, (Color){20, 20, 30, 255});
        DrawText("VHS RF preview unavailable", (int)(x + 16), (int)(y + 16), 16, (Color){180, 180, 180, 255});
        return;
    }

    if (decoder->display_mode == VHS_FM_DISPLAY_SCOPE) {
        render_scope_fallback(decoder, (Rectangle){x, y, width, height});
        return;
    }

    if (!decoder->texture_valid) {
        decoder->frame_texture = LoadTextureFromImage(decoder->frame_image);
        SetTextureFilter(decoder->frame_texture, TEXTURE_FILTER_BILINEAR);
        decoder->texture_valid = true;
    }

    int frame_h = decoder->front_height;
    if (frame_h <= 0 || frame_h > VHS_PREVIEW_HEIGHT) frame_h = VHS_PREVIEW_HEIGHT;

    uint8_t *gray = decoder->display_front;
    uint32_t *rgba = (uint32_t *)decoder->frame_image.data;
    int total = frame_h * VHS_PREVIEW_WIDTH;
    for (int i = 0; i < total; i++) {
        uint8_t g = gray[i];
        rgba[i] = 0xFF000000u | ((uint32_t)g << 16) | ((uint32_t)g << 8) | g;
    }
    UpdateTexture(decoder->frame_texture, decoder->frame_image.data);

    float aspect = 4.0f / 3.0f;
    float display_aspect = width / height;
    float draw_w, draw_h;
    if (display_aspect > aspect) {
        draw_h = height;
        draw_w = height * aspect;
    } else {
        draw_w = width;
        draw_h = width / aspect;
    }
    float draw_x = x + (width - draw_w) * 0.5f;
    float draw_y = y + (height - draw_h) * 0.5f;

    Rectangle src = {0, 0, (float)VHS_PREVIEW_WIDTH, (float)frame_h};
    Rectangle dst = {draw_x, draw_y, draw_w, draw_h};
    DrawTexturePro(decoder->frame_texture, src, dst, (Vector2){0, 0}, 0.0f, WHITE);

    char line1[160];
    const char *lock_state = decoder->last_locked
                           ? "LOCK H+V"
                           : (strncmp(decoder->last_reason, "LOCK H", 6) == 0 ? "LOCK H" : "SCAN");
    snprintf(line1, sizeof(line1),
             "%s | H %d | V %d | linha %.3f us | decoder %.1f ms",
             lock_state,
             decoder->last_hsync_count,
             decoder->last_vsync_count,
             decoder->last_line_period_us,
             decoder->last_decode_ms);
    gui_text_draw_mono(line1, draw_x + 9, draw_y + draw_h - 42, 14, BLACK);
    gui_text_draw_mono(line1, draw_x + 8, draw_y + draw_h - 43, 14,
                       decoder->last_locked ? (Color){120, 255, 120, 255}
                                            : (Color){255, 220, 120, 255});

    gui_text_draw_mono(decoder->last_reason, draw_x + 9, draw_y + draw_h - 24, 14, BLACK);
    gui_text_draw_mono(decoder->last_reason, draw_x + 8, draw_y + draw_h - 25, 14, (Color){230, 230, 230, 255});
}

void gui_vhs_fm_set_format(vhs_fm_decoder_t *decoder, int format_select) {
    if (!decoder) return;
    decoder->state.format = (format_select == 1) ? VHS_FM_FORMAT_NTSC : VHS_FM_FORMAT_PAL;
}

vhs_fm_format_t gui_vhs_fm_get_format(vhs_fm_decoder_t *decoder) {
    return decoder ? decoder->state.format : VHS_FM_FORMAT_UNKNOWN;
}

const char *gui_vhs_fm_get_format_name(vhs_fm_decoder_t *decoder) {
    if (!decoder) return "Unknown";
    switch (decoder->state.format) {
        case VHS_FM_FORMAT_PAL: return "PAL";
        case VHS_FM_FORMAT_NTSC: return "NTSC";
        default: return "Unknown";
    }
}

//-----------------------------------------------------------------------------
// Panel Interface
//-----------------------------------------------------------------------------

static void *vhs_fm_vtable_create(void) {
    vhs_fm_decoder_t *decoder = (vhs_fm_decoder_t *)calloc(1, sizeof(vhs_fm_decoder_t));
    if (!decoder) return NULL;
    if (!gui_vhs_fm_init(decoder)) {
        free(decoder);
        return NULL;
    }
    return decoder;
}

static void vhs_fm_vtable_destroy(void *state) {
    if (!state) return;
    gui_vhs_fm_cleanup((vhs_fm_decoder_t *)state);
    free(state);
}

static void vhs_fm_vtable_clear(void *state) {
    if (!state) return;
    gui_vhs_fm_reset((vhs_fm_decoder_t *)state);
}

static void vhs_fm_vtable_process(void *state, const int16_t *samples, size_t count, uint32_t sample_rate) {
    vhs_fm_decoder_t *decoder = (vhs_fm_decoder_t *)state;
    if (!decoder || !samples || count == 0) return;
    if (sample_rate == 0) sample_rate = MISRC_SAMPLE_RATE;
    if (decoder->preview_sample_rate_hz != sample_rate) {
        vhs_reconfigure_demod(decoder, sample_rate);
    }
    gui_vhs_fm_process_buffer(decoder, samples, count);
}

static void vhs_fm_vtable_render(void *state, struct gui_app *app, int channel,
                                 Rectangle bounds, Color color) {
    (void)app;
    (void)channel;
    (void)color;
    vhs_fm_decoder_t *decoder = (vhs_fm_decoder_t *)state;
    if (!decoder) {
        DrawRectangleRec(bounds, (Color){20, 20, 20, 255});
        DrawText("VHS FM unavailable", (int)(bounds.x + 12), (int)(bounds.y + 12), 16, (Color){180, 180, 180, 255});
        return;
    }
    gui_vhs_fm_swap_buffers(decoder);
    gui_vhs_fm_render_frame(decoder, bounds.x, bounds.y, bounds.width, bounds.height);
}

static bool vhs_fm_handle_click(void *state, struct gui_app *app, int channel,
                                Vector2 pos, Rectangle bounds) {
    (void)app;
    (void)channel;
    vhs_fm_decoder_t *decoder = (vhs_fm_decoder_t *)state;
    if (!decoder) return false;
    if (!CheckCollisionPointRec(pos, bounds)) return false;

    // Ctrl+click toggles scope fallback mode for quick diagnostics.
    if ((IsKeyDown(KEY_LEFT_CONTROL) || IsKeyDown(KEY_RIGHT_CONTROL)) && IsMouseButtonPressed(MOUSE_LEFT_BUTTON)) {
        decoder->display_mode = (decoder->display_mode == VHS_FM_DISPLAY_VIDEO)
                                  ? VHS_FM_DISPLAY_SCOPE
                                  : VHS_FM_DISPLAY_VIDEO;
        return true;
    }
    return false;
}

static bool vhs_fm_handle_scroll(void *state, float delta, Rectangle bounds) {
    (void)bounds;
    vhs_fm_decoder_t *decoder = (vhs_fm_decoder_t *)state;
    if (!decoder || delta == 0.0f) return false;

    // Scroll tunes gain in scope mode (debug aid); ignored in video mode.
    if (decoder->display_mode != VHS_FM_DISPLAY_SCOPE) return false;
    decoder->tune_gain = clampf(decoder->tune_gain + delta * 0.05f, 0.2f, 4.0f);
    return true;
}

static const panel_vtable_t s_vhs_fm_vtable = {
    .name = "VHS FM",
    .create = vhs_fm_vtable_create,
    .destroy = vhs_fm_vtable_destroy,
    .clear = vhs_fm_vtable_clear,
    .process = vhs_fm_vtable_process,
    .render = vhs_fm_vtable_render,
    .render_overlay = NULL,
    .handle_click = vhs_fm_handle_click,
    .handle_scroll = vhs_fm_handle_scroll,
    .get_menu_count = NULL,
    .get_menu = NULL,
};

void gui_vhs_fm_panel_register(void) {
    // Integrated via the Video panel plugin selector in gui_cvbs.c.
    // Standalone panel registration is intentionally disabled here.
    (void)s_vhs_fm_vtable;
}
