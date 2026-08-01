/*
 * MISRC GUI - VHS FM Video Demodulator Module
 *
 * Demodulates FM-modulated video from VHS tape captures.
 * VHS PAL uses 3.8-4.8 MHz carrier with 1 MHz deviation.
 * Produces grayscale video frames with line/field synchronization.
 */

#ifndef GUI_VHS_FM_H
#define GUI_VHS_FM_H

#include "../core/gui_app.h"
#include "gui_trigger.h"
#include "../visualization/panel_interface.h"
#include "raylib.h"
#include <stdint.h>
#include <stddef.h>
#include <stdbool.h>
#include <stdatomic.h>

// Sample-rate defaults for VHS FM processing.
#ifndef MISRC_SAMPLE_RATE
#define MISRC_SAMPLE_RATE DEFAULT_SAMPLE_RATE
#endif
#ifndef MISRC_SAMPLE_RATE_MHZ
#define MISRC_SAMPLE_RATE_MHZ ((float)MISRC_SAMPLE_RATE / 1000000.0f)
#endif
//-----------------------------------------------------------------------------
// Panel Interface Registration
//-----------------------------------------------------------------------------

// Register the VHS FM panel vtable with the panel registry.
// Call this once at startup.
void gui_vhs_fm_panel_register(void);

//-----------------------------------------------------------------------------
// VHS FM Constants
//-----------------------------------------------------------------------------

// VHS PAL FM carrier frequencies (Hz)
#define VHS_FM_SYNC_TIP_HZ      3800000.0f   // 3.8 MHz at sync tip
#define VHS_FM_WHITE_LEVEL_HZ   4800000.0f   // 4.8 MHz at white
#define VHS_FM_CENTER_HZ        4300000.0f   // 4.3 MHz center frequency
#define VHS_FM_DEVIATION_HZ     1000000.0f   // 1 MHz total deviation

// Bandpass filter parameters
#define VHS_FM_BP_CENTER_HZ     4300000.0f   // Bandpass center frequency
#define VHS_FM_BP_BANDWIDTH_HZ  2000000.0f   // Bandpass bandwidth (3.3-5.3 MHz)

// Hilbert transform filter
#define VHS_FM_HILBERT_TAPS     31           // 31-tap FIR for Hilbert transform

// Frame dimensions (same as CVBS)
#define VHS_FM_FRAME_WIDTH      720          // Standard horizontal resolution
#define VHS_FM_PAL_HEIGHT       576          // PAL active lines
#define VHS_FM_PAL_FIELD_HEIGHT 288          // PAL field height
#define VHS_FM_MAX_HEIGHT       576          // Maximum frame height

// Line counts
#define VHS_FM_PAL_TOTAL_LINES  625
#define VHS_FM_PAL_ACTIVE_LINES 576

// Timing at 40 MSPS (derived from MISRC_SAMPLE_RATE_MHZ)
#define VHS_FM_LINE_PERIOD_US       64.0f
#define VHS_FM_ACTIVE_VIDEO_US      52.0f
// Sync pulse timing from VHS-decode project documentation:
// - HSYNC: ~4.7 µs (horizontal sync)
// - EQPL:  ~2.3 µs (equalization pulses during V-blank)
// - VSYNC: ~30 µs for PAL, ~27 µs for NTSC (long vertical sync pulses)
// Widened range for VHS tape jitter and filter effects
#define VHS_FM_HSYNC_MIN_US         2.0f    // Min for H-sync or equalization pulse
#define VHS_FM_HSYNC_MAX_US         12.0f   // Max for H-sync (includes some margin)
#define VHS_FM_VSYNC_PULSE_US       30.0f   // PAL V-sync pulse width
#define VHS_FM_VSYNC_MIN_US         20.0f   // Min V-sync pulse (allows for jitter)
#define VHS_FM_VSYNC_MAX_US         40.0f   // Max V-sync pulse
#define VHS_FM_BACK_PORCH_US        7.0f

// Samples at 40 MSPS
#define VHS_FM_LINE_SAMPLES         ((int)(VHS_FM_LINE_PERIOD_US * MISRC_SAMPLE_RATE_MHZ))
#define VHS_FM_ACTIVE_SAMPLES       ((int)(VHS_FM_ACTIVE_VIDEO_US * MISRC_SAMPLE_RATE_MHZ))
#define VHS_FM_HSYNC_MIN_SAMPLES    ((int)(VHS_FM_HSYNC_MIN_US * MISRC_SAMPLE_RATE_MHZ))
#define VHS_FM_HSYNC_MAX_SAMPLES    ((int)(VHS_FM_HSYNC_MAX_US * MISRC_SAMPLE_RATE_MHZ))
#define VHS_FM_VSYNC_MIN_SAMPLES    ((int)(VHS_FM_VSYNC_MIN_US * MISRC_SAMPLE_RATE_MHZ))
#define VHS_FM_VSYNC_MAX_SAMPLES    ((int)(VHS_FM_VSYNC_MAX_US * MISRC_SAMPLE_RATE_MHZ))
#define VHS_FM_BACK_PORCH_SAMPLES   ((int)(VHS_FM_BACK_PORCH_US * MISRC_SAMPLE_RATE_MHZ))

// Line buffer size (slightly more than one line period)
#define VHS_FM_LINE_BUFFER_SIZE     ((int)(75.0f * MISRC_SAMPLE_RATE_MHZ))

// Adaptive level detection buffer size (~0.4ms of samples)
#define VHS_FM_LEVEL_SAMPLE_BUFFER_SIZE ((int)(0.4f * MISRC_SAMPLE_RATE_MHZ * 1000))

//-----------------------------------------------------------------------------

// Simple Direct Form I biquad state and coefficients.
typedef struct {
    float x1, x2;
    float y1, y2;
} biquad_state_t;

typedef struct {
    float b0, b1, b2;
    float a1, a2;
} biquad_coeffs_t;
// Decoder State Structures
//-----------------------------------------------------------------------------

// Video format (currently PAL only, but extensible)
typedef enum {
    VHS_FM_FORMAT_UNKNOWN,
    VHS_FM_FORMAT_PAL,
    VHS_FM_FORMAT_NTSC
} vhs_fm_format_t;

// Display mode
typedef enum {
    VHS_FM_DISPLAY_VIDEO,      // Normal video output
    VHS_FM_DISPLAY_SCOPE       // Oscilloscope waveform view
} vhs_fm_display_mode_t;

// Scope trace selection
typedef enum {
    VHS_FM_SCOPE_RAW,          // Raw input signal
    VHS_FM_SCOPE_BANDPASS,     // After bandpass filter
    VHS_FM_SCOPE_DEMOD,        // After FM demodulation
    VHS_FM_SCOPE_SYNC_LPF,     // After sync detection LPF
    VHS_FM_SCOPE_COUNT
} vhs_fm_scope_trace_t;

// Scope buffer size (2 TV lines worth at 40 MSPS)
#define VHS_FM_SCOPE_BUFFER_SIZE  (2 * 2560)

// Frame synchronization state
typedef struct {
    vhs_fm_format_t format;        // Detected video format
    int total_lines;               // Total lines per frame (625 for PAL)
    int active_lines;              // Active video lines (576 for PAL)
    int current_line;              // Current line being decoded (0-based)
    int current_field;             // Current field (0=odd/first, 1=even/second)
    bool in_vsync;                 // Currently in vertical sync region
    bool frame_complete;           // A complete frame is ready for display
    int frames_decoded;            // Total frames decoded
} vhs_fm_frame_state_t;

// Software PLL state for H-sync tracking (same as CVBS)
typedef struct {
    double phase;                  // Current phase within line (0 to line_period)
    double line_period;            // Nominal line period in samples
    double freq_adjust;            // Fine frequency adjustment

    double phase_error;            // Last phase error
    double phase_integral;         // Integrated phase error

    int good_sync_count;           // Consecutive syncs within tolerance
    int bad_sync_count;            // Consecutive syncs out of tolerance
    bool locked;                   // True if PLL is locked

    int current_line;              // Current line number in field
    int samples_in_line;           // Samples processed in current line
    size_t total_samples;          // Total samples processed
    size_t field_start_sample;     // Sample position when field started (for V-sync timing)
} vhs_fm_pll_state_t;

// V-sync detection state
typedef struct {
    int half_line_count;           // Count of consecutive half-line intervals
    int total_half_lines;          // Total half-line pulses in V-sync
    bool in_vsync;                 // Currently in V-sync region
} vhs_fm_vsync_state_t;

// Adaptive threshold state for demodulated signal (float-based)
typedef struct {
    float sync_tip;                // Estimated sync tip level
    float blanking;                // Estimated blanking level
    float black;                   // Estimated black level
    float white;                   // Estimated white level
    float threshold;               // Current sync threshold

    float *level_sample_buf;       // Buffer for histogram samples
    size_t level_sample_count;     // Current count of samples
    int subsample_counter;         // Counter for subsampling
} vhs_fm_adaptive_levels_t;

// Main decoder structure
typedef struct vhs_fm_decoder {
    //=========================================================================
    // FM Demodulation State
    //=========================================================================

    // Bandpass filter (4th order IIR = 2 cascaded biquads)
    biquad_state_t bp_stage1;
    biquad_state_t bp_stage2;
    biquad_coeffs_t bp_coeffs[2];

    // Hilbert transform FIR filter
    float hilbert_delay_line[VHS_FM_HILBERT_TAPS];
    int hilbert_idx;                   // Circular buffer write index
    float hilbert_coeffs[VHS_FM_HILBERT_TAPS];

    // Quadrature FM demodulator state (NCO + IQ mixer)
    float nco_phase;                   // NCO phase accumulator
    float nco_freq_inc;                // Phase increment per sample (2π * fc / fs)
    float i_lpf;                       // I channel lowpass filter state
    float q_lpf;                       // Q channel lowpass filter state
    float prev_phase;                  // Previous phase for differentiation

    // Legacy phase tracking (kept for compatibility)
    float prev_i;                      // Previous I sample (for phase diff)
    float prev_q;                      // Previous Q sample

    // Post-demodulation lowpass filter (2-stage IIR for stronger filtering)
    float demod_lpf_state1;
    float demod_lpf_state2;
    int32_t demod_lpf_state;  // Legacy, kept for sync LPF

    //=========================================================================
    // Video Sync Detection
    //=========================================================================

    // Software PLL for H-sync tracking
    vhs_fm_pll_state_t pll;

    // V-sync detection
    vhs_fm_vsync_state_t vsync;

    // Adaptive level detection
    vhs_fm_adaptive_levels_t adaptive;

    //=========================================================================
    // Frame Buffer State
    //=========================================================================

    // Track field reception
    bool field_ready[2];               // [0]=odd, [1]=even field received

    // Frame state
    vhs_fm_frame_state_t state;

    // Field buffers (720 x field_height each)
    uint8_t *field_buffer[2];          // Two field buffers for deinterlacing
    int field_height;                  // Height of each field (288 for PAL)

    // Deinterlaced frame buffer (720 x full_height)
    uint8_t *frame_buffer;
    int frame_width;                   // Always VHS_FM_FRAME_WIDTH (720)
    atomic_int frame_height;           // Full frame height (576 for PAL)

    //=========================================================================
    // Double Buffering for Thread-Safe Display
    //=========================================================================

    uint8_t *display_front;            // Front buffer - read by render thread
    uint8_t *display_back;             // Back buffer - written by display thread
    atomic_int display_ready;          // 1 when back buffer has new frame
    atomic_int display_height;         // Height used when writing back buffer
    int front_height;                  // Height of data in front buffer

    //=========================================================================
    // GPU Resources
    //=========================================================================

    Image frame_image;
    Texture2D frame_texture;
    bool texture_valid;

    //=========================================================================
    // Line Assembly
    //=========================================================================

    float *demod_line_buffer;          // Demodulated samples for current line
    int line_buffer_count;             // Samples currently in line buffer

    //=========================================================================
    // Edge/Sync Detection State
    //=========================================================================

    int32_t lpf_state;                 // Sync detection lowpass filter (Q16)
    bool last_above_threshold;         // Edge detector state
    size_t global_sample_pos;          // Total samples processed

    bool in_hsync_pulse;               // Inside H-sync pulse
    size_t hsync_pulse_start;          // Start position of current pulse
    size_t vsync_last_edge_pos;        // Last V-sync edge position

    //=========================================================================
    // Statistics
    //=========================================================================

    struct {
        int fields_decoded;
        int vsync_found;
        int hsyncs_last_field;
        int last_half_line_count;
        int log_counter;
    } debug;

    //=========================================================================
    // UI Overlay State
    //=========================================================================

    struct {
        Rectangle button_rect;
        Rectangle options_rect[2];     // PAL, NTSC
        bool is_visible;
        bool dropdown_open;
        int selected_system;           // 0=PAL, 1=NTSC
    } overlay;

    //=========================================================================
    // Scope Display Mode
    //=========================================================================

    vhs_fm_display_mode_t display_mode;  // Video or Scope

    struct {
        float *buffer[VHS_FM_SCOPE_COUNT];  // Buffers for each trace type
        int write_idx;                       // Circular buffer write position
        bool buffer_full;                    // True after first wrap
        vhs_fm_scope_trace_t active_trace;   // Which trace to display
        float y_scale;                       // Vertical zoom (amplitude)
        float y_offset;                      // Vertical offset
        float zoom_scale;                    // Samples per pixel (time zoom)
        bool trigger_enabled;                // Trigger on/off
        float trigger_level;                 // Trigger threshold level
        int trigger_pos;                     // Trigger position in display (pixels from left)
        int triggered_idx;                   // Buffer index where trigger was found
        bool triggered;                      // True if triggered this frame
    } scope;

    //=========================================================================
    // RF Preview Plugin Runtime (no GNU Radio dependency)
    //=========================================================================

    // Rolling demodulated window (u8 grayscale-like RF-derived signal).
    // Contains up to VHS_PREVIEW_WINDOW_BYTES samples for frame builder.
    uint8_t *rf_window;
    uint8_t *rf_linear;
    size_t rf_window_head;
    size_t rf_window_fill;

    // NCO oscillator state for quadrature downconversion.
    float osc_cos;
    float osc_sin;
    float osc_step_cos;
    float osc_step_sin;

    // One-pole lowpass/deemphasis coefficients (y = a*y + (1-a)*x).
    float iq_lpf_alpha;
    float deemph_alpha;

    // Adaptive normalization of demodulated luma-like signal.
    float demod_mean;
    float demod_dev;

    // User tuning (community defaults from reference implementation intent).
    float tune_offset;
    float tune_gain;
    uint32_t preview_sample_rate_hz;

    // Render/update pacing.
    double last_preview_emit_s;

    // Last decode diagnostics for OSD.
    bool last_locked;
    int last_threshold;
    int last_hsync_count;
    int last_vsync_count;
    float last_line_period_us;
    float last_decode_ms;
    char last_reason[96];

} vhs_fm_decoder_t;

//-----------------------------------------------------------------------------
// Initialization and Cleanup
//-----------------------------------------------------------------------------

// Initialize decoder
// Returns true on success, false on allocation failure
bool gui_vhs_fm_init(vhs_fm_decoder_t *decoder);

// Cleanup decoder resources
void gui_vhs_fm_cleanup(vhs_fm_decoder_t *decoder);

// Reset decoder state (clear frame, reset sync)
void gui_vhs_fm_reset(vhs_fm_decoder_t *decoder);

//-----------------------------------------------------------------------------
// Decoding
//-----------------------------------------------------------------------------

// Process a buffer of raw ADC samples
// Call this from the display thread with each new buffer
void gui_vhs_fm_process_buffer(vhs_fm_decoder_t *decoder,
                                const int16_t *buf, size_t count);

//-----------------------------------------------------------------------------
// Rendering
//-----------------------------------------------------------------------------

// Swap buffers if new frame available (called from render thread)
void gui_vhs_fm_swap_buffers(vhs_fm_decoder_t *decoder);

// Render the decoded video frame
// Scales to fit within the given rectangle while maintaining aspect ratio
void gui_vhs_fm_render_frame(vhs_fm_decoder_t *decoder,
                              float x, float y, float width, float height);

//-----------------------------------------------------------------------------
// Configuration
//-----------------------------------------------------------------------------

// Set video format manually (PAL/NTSC)
void gui_vhs_fm_set_format(vhs_fm_decoder_t *decoder, int format_select);
void gui_vhs_fm_set_sample_rate(vhs_fm_decoder_t *decoder, uint32_t sample_rate_hz);

//-----------------------------------------------------------------------------
// Status
//-----------------------------------------------------------------------------

// Get detected format
vhs_fm_format_t gui_vhs_fm_get_format(vhs_fm_decoder_t *decoder);

// Get format name string
const char *gui_vhs_fm_get_format_name(vhs_fm_decoder_t *decoder);

#endif // GUI_VHS_FM_H
