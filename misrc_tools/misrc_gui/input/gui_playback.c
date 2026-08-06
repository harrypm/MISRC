/*
 * MISRC - hsdaoh-rp2350 GUI - Playback Mode Implementation
 *
 * Reads FLAC files and plays them back as if being captured live.
 * Uses libFLAC stream decoder for reading.
 */

#include "gui_playback.h"
#include "../core/gui_app.h"
#include "../processing/gui_extract.h"
#include "../processing/gui_display_thread.h"
#include "../../common/buffer_manager.h"
#include "../../common/threading.h"

#include <stdio.h>
#include <stdlib.h>
#include <stdint.h>
#include <string.h>
#include <ctype.h>
#include <errno.h>
#include <inttypes.h>
#include <limits.h>
#include <math.h>
#include <stdatomic.h>
#include <time.h>
#if defined(_WIN32) || defined(_WIN64)
#include <io.h>
#else
#include <dirent.h>
#include <sys/types.h>
#endif

#if LIBFLAC_ENABLED == 1
#include "FLAC/stream_decoder.h"
#include "FLAC/metadata.h"
#endif

//-----------------------------------------------------------------------------
// Playback State
//-----------------------------------------------------------------------------

typedef struct {
    uint16_t min_block_size;
    uint16_t max_block_size;
    uint32_t min_frame_size;
    uint32_t max_frame_size;
    uint32_t channels;
} playback_streaminfo_aux_t;

typedef struct {
    // File handles and decoders
#if LIBFLAC_ENABLED == 1
    FLAC__StreamDecoder *decoder_a;
    FLAC__StreamDecoder *decoder_b;
#endif
    FILE *file_a;
    FILE *file_b;

    // File info
    playback_file_info_t info_a;
    playback_file_info_t info_b;
    playback_streaminfo_aux_t stream_a;
    playback_streaminfo_aux_t stream_b;

    // Decode buffers (filled by FLAC decoder callbacks)
    int16_t *decode_buf_a;
    int16_t *decode_buf_b;
    size_t decode_buf_size;        // Allocated size
    size_t decode_available_a;     // Samples available in buffer
    size_t decode_available_b;
    size_t decode_pos_a;           // Current read position in buffer
    size_t decode_pos_b;

    // Playback state
    atomic_int state;              // playback_state_t
    atomic_int speed;              // playback_speed_t
    atomic_bool loop_enabled;
    atomic_bool seek_requested_a;
    atomic_bool seek_requested_b;
    atomic_uint_fast64_t seek_target_a;
    atomic_uint_fast64_t seek_target_b;

    // Position tracking
    atomic_uint_fast64_t current_sample;
    atomic_uint_fast64_t current_sample_a;
    atomic_uint_fast64_t current_sample_b;
    uint64_t total_samples;        // Max of file_a and file_b totals
    double total_duration_seconds; // Duration derived from resolved real sample rate
    double realtime_sample_rate_hz;

    // Thread
    void *playback_thread;
    atomic_bool running;

    // App reference
    gui_app_t *app;
} playback_ctx_t;

static playback_ctx_t s_playback = {0};

//-----------------------------------------------------------------------------
// Raw Format Encoding
//-----------------------------------------------------------------------------

// Encode int16_t samples to the raw 32-bit capture format
// This mirrors the decoding done in extract.c
// Raw format:
//   Bits 0-11:  Channel A (12-bit, stored as 2047 - sample)
//   Bits 12-19: AUX data (8 bits, we set to 0)
//   Bits 20-31: Channel B (12-bit, stored as 2047 - sample)
static inline uint32_t encode_raw_sample(int16_t sample_a, int16_t sample_b) {
    // Clamp samples to 12-bit signed range
    if (sample_a > 2047) sample_a = 2047;
    if (sample_a < -2048) sample_a = -2048;
    if (sample_b > 2047) sample_b = 2047;
    if (sample_b < -2048) sample_b = -2048;

    // Encode: store as (2047 - sample) to match extract.c decoding
    uint32_t ch_a = (uint32_t)((2047 - sample_a) & 0xFFF);
    uint32_t ch_b = (uint32_t)((2047 - sample_b) & 0xFFF);
    uint32_t aux = 0;  // No AUX data during playback

    return ch_a | (aux << 12) | (ch_b << 20);
}

//-----------------------------------------------------------------------------
// Speed Multipliers
//-----------------------------------------------------------------------------

static const float speed_multipliers[] = {
    0.25f,   // PLAYBACK_SPEED_0_25X
    0.5f,    // PLAYBACK_SPEED_0_5X
    1.0f,    // PLAYBACK_SPEED_1X
    2.0f,    // PLAYBACK_SPEED_2X
    4.0f,    // PLAYBACK_SPEED_4X
    0.0f     // PLAYBACK_SPEED_MAX (no delay)
};

const char* gui_playback_speed_name(playback_speed_t speed) {
    switch (speed) {
        case PLAYBACK_SPEED_0_25X: return "0.25x";
        case PLAYBACK_SPEED_0_5X:  return "0.5x";
        case PLAYBACK_SPEED_1X:    return "1x";
        case PLAYBACK_SPEED_2X:    return "2x";
        case PLAYBACK_SPEED_4X:    return "4x";
        case PLAYBACK_SPEED_MAX:   return "Max";
        default: return "?";
    }
}
#if LIBFLAC_ENABLED == 1

#if defined(_WIN32) || defined(_WIN64)
typedef __int64 playback_file_off_t;
#define PLAYBACK_FSEEK _fseeki64
#define PLAYBACK_FTELL _ftelli64
#define PLAYBACK_PATH_SEPARATOR '\\'
#else
typedef off_t playback_file_off_t;
#define PLAYBACK_FSEEK fseeko
#define PLAYBACK_FTELL ftello
#define PLAYBACK_PATH_SEPARATOR '/'
#endif

#define PLAYBACK_FLAC_TOTAL_SAMPLES_FIELD_MOD (1ULL << 36)
#define PLAYBACK_FRAME_SCAN_CHUNK_BYTES (8u << 20)
#define PLAYBACK_FRAME_SCAN_SOFT_EOF_AFTER (4ULL * 1024ULL * 1024ULL)
#define PLAYBACK_MAX_HEADER_LEN 17u
#define PLAYBACK_COMPANION_READ_LIMIT (64u * 1024u)
#define PLAYBACK_AUDIO_RATE_MATCH_TOLERANCE 0.05

typedef enum {
    PLAYBACK_TOTAL_SOURCE_UNRESOLVED = 0,
    PLAYBACK_TOTAL_SOURCE_STREAMINFO,
    PLAYBACK_TOTAL_SOURCE_VORBIS_TAG,
    PLAYBACK_TOTAL_SOURCE_VORBIS_DURATION,
    PLAYBACK_TOTAL_SOURCE_STREAMINFO_WRAP_CORRECTED,
    PLAYBACK_TOTAL_SOURCE_STREAMINFO_WRAP_ESTIMATED,
    PLAYBACK_TOTAL_SOURCE_COMPANION,
    PLAYBACK_TOTAL_SOURCE_FRAME_SCAN
} playback_total_source_t;

typedef struct {
    bool has_rf_total_samples;
    bool has_rf_sample_rate;
    bool has_duration_seconds;
    uint64_t rf_total_samples;
    uint64_t rf_sample_rate;
    double duration_seconds;
} playback_vorbis_rf_tags_t;

static const uint8_t playback_crc8_table[256] = {
    0x00, 0x07, 0x0e, 0x09, 0x1c, 0x1b, 0x12, 0x15, 0x38, 0x3f, 0x36, 0x31, 0x24, 0x23, 0x2a, 0x2d,
    0x70, 0x77, 0x7e, 0x79, 0x6c, 0x6b, 0x62, 0x65, 0x48, 0x4f, 0x46, 0x41, 0x54, 0x53, 0x5a, 0x5d,
    0xe0, 0xe7, 0xee, 0xe9, 0xfc, 0xfb, 0xf2, 0xf5, 0xd8, 0xdf, 0xd6, 0xd1, 0xc4, 0xc3, 0xca, 0xcd,
    0x90, 0x97, 0x9e, 0x99, 0x8c, 0x8b, 0x82, 0x85, 0xa8, 0xaf, 0xa6, 0xa1, 0xb4, 0xb3, 0xba, 0xbd,
    0xc7, 0xc0, 0xc9, 0xce, 0xdb, 0xdc, 0xd5, 0xd2, 0xff, 0xf8, 0xf1, 0xf6, 0xe3, 0xe4, 0xed, 0xea,
    0xb7, 0xb0, 0xb9, 0xbe, 0xab, 0xac, 0xa5, 0xa2, 0x8f, 0x88, 0x81, 0x86, 0x93, 0x94, 0x9d, 0x9a,
    0x27, 0x20, 0x29, 0x2e, 0x3b, 0x3c, 0x35, 0x32, 0x1f, 0x18, 0x11, 0x16, 0x03, 0x04, 0x0d, 0x0a,
    0x57, 0x50, 0x59, 0x5e, 0x4b, 0x4c, 0x45, 0x42, 0x6f, 0x68, 0x61, 0x66, 0x73, 0x74, 0x7d, 0x7a,
    0x89, 0x8e, 0x87, 0x80, 0x95, 0x92, 0x9b, 0x9c, 0xb1, 0xb6, 0xbf, 0xb8, 0xad, 0xaa, 0xa3, 0xa4,
    0xf9, 0xfe, 0xf7, 0xf0, 0xe5, 0xe2, 0xeb, 0xec, 0xc1, 0xc6, 0xcf, 0xc8, 0xdd, 0xda, 0xd3, 0xd4,
    0x69, 0x6e, 0x67, 0x60, 0x75, 0x72, 0x7b, 0x7c, 0x51, 0x56, 0x5f, 0x58, 0x4d, 0x4a, 0x43, 0x44,
    0x19, 0x1e, 0x17, 0x10, 0x05, 0x02, 0x0b, 0x0c, 0x21, 0x26, 0x2f, 0x28, 0x3d, 0x3a, 0x33, 0x34,
    0x4e, 0x49, 0x40, 0x47, 0x52, 0x55, 0x5c, 0x5b, 0x76, 0x71, 0x78, 0x7f, 0x6a, 0x6d, 0x64, 0x63,
    0x3e, 0x39, 0x30, 0x37, 0x22, 0x25, 0x2c, 0x2b, 0x06, 0x01, 0x08, 0x0f, 0x1a, 0x1d, 0x14, 0x13,
    0xae, 0xa9, 0xa0, 0xa7, 0xb2, 0xb5, 0xbc, 0xbb, 0x96, 0x91, 0x98, 0x9f, 0x8a, 0x8d, 0x84, 0x83,
    0xde, 0xd9, 0xd0, 0xd7, 0xc2, 0xc5, 0xcc, 0xcb, 0xe6, 0xe1, 0xe8, 0xef, 0xfa, 0xfd, 0xf4, 0xf3,
};

static const char *gui_playback_total_source_name(playback_total_source_t source) {
    switch (source) {
        case PLAYBACK_TOTAL_SOURCE_STREAMINFO: return "STREAMINFO";
        case PLAYBACK_TOTAL_SOURCE_VORBIS_TAG: return "VORBIS_RF_TOTAL_SAMPLES";
        case PLAYBACK_TOTAL_SOURCE_VORBIS_DURATION: return "VORBIS_DURATION_SECONDS";
        case PLAYBACK_TOTAL_SOURCE_STREAMINFO_WRAP_CORRECTED: return "STREAMINFO_WRAP_CORRECTED";
        case PLAYBACK_TOTAL_SOURCE_STREAMINFO_WRAP_ESTIMATED: return "STREAMINFO_WRAP_ESTIMATED";
        case PLAYBACK_TOTAL_SOURCE_COMPANION: return "COMPANION_LOG_OR_WAV";
        case PLAYBACK_TOTAL_SOURCE_FRAME_SCAN: return "FRAME_HEADER_SCAN";
        default: return "UNRESOLVED";
    }
}

static uint64_t gui_playback_round_positive_ld(long double v) {
    if (!(v > 0.0L) || !isfinite((double)v)) {
        return 0;
    }
    if (v >= (long double)UINT64_MAX) {
        return UINT64_MAX;
    }
    return (uint64_t)(v + 0.5L);
}

static bool gui_playback_is_audio_sample_rate(uint32_t header_sample_rate) {
    if (header_sample_rate == 0) return false;
    static const uint32_t audio_rates[] = {
        22050u, 24000u, 32000u, 44100u, 48000u, 64000u, 88200u, 96000u, 176400u, 192000u, 352800u, 384000u
    };
    for (size_t i = 0; i < sizeof(audio_rates) / sizeof(audio_rates[0]); i++) {
        double target = (double)audio_rates[i];
        if (fabs((double)header_sample_rate - target) <= (target * PLAYBACK_AUDIO_RATE_MATCH_TOLERANCE)) {
            return true;
        }
    }
    return false;
}

static bool gui_playback_extract_msps_hint(const char *path, double *out_msps) {
    if (!path || !out_msps) return false;
    const unsigned char *b = (const unsigned char *)path;
    size_t len = strlen(path);
    size_t i = 0;
    while (i < len) {
        if (!isdigit(b[i])) {
            i++;
            continue;
        }
        size_t start = i;
        bool seen_dot = false;
        while (i < len) {
            if (isdigit(b[i])) {
                i++;
            } else if (b[i] == '.' && !seen_dot) {
                seen_dot = true;
                i++;
            } else {
                break;
            }
        }

        if (i + 4 <= len &&
            tolower(b[i]) == 'm' &&
            tolower(b[i + 1]) == 's' &&
            tolower(b[i + 2]) == 'p' &&
            tolower(b[i + 3]) == 's') {
            size_t n = i - start;
            if (n > 0 && n < 64) {
                char temp[64];
                memcpy(temp, &path[start], n);
                temp[n] = '\0';
                char *endptr = NULL;
                errno = 0;
                double v = strtod(temp, &endptr);
                if (errno == 0 && endptr != temp && *endptr == '\0' && isfinite(v) && v > 0.0) {
                    *out_msps = v;
                    return true;
                }
            }
        }
    }
    return false;
}

static double gui_playback_resolve_real_rate_hz(const char *filepath,
                                                 uint32_t header_sample_rate,
                                                 const playback_vorbis_rf_tags_t *tags) {
    if (tags && tags->has_rf_sample_rate && tags->rf_sample_rate > 0) {
        return (double)tags->rf_sample_rate;
    }
    if (header_sample_rate == 0) {
        return 0.0;
    }
    if (gui_playback_is_audio_sample_rate(header_sample_rate)) {
        return (double)header_sample_rate;
    }
    double msps_hint = 0.0;
    if (gui_playback_extract_msps_hint(filepath, &msps_hint) && msps_hint > 0.0) {
        return msps_hint * 1000000.0;
    }
    return (double)header_sample_rate * 1000.0;
}

static bool gui_playback_eq_ascii_ci_char(char a, char b) {
    if (a == b) return true;
    return (char)tolower((unsigned char)a) == (char)tolower((unsigned char)b);
}

static bool gui_playback_eq_ascii_ci_bytes(const uint8_t *lhs, size_t lhs_len, const char *rhs) {
    if (!lhs || !rhs) return false;
    size_t rhs_len = strlen(rhs);
    if (lhs_len != rhs_len) return false;
    for (size_t i = 0; i < lhs_len; i++) {
        if (!gui_playback_eq_ascii_ci_char((char)lhs[i], rhs[i])) {
            return false;
        }
    }
    return true;
}

static bool gui_playback_parse_u64_bytes(const uint8_t *src, size_t len, uint64_t *out_value) {
    if (!src || !out_value) return false;
    size_t start = 0;
    size_t end = len;
    while (start < end && isspace((unsigned char)src[start])) start++;
    while (end > start && isspace((unsigned char)src[end - 1])) end--;
    if (start >= end) return false;
    uint64_t value = 0;
    for (size_t i = start; i < end; i++) {
        if (src[i] < '0' || src[i] > '9') return false;
        uint64_t digit = (uint64_t)(src[i] - '0');
        if (value > (UINT64_MAX - digit) / 10ULL) {
            return false;
        }
        value = value * 10ULL + digit;
    }
    *out_value = value;
    return true;
}

static bool gui_playback_parse_f64_bytes(const uint8_t *src, size_t len, double *out_value) {
    if (!src || !out_value || len == 0) return false;
    while (len > 0 && isspace((unsigned char)*src)) {
        src++;
        len--;
    }
    while (len > 0 && isspace((unsigned char)src[len - 1])) {
        len--;
    }
    if (len == 0 || len >= 128) {
        return false;
    }
    char temp[128];
    memcpy(temp, src, len);
    temp[len] = '\0';
    char *endptr = NULL;
    errno = 0;
    double value = strtod(temp, &endptr);
    if (errno != 0 || endptr == temp || *endptr != '\0' || !isfinite(value)) {
        return false;
    }
    *out_value = value;
    return true;
}

static bool gui_playback_read_vorbis_rf_tags(const char *filepath, playback_vorbis_rf_tags_t *out_tags) {
    if (!filepath || !out_tags) return false;
    memset(out_tags, 0, sizeof(*out_tags));

    FLAC__StreamMetadata *tags_block = NULL;
    if (!FLAC__metadata_get_tags(filepath, &tags_block)) {
        return false;
    }
    if (!tags_block || tags_block->type != FLAC__METADATA_TYPE_VORBIS_COMMENT) {
        if (tags_block) {
            FLAC__metadata_object_delete(tags_block);
        }
        return false;
    }

    FLAC__StreamMetadata_VorbisComment *vc = &tags_block->data.vorbis_comment;
    for (uint32_t i = 0; i < vc->num_comments; i++) {
        FLAC__StreamMetadata_VorbisComment_Entry *entry = &vc->comments[i];
        if (!entry->entry || entry->length == 0) continue;
        uint32_t split = 0;
        while (split < entry->length && entry->entry[split] != '=') split++;
        if (split == 0 || split >= entry->length) continue;

        const uint8_t *key = entry->entry;
        size_t key_len = split;
        const uint8_t *value = entry->entry + split + 1;
        size_t value_len = (size_t)entry->length - (size_t)split - 1u;

        if (!out_tags->has_rf_total_samples &&
            gui_playback_eq_ascii_ci_bytes(key, key_len, "RF_TOTAL_SAMPLES")) {
            uint64_t parsed = 0;
            if (gui_playback_parse_u64_bytes(value, value_len, &parsed) && parsed > 0) {
                out_tags->has_rf_total_samples = true;
                out_tags->rf_total_samples = parsed;
            }
        } else if (!out_tags->has_rf_sample_rate &&
                   gui_playback_eq_ascii_ci_bytes(key, key_len, "RF_SAMPLE_RATE")) {
            uint64_t parsed = 0;
            if (gui_playback_parse_u64_bytes(value, value_len, &parsed) && parsed > 0) {
                out_tags->has_rf_sample_rate = true;
                out_tags->rf_sample_rate = parsed;
            }
        } else if (!out_tags->has_duration_seconds &&
                   gui_playback_eq_ascii_ci_bytes(key, key_len, "DURATION_SECONDS")) {
            double parsed = 0.0;
            if (gui_playback_parse_f64_bytes(value, value_len, &parsed) && parsed > 0.0) {
                out_tags->has_duration_seconds = true;
                out_tags->duration_seconds = parsed;
            }
        }
    }

    FLAC__metadata_object_delete(tags_block);
    return out_tags->has_rf_total_samples || out_tags->has_rf_sample_rate || out_tags->has_duration_seconds;
}

static bool gui_playback_file_seek_abs(FILE *file, uint64_t offset) {
    if (!file) return false;
    if (offset > (uint64_t)INT64_MAX) return false;
    return PLAYBACK_FSEEK(file, (playback_file_off_t)offset, SEEK_SET) == 0;
}

static bool gui_playback_file_seek_rel(FILE *file, uint64_t offset) {
    if (!file) return false;
    if (offset > (uint64_t)INT64_MAX) return false;
    return PLAYBACK_FSEEK(file, (playback_file_off_t)offset, SEEK_CUR) == 0;
}

static bool gui_playback_file_tell(FILE *file, uint64_t *out_offset) {
    if (!file || !out_offset) return false;
    playback_file_off_t pos = PLAYBACK_FTELL(file);
    if (pos < 0) return false;
    *out_offset = (uint64_t)pos;
    return true;
}

static bool gui_playback_find_audio_offset_and_size(const char *filepath, uint64_t *out_audio_offset, uint64_t *out_file_size) {
    if (!filepath || !out_audio_offset || !out_file_size) return false;
    *out_audio_offset = 0;
    *out_file_size = 0;

    FILE *file = fopen(filepath, "rb");
    if (!file) return false;

    bool ok = false;
    do {
        if (PLAYBACK_FSEEK(file, 0, SEEK_END) != 0) break;
        if (!gui_playback_file_tell(file, out_file_size)) break;
        if (!gui_playback_file_seek_abs(file, 0)) break;

        uint8_t magic[4];
        if (fread(magic, 1, sizeof(magic), file) != sizeof(magic)) break;
        if (memcmp(magic, "fLaC", 4) != 0) break;

        while (true) {
            uint8_t header[4];
            if (fread(header, 1, sizeof(header), file) != sizeof(header)) {
                goto done_audio_offset;
            }
            bool is_last = (header[0] & 0x80u) != 0;
            uint32_t length = ((uint32_t)header[1] << 16) |
                              ((uint32_t)header[2] << 8) |
                              (uint32_t)header[3];
            if (!gui_playback_file_seek_rel(file, length)) {
                goto done_audio_offset;
            }
            if (is_last) {
                if (!gui_playback_file_tell(file, out_audio_offset)) {
                    goto done_audio_offset;
                }
                ok = true;
                goto done_audio_offset;
            }
        }
    } while (0);

done_audio_offset:
    fclose(file);
    return ok;
}

static bool gui_playback_read_utf8_coded(const uint8_t *w, size_t len, uint64_t *out_value, size_t *out_consumed) {
    if (!w || len == 0 || !out_value || !out_consumed) return false;
    uint8_t first = w[0];
    uint8_t read_additional = 0;
    uint8_t mask_data = 0x7F;
    uint8_t mask_mark = 0x80;
    while (first & mask_mark) {
        read_additional++;
        mask_data >>= 1;
        mask_mark >>= 1;
    }
    if (read_additional > 0) {
        if (read_additional == 1) {
            return false;
        }
        read_additional--;
    }
    size_t total = 1u + (size_t)read_additional;
    if (total > len) return false;

    uint64_t value = ((uint64_t)(first & mask_data)) << (6u * (unsigned)read_additional);
    for (size_t k = 0; k < (size_t)read_additional; k++) {
        uint8_t byte = w[1 + k];
        if ((byte & 0xC0u) != 0x80u) return false;
        size_t shift = 6u * ((size_t)read_additional - 1u - k);
        value |= ((uint64_t)(byte & 0x3Fu)) << shift;
    }
    *out_value = value;
    *out_consumed = total;
    return true;
}

static bool gui_playback_parse_frame_header(const uint8_t *w, size_t len,
                                            uint16_t *out_block_size, size_t *out_header_len,
                                            uint64_t *out_number, bool *out_variable) {
    if (!w || len < 4 || !out_block_size || !out_header_len || !out_number || !out_variable) {
        return false;
    }

    uint16_t sync = (uint16_t)(((uint16_t)w[0] << 8) | (uint16_t)w[1]);
    if ((sync & 0xFFFEu) != 0xFFF8u) return false;
    if (sync & 0x0002u) return false;
    bool variable = (sync & 0x0001u) != 0;

    uint8_t bs_sr = w[2];
    uint16_t block_size = 0;
    bool read_8bit_bs = false;
    bool read_16bit_bs = false;
    uint8_t block_code = (uint8_t)(bs_sr >> 4);
    if (block_code == 0x0u) {
        return false;
    } else if (block_code == 0x1u) {
        block_size = 192;
    } else if (block_code >= 0x2u && block_code <= 0x5u) {
        block_size = (uint16_t)(576u * (1u << (block_code - 2u)));
    } else if (block_code == 0x6u) {
        read_8bit_bs = true;
    } else if (block_code == 0x7u) {
        read_16bit_bs = true;
    } else {
        block_size = (uint16_t)(256u * (1u << (block_code - 8u)));
    }

    bool read_8bit_sr = false;
    bool read_16bit_sr = false;
    bool read_16bit_sr_ten = false;
    uint8_t sample_rate_code = (uint8_t)(bs_sr & 0x0Fu);
    if (sample_rate_code == 0xCu) {
        read_8bit_sr = true;
    } else if (sample_rate_code == 0xDu) {
        read_16bit_sr = true;
    } else if (sample_rate_code == 0xEu) {
        read_16bit_sr_ten = true;
    } else if (sample_rate_code == 0xFu) {
        return false;
    }

    uint8_t chan_bps_res = w[3];
    if ((chan_bps_res >> 4) >= 0xBu) return false;
    uint8_t bps_code = (uint8_t)((chan_bps_res & 0x0Eu) >> 1);
    if (!(bps_code == 0 || bps_code == 1 || bps_code == 2 || bps_code == 4 || bps_code == 5 || bps_code == 6)) {
        return false;
    }
    if (chan_bps_res & 0x01u) return false;

    uint64_t number = 0;
    size_t utf8_len = 0;
    if (!gui_playback_read_utf8_coded(&w[4], len - 4u, &number, &utf8_len)) {
        return false;
    }
    size_t pos = 4u + utf8_len;

    if (read_8bit_bs) {
        if (pos + 1u > len) return false;
        block_size = (uint16_t)w[pos] + 1u;
        pos += 1u;
    }
    if (read_16bit_bs) {
        if (pos + 2u > len) return false;
        uint16_t bs = (uint16_t)(((uint16_t)w[pos] << 8) | (uint16_t)w[pos + 1u]);
        if (bs == 0xFFFFu) return false;
        block_size = (uint16_t)(bs + 1u);
        pos += 2u;
    }
    if (read_8bit_sr) {
        if (pos + 1u > len) return false;
        pos += 1u;
    }
    if (read_16bit_sr || read_16bit_sr_ten) {
        if (pos + 2u > len) return false;
        pos += 2u;
    }

    if (pos + 1u > len) return false;
    uint8_t crc = 0;
    for (size_t i = 0; i < pos; i++) {
        crc = playback_crc8_table[(uint8_t)(crc ^ w[i])];
    }
    if (crc != w[pos]) return false;

    *out_block_size = block_size;
    *out_header_len = pos + 1u;
    *out_number = number;
    *out_variable = variable;
    return true;
}

static bool gui_playback_count_samples_by_scanning(const char *filepath, uint64_t audio_offset,
                                                   uint32_t min_frame_size, uint64_t *out_samples) {
    if (!filepath || !out_samples) return false;
    *out_samples = 0;

    FILE *file = fopen(filepath, "rb");
    if (!file) return false;
    if (!gui_playback_file_seek_abs(file, audio_offset)) {
        fclose(file);
        return false;
    }

    uint8_t *window = (uint8_t *)malloc(PLAYBACK_FRAME_SCAN_CHUNK_BYTES + 64u);
    if (!window) {
        fclose(file);
        return false;
    }

    bool ok = false;
    size_t carry_len = 0;
    uint64_t running = 0;
    uint64_t frame_index = 0;
    bool seeded = false;
    bool strict = true;
    uint64_t scanned_so_far = 0;
    size_t min_frame_skip = (size_t)min_frame_size;

    while (true) {
        size_t n = fread(window + carry_len, 1, PLAYBACK_FRAME_SCAN_CHUNK_BYTES, file);
        bool eof = (n == 0);
        size_t total = carry_len + n;
        size_t i = 0;

        while (i + 1u < total) {
            uint8_t b0 = window[i];
            uint8_t b1 = window[i + 1u];
            if (b0 == 0xFFu && (b1 == 0xF8u || b1 == 0xF9u)) {
                if (!eof && (total - i) < PLAYBACK_MAX_HEADER_LEN) {
                    break;
                }
                uint16_t block_size = 0;
                size_t header_len = 0;
                uint64_t number = 0;
                bool variable = false;
                if (gui_playback_parse_frame_header(&window[i], total - i,
                                                    &block_size, &header_len, &number, &variable)) {
                    uint64_t expected = variable ? running : frame_index;
                    if (!strict && !seeded) {
                        if (variable) {
                            running = number;
                        } else {
                            frame_index = number;
                        }
                    } else if (number != expected) {
                        i += 1u;
                        continue;
                    }

                    if (UINT64_MAX - running < (uint64_t)block_size) {
                        goto done_scan;
                    }
                    running += (uint64_t)block_size;
                    frame_index += 1u;
                    seeded = true;
                    strict = true;
                    i += header_len;
                    if (min_frame_skip > header_len) {
                        i += min_frame_skip - header_len;
                    }
                    continue;
                }
            }
            i += 1u;
        }

        if (eof) {
            break;
        }

        scanned_so_far += (uint64_t)n;
        if (!seeded && strict && scanned_so_far >= PLAYBACK_FRAME_SCAN_SOFT_EOF_AFTER) {
            strict = false;
        }

        size_t keep_from = i;
        if (keep_from > total) keep_from = total;
        size_t min_keep_from = (total > 64u) ? (total - 64u) : 0u;
        if (keep_from < min_keep_from) keep_from = min_keep_from;
        carry_len = total - keep_from;
        if (carry_len > 0) {
            memmove(window, window + keep_from, carry_len);
        }
    }

    if (running > 0) {
        *out_samples = running;
        ok = true;
    }

done_scan:
    free(window);
    fclose(file);
    return ok;
}

static bool gui_playback_check_total_samples(uint64_t declared, uint64_t audio_bytes,
                                             uint16_t min_block, uint16_t max_block,
                                             uint32_t min_frame, uint32_t max_frame,
                                             uint32_t channels, uint32_t bps,
                                             uint64_t *out_corrected) {
    if (out_corrected) *out_corrected = 0;
    if (audio_bytes == 0) return true;
    if (declared == 0) return false;

    long double bytes_per_sample = (long double)channels * (long double)((bps + 7u) / 8u);
    if (bytes_per_sample <= 0.0L) bytes_per_sample = 1.0L;

    long double declared_max_bytes = 0.0L;
    if (min_block > 0 && max_frame > 0) {
        declared_max_bytes = (((long double)declared / (long double)min_block) + 1.0L) * (long double)max_frame;
    } else {
        declared_max_bytes = ((long double)declared * bytes_per_sample * 1.05L) + 65536.0L;
    }

    if ((long double)audio_bytes <= declared_max_bytes) {
        return true;
    }

    if (min_block > 0 && max_block > 0 && min_frame > 0 && max_frame > 0) {
        long double lower = ((long double)audio_bytes / (long double)max_frame) * (long double)min_block;
        long double upper = ((long double)audio_bytes / (long double)min_frame) * (long double)max_block;
        uint64_t unique = 0;
        int candidates = 0;
        for (uint64_t k = 1; k < 4096; k++) {
            uint64_t candidate = declared + k * PLAYBACK_FLAC_TOTAL_SAMPLES_FIELD_MOD;
            long double cand_ld = (long double)candidate;
            if (cand_ld > upper) break;
            if (cand_ld >= lower) {
                unique = candidate;
                candidates++;
                if (candidates > 1) break;
            }
        }
        if (candidates == 1 && out_corrected) {
            *out_corrected = unique;
        }
    }

    return false;
}

static bool gui_playback_has_timestamp_at(const char *s, size_t i, size_t len) {
    if (!s || i + 19u > len) return false;
    const unsigned char *b = (const unsigned char *)s;
    return isdigit(b[i + 0]) && isdigit(b[i + 1]) && isdigit(b[i + 2]) && isdigit(b[i + 3]) &&
           b[i + 4] == '.' &&
           isdigit(b[i + 5]) && isdigit(b[i + 6]) &&
           b[i + 7] == '.' &&
           isdigit(b[i + 8]) && isdigit(b[i + 9]) &&
           b[i + 10] == '_' &&
           isdigit(b[i + 11]) && isdigit(b[i + 12]) &&
           b[i + 13] == '.' &&
           isdigit(b[i + 14]) && isdigit(b[i + 15]) &&
           b[i + 16] == '.' &&
           isdigit(b[i + 17]) && isdigit(b[i + 18]);
}

static void gui_playback_capture_base(const char *stem, char *out_base, size_t out_size) {
    if (!out_base || out_size == 0) return;
    out_base[0] = '\0';
    if (!stem || !stem[0]) return;
    size_t len = strlen(stem);
    size_t keep_len = len;
    for (size_t i = 0; i + 19u <= len; i++) {
        if (gui_playback_has_timestamp_at(stem, i, len)) {
            keep_len = i + 19u;
            break;
        }
    }
    if (keep_len >= out_size) keep_len = out_size - 1u;
    memcpy(out_base, stem, keep_len);
    out_base[keep_len] = '\0';
}

static bool gui_playback_split_path(const char *filepath,
                                    char *out_dir, size_t out_dir_size,
                                    char *out_stem, size_t out_stem_size) {
    if (!filepath || !out_dir || out_dir_size == 0 || !out_stem || out_stem_size == 0) {
        return false;
    }

    const char *last_slash = strrchr(filepath, '/');
    const char *last_backslash = strrchr(filepath, '\\');
    const char *sep = last_slash;
    if (last_backslash && (!sep || last_backslash > sep)) sep = last_backslash;

    const char *filename = filepath;
    if (sep) {
        size_t dir_len = (size_t)(sep - filepath);
        if (dir_len == 0) {
            snprintf(out_dir, out_dir_size, "%c", PLAYBACK_PATH_SEPARATOR);
        } else {
            if (dir_len >= out_dir_size) dir_len = out_dir_size - 1u;
            memcpy(out_dir, filepath, dir_len);
            out_dir[dir_len] = '\0';
        }
        filename = sep + 1;
    } else {
        snprintf(out_dir, out_dir_size, ".");
    }

    const char *dot = strrchr(filename, '.');
    size_t stem_len = dot ? (size_t)(dot - filename) : strlen(filename);
    if (stem_len == 0) {
        return false;
    }
    if (stem_len >= out_stem_size) stem_len = out_stem_size - 1u;
    memcpy(out_stem, filename, stem_len);
    out_stem[stem_len] = '\0';
    return true;
}

static bool gui_playback_filename_matches(const char *name, const char *base_prefix, const char *ext_without_dot) {
    if (!name || !base_prefix || !ext_without_dot) return false;
    const char *dot = strrchr(name, '.');
    if (!dot || dot == name || dot[1] == '\0') return false;
    if (strncmp(name, base_prefix, strlen(base_prefix)) != 0) return false;
    return gui_playback_eq_ascii_ci_bytes((const uint8_t *)(dot + 1), strlen(dot + 1), ext_without_dot);
}

static bool gui_playback_parse_log_duration_text(const uint8_t *text, size_t len, double *out_seconds) {
    if (!text || !out_seconds) return false;
    size_t limit = (len < PLAYBACK_COMPANION_READ_LIMIT) ? len : PLAYBACK_COMPANION_READ_LIMIT;
    const char needle[] = "duration";
    const size_t needle_len = sizeof(needle) - 1u;

    for (size_t i = 0; i + needle_len <= limit; i++) {
        if (!gui_playback_eq_ascii_ci_bytes(text + i, needle_len, needle)) continue;
        size_t j = i + needle_len;
        while (j < limit && (text[j] == ':' || text[j] == '=' || isspace((unsigned char)text[j]))) j++;
        size_t num_start = j;
        while (j < limit && (isdigit((unsigned char)text[j]) || text[j] == '.')) j++;
        if (j <= num_start) continue;
        double secs = 0.0;
        if (gui_playback_parse_f64_bytes(text + num_start, j - num_start, &secs) && secs > 0.0) {
            *out_seconds = secs;
            return true;
        }
    }
    return false;
}

static bool gui_playback_read_log_duration_seconds(const char *path, double *out_seconds) {
    if (!path || !out_seconds) return false;
    FILE *file = fopen(path, "rb");
    if (!file) return false;
    uint8_t *buf = (uint8_t *)malloc(PLAYBACK_COMPANION_READ_LIMIT);
    if (!buf) {
        fclose(file);
        return false;
    }
    size_t n = fread(buf, 1, PLAYBACK_COMPANION_READ_LIMIT, file);
    fclose(file);
    bool ok = gui_playback_parse_log_duration_text(buf, n, out_seconds);
    free(buf);
    return ok;
}

static bool gui_playback_read_wav_duration_seconds(const char *path, double *out_seconds) {
    if (!path || !out_seconds) return false;
    FILE *file = fopen(path, "rb");
    if (!file) return false;

    uint8_t head[128];
    size_t n = fread(head, 1, sizeof(head), file);
    fclose(file);
    if (n < 12) return false;
    if (memcmp(head, "RIFF", 4) != 0 || memcmp(head + 8, "WAVE", 4) != 0) return false;

    size_t i = 12;
    uint32_t sample_rate = 0;
    uint16_t channels = 0;
    uint16_t bits_per_sample = 0;
    uint32_t data_size = 0;
    bool have_fmt = false;
    bool have_data = false;
    while (i + 8 <= n) {
        const uint8_t *id = &head[i];
        uint32_t chunk_size = (uint32_t)head[i + 4] |
                              ((uint32_t)head[i + 5] << 8) |
                              ((uint32_t)head[i + 6] << 16) |
                              ((uint32_t)head[i + 7] << 24);
        size_t body = i + 8u;
        if (memcmp(id, "fmt ", 4) == 0 && body + 16u <= n) {
            channels = (uint16_t)head[body + 2] | (uint16_t)((uint16_t)head[body + 3] << 8);
            sample_rate = (uint32_t)head[body + 4] |
                          ((uint32_t)head[body + 5] << 8) |
                          ((uint32_t)head[body + 6] << 16) |
                          ((uint32_t)head[body + 7] << 24);
            bits_per_sample = (uint16_t)head[body + 14] | (uint16_t)((uint16_t)head[body + 15] << 8);
            have_fmt = true;
        } else if (memcmp(id, "data", 4) == 0) {
            data_size = chunk_size;
            have_data = true;
            break;
        }

        size_t next = body + (size_t)chunk_size + ((chunk_size & 1u) ? 1u : 0u);
        if (next <= i) break;
        i = next;
    }

    if (!have_fmt || !have_data || sample_rate == 0 || channels == 0 || bits_per_sample == 0) {
        return false;
    }
    long double bytes_per_frame = (long double)channels * ((long double)bits_per_sample / 8.0L);
    if (bytes_per_frame <= 0.0L) return false;
    long double frames = (long double)data_size / bytes_per_frame;
    long double duration = frames / (long double)sample_rate;
    if (!(duration > 0.0L) || !isfinite((double)duration)) return false;
    *out_seconds = (double)duration;
    return true;
}

static bool gui_playback_try_companion_duration(const char *filepath, double *out_seconds) {
    if (!filepath || !out_seconds) return false;

    char dir[512];
    char stem[256];
    char base[256];
    if (!gui_playback_split_path(filepath, dir, sizeof(dir), stem, sizeof(stem))) {
        return false;
    }
    gui_playback_capture_base(stem, base, sizeof(base));
    if (base[0] == '\0') return false;

#if defined(_WIN32) || defined(_WIN64)
    char pattern[640];
    snprintf(pattern, sizeof(pattern), "%s\\*", dir);
    struct _finddata_t entry;
    intptr_t handle = _findfirst(pattern, &entry);
    if (handle != -1) {
        do {
            if (entry.attrib & _A_SUBDIR) continue;
            if (!gui_playback_filename_matches(entry.name, base, "log")) continue;
            char candidate[640];
            snprintf(candidate, sizeof(candidate), "%s\\%s", dir, entry.name);
            if (gui_playback_read_log_duration_seconds(candidate, out_seconds) && *out_seconds > 0.0) {
                _findclose(handle);
                return true;
            }
        } while (_findnext(handle, &entry) == 0);
        _findclose(handle);
    }

    handle = _findfirst(pattern, &entry);
    if (handle != -1) {
        do {
            if (entry.attrib & _A_SUBDIR) continue;
            if (!gui_playback_filename_matches(entry.name, base, "wav")) continue;
            char candidate[640];
            snprintf(candidate, sizeof(candidate), "%s\\%s", dir, entry.name);
            if (gui_playback_read_wav_duration_seconds(candidate, out_seconds) && *out_seconds > 0.0) {
                _findclose(handle);
                return true;
            }
        } while (_findnext(handle, &entry) == 0);
        _findclose(handle);
    }
#else
    DIR *dp = opendir(dir);
    if (dp) {
        struct dirent *entry = NULL;
        while ((entry = readdir(dp)) != NULL) {
            if (!gui_playback_filename_matches(entry->d_name, base, "log")) continue;
            char candidate[640];
            snprintf(candidate, sizeof(candidate), "%s/%s", dir, entry->d_name);
            if (gui_playback_read_log_duration_seconds(candidate, out_seconds) && *out_seconds > 0.0) {
                closedir(dp);
                return true;
            }
        }
        closedir(dp);
    }

    dp = opendir(dir);
    if (dp) {
        struct dirent *entry = NULL;
        while ((entry = readdir(dp)) != NULL) {
            if (!gui_playback_filename_matches(entry->d_name, base, "wav")) continue;
            char candidate[640];
            snprintf(candidate, sizeof(candidate), "%s/%s", dir, entry->d_name);
            if (gui_playback_read_wav_duration_seconds(candidate, out_seconds) && *out_seconds > 0.0) {
                closedir(dp);
                return true;
            }
        }
        closedir(dp);
    }
#endif

    return false;
}

static uint64_t gui_playback_resolve_total_samples(const char *filepath,
                                                   uint32_t header_sample_rate,
                                                   uint8_t bits_per_sample,
                                                   uint64_t declared_total_samples,
                                                   const playback_streaminfo_aux_t *aux,
                                                   playback_total_source_t *out_source,
                                                   double *out_real_rate_hz) {
    if (out_source) {
        *out_source = (declared_total_samples > 0)
            ? PLAYBACK_TOTAL_SOURCE_STREAMINFO
            : PLAYBACK_TOTAL_SOURCE_UNRESOLVED;
    }
    if (out_real_rate_hz) {
        *out_real_rate_hz = gui_playback_resolve_real_rate_hz(filepath, header_sample_rate, NULL);
    }
    if (!filepath || !aux) {
        return declared_total_samples;
    }

    playback_vorbis_rf_tags_t tags = {0};
    (void)gui_playback_read_vorbis_rf_tags(filepath, &tags);
    double real_rate_hz = gui_playback_resolve_real_rate_hz(filepath, header_sample_rate, &tags);
    if (out_real_rate_hz) {
        *out_real_rate_hz = real_rate_hz;
    }
    if (tags.has_rf_total_samples || tags.has_rf_sample_rate || tags.has_duration_seconds) {
        if (tags.has_rf_total_samples) {
            if (tags.rf_total_samples > 0) {
                if (out_source) *out_source = PLAYBACK_TOTAL_SOURCE_VORBIS_TAG;
                return tags.rf_total_samples;
            }
        }
        if (tags.has_duration_seconds && real_rate_hz > 0.0) {
            uint64_t resolved = gui_playback_round_positive_ld((long double)tags.duration_seconds * (long double)real_rate_hz);
            if (resolved > 0) {
                if (out_source) *out_source = PLAYBACK_TOTAL_SOURCE_VORBIS_DURATION;
                return resolved;
            }
        }
    }

    uint64_t audio_offset = 0;
    uint64_t file_size = 0;
    bool have_audio_bounds = gui_playback_find_audio_offset_and_size(filepath, &audio_offset, &file_size) &&
                             file_size > audio_offset;
    uint64_t resolved_total = declared_total_samples;

    if (declared_total_samples > 0 && have_audio_bounds) {
        uint64_t corrected = 0;
        bool trustworthy = gui_playback_check_total_samples(
            declared_total_samples,
            file_size - audio_offset,
            aux->min_block_size,
            aux->max_block_size,
            aux->min_frame_size,
            aux->max_frame_size,
            aux->channels,
            bits_per_sample,
            &corrected);

        if (!trustworthy) {
            if (corrected > 0) {
                resolved_total = corrected;
                if (out_source) *out_source = PLAYBACK_TOTAL_SOURCE_STREAMINFO_WRAP_CORRECTED;
            } else {
                uint64_t bytes_per_sample = (uint64_t)aux->channels * (uint64_t)((bits_per_sample + 7u) / 8u);
                if (bytes_per_sample > 0) {
                    uint64_t audio_bytes = file_size - audio_offset;
                    uint64_t lower = (audio_bytes + bytes_per_sample - 1u) / bytes_per_sample;
                    for (uint64_t k = 1; k <= 4096; k++) {
                        uint64_t candidate = declared_total_samples + k * PLAYBACK_FLAC_TOTAL_SAMPLES_FIELD_MOD;
                        if (candidate >= lower) {
                            resolved_total = candidate;
                            if (out_source) *out_source = PLAYBACK_TOTAL_SOURCE_STREAMINFO_WRAP_ESTIMATED;
                            break;
                        }
                    }
                }
            }
        }
    }

    if (resolved_total == 0 && real_rate_hz > 0.0) {
        double companion_seconds = 0.0;
        if (gui_playback_try_companion_duration(filepath, &companion_seconds) &&
            companion_seconds > 0.0 && isfinite(companion_seconds)) {
            resolved_total = gui_playback_round_positive_ld((long double)companion_seconds * (long double)real_rate_hz);
            if (resolved_total > 0 && out_source) {
                *out_source = PLAYBACK_TOTAL_SOURCE_COMPANION;
            }
        }
    }

    if (resolved_total == 0 && have_audio_bounds) {
        uint64_t scanned_total = 0;
        if (gui_playback_count_samples_by_scanning(filepath, audio_offset, aux->min_frame_size, &scanned_total) &&
            scanned_total > 0) {
            resolved_total = scanned_total;
            if (out_source) *out_source = PLAYBACK_TOTAL_SOURCE_FRAME_SCAN;
        }
    }

    return resolved_total;
}

static void gui_playback_apply_resolved_file_info(playback_file_info_t *info,
                                                   const playback_streaminfo_aux_t *aux,
                                                   const char *channel_label) {
    if (!info || !aux) return;
    playback_total_source_t source = PLAYBACK_TOTAL_SOURCE_UNRESOLVED;
    double real_rate_hz = 0.0;
    uint64_t resolved_total = gui_playback_resolve_total_samples(
        info->filepath,
        info->sample_rate,
        info->bits_per_sample,
        info->total_samples,
        aux,
        &source,
        &real_rate_hz);

    if (resolved_total > 0) {
        info->total_samples = resolved_total;
    }
    if (real_rate_hz <= 0.0) {
        real_rate_hz = gui_playback_resolve_real_rate_hz(info->filepath, info->sample_rate, NULL);
    }
    if (real_rate_hz > 0.0 && info->total_samples > 0) {
        info->duration_seconds = (double)info->total_samples / real_rate_hz;
    } else {
        info->duration_seconds = 0.0;
    }

    fprintf(stderr,
            "[PLAYBACK] %s resolved total=%" PRIu64 " (source=%s, real_rate=%.0fHz, %.2f sec)\n",
            channel_label ? channel_label : "File",
            info->total_samples,
            gui_playback_total_source_name(source),
            real_rate_hz,
            info->duration_seconds);
}

static double gui_playback_real_rate_from_info(const playback_file_info_t *info) {
    if (!info) return 0.0;
    if (info->total_samples > 0 && info->duration_seconds > 0.0 && isfinite(info->duration_seconds)) {
        return (double)info->total_samples / info->duration_seconds;
    }
    return gui_playback_resolve_real_rate_hz(info->filepath, info->sample_rate, NULL);
}

#endif // LIBFLAC_ENABLED

//-----------------------------------------------------------------------------
// FLAC Decoder Callbacks
//-----------------------------------------------------------------------------

#if LIBFLAC_ENABLED == 1

// Channel A write callback
static FLAC__StreamDecoderWriteStatus decoder_write_cb_a(
    const FLAC__StreamDecoder *decoder,
    const FLAC__Frame *frame,
    const FLAC__int32 *const buffer[],
    void *client_data)
{
    (void)decoder;
    playback_ctx_t *ctx = (playback_ctx_t *)client_data;

    uint32_t samples = frame->header.blocksize;
    uint8_t bps = frame->header.bits_per_sample;

    // Ensure buffer space
    if (ctx->decode_available_a + samples > ctx->decode_buf_size) {
        // Buffer overflow - shouldn't happen with proper flow control
        fprintf(stderr, "[PLAYBACK] Channel A decode buffer overflow\n");
        return FLAC__STREAM_DECODER_WRITE_STATUS_ABORT;
    }

    // Convert to int16 and store
    int16_t *out = ctx->decode_buf_a + ctx->decode_available_a;
    const FLAC__int32 *in = buffer[0];  // Mono FLAC

    // Debug: print first frame's sample info
    static int debug_count_a = 0;
    if (debug_count_a < 5) {
        int32_t min_val = in[0], max_val = in[0];
        for (uint32_t i = 1; i < samples && i < 1000; i++) {
            if (in[i] < min_val) min_val = in[i];
            if (in[i] > max_val) max_val = in[i];
        }
        fprintf(stderr, "[PLAYBACK] Ch A frame %d: bps=%u, samples=%u, range=[%d, %d]\n",
                debug_count_a, bps, samples, min_val, max_val);
        debug_count_a++;
    }

    // FLAC returns samples as int32 with the full bit depth
    // MISRC recording format:
    //   16-bit FLAC: 12-bit ADC samples shifted left 4 bits (range -32768 to 32752)
    //   12-bit FLAC: 12-bit ADC samples as-is (range -2048 to 2047)
    //   8-bit FLAC: samples clamped to 8-bit (range -128 to 127)
    // Display expects 12-bit range values (like simulated mode uses ~1024 scale)
    if (bps == 16) {
        // 16-bit FLAC: shift right 4 to get back to 12-bit range
        for (uint32_t i = 0; i < samples; i++) {
            out[i] = (int16_t)(in[i] >> 4);
        }
    } else if (bps == 12) {
        // 12-bit FLAC: already in 12-bit range, just cast
        for (uint32_t i = 0; i < samples; i++) {
            out[i] = (int16_t)in[i];
        }
    } else if (bps == 8) {
        // 8-bit FLAC: keep in 8-bit range (-128 to 127), don't scale up
        // This will show reduced amplitude on display, reflecting the lower bit depth
        for (uint32_t i = 0; i < samples; i++) {
            out[i] = (int16_t)in[i];
        }
    } else {
        // Unknown bit depth - just cast
        for (uint32_t i = 0; i < samples; i++) {
            out[i] = (int16_t)in[i];
        }
    }

    ctx->decode_available_a += samples;
    return FLAC__STREAM_DECODER_WRITE_STATUS_CONTINUE;
}

// Channel B write callback
static FLAC__StreamDecoderWriteStatus decoder_write_cb_b(
    const FLAC__StreamDecoder *decoder,
    const FLAC__Frame *frame,
    const FLAC__int32 *const buffer[],
    void *client_data)
{
    (void)decoder;
    playback_ctx_t *ctx = (playback_ctx_t *)client_data;

    uint32_t samples = frame->header.blocksize;
    uint8_t bps = frame->header.bits_per_sample;

    if (ctx->decode_available_b + samples > ctx->decode_buf_size) {
        fprintf(stderr, "[PLAYBACK] Channel B decode buffer overflow\n");
        return FLAC__STREAM_DECODER_WRITE_STATUS_ABORT;
    }

    int16_t *out = ctx->decode_buf_b + ctx->decode_available_b;
    const FLAC__int32 *in = buffer[0];

    // Debug: print first frame's sample info
    static int debug_count_b = 0;
    if (debug_count_b < 5) {
        int32_t min_val = in[0], max_val = in[0];
        for (uint32_t i = 1; i < samples && i < 1000; i++) {
            if (in[i] < min_val) min_val = in[i];
            if (in[i] > max_val) max_val = in[i];
        }
        fprintf(stderr, "[PLAYBACK] Ch B frame %d: bps=%u, samples=%u, range=[%d, %d]\n",
                debug_count_b, bps, samples, min_val, max_val);
        debug_count_b++;
    }

    // Same conversion as Channel A - normalize to 12-bit range for display
    if (bps == 16) {
        // 16-bit FLAC: shift right 4 to get back to 12-bit range
        for (uint32_t i = 0; i < samples; i++) {
            out[i] = (int16_t)(in[i] >> 4);
        }
    } else if (bps == 12) {
        // 12-bit FLAC: already in 12-bit range, just cast
        for (uint32_t i = 0; i < samples; i++) {
            out[i] = (int16_t)in[i];
        }
    } else if (bps == 8) {
        // 8-bit FLAC: keep in 8-bit range, don't scale up
        for (uint32_t i = 0; i < samples; i++) {
            out[i] = (int16_t)in[i];
        }
    } else {
        // Unknown bit depth - just cast
        for (uint32_t i = 0; i < samples; i++) {
            out[i] = (int16_t)in[i];
        }
    }

    ctx->decode_available_b += samples;
    return FLAC__STREAM_DECODER_WRITE_STATUS_CONTINUE;
}

// Metadata callback (extracts stream info)
static void decoder_metadata_cb_a(
    const FLAC__StreamDecoder *decoder,
    const FLAC__StreamMetadata *metadata,
    void *client_data)
{
    (void)decoder;
    playback_ctx_t *ctx = (playback_ctx_t *)client_data;

    if (metadata->type == FLAC__METADATA_TYPE_STREAMINFO) {
        ctx->info_a.sample_rate = metadata->data.stream_info.sample_rate;
        ctx->info_a.bits_per_sample = metadata->data.stream_info.bits_per_sample;
        ctx->info_a.total_samples = metadata->data.stream_info.total_samples;
        ctx->stream_a.min_block_size = metadata->data.stream_info.min_blocksize;
        ctx->stream_a.max_block_size = metadata->data.stream_info.max_blocksize;
        ctx->stream_a.min_frame_size = metadata->data.stream_info.min_framesize;
        ctx->stream_a.max_frame_size = metadata->data.stream_info.max_framesize;
        ctx->stream_a.channels = metadata->data.stream_info.channels;
        if (ctx->info_a.sample_rate > 0) {
            ctx->info_a.duration_seconds = (double)ctx->info_a.total_samples / ctx->info_a.sample_rate;
        }
        ctx->info_a.valid = true;
        fprintf(stderr, "[PLAYBACK] File A header: %u Hz, %u-bit, %u ch, %" PRIu64 " samples\n",
                ctx->info_a.sample_rate, ctx->info_a.bits_per_sample,
                ctx->stream_a.channels, ctx->info_a.total_samples);
    }
}

static void decoder_metadata_cb_b(
    const FLAC__StreamDecoder *decoder,
    const FLAC__StreamMetadata *metadata,
    void *client_data)
{
    (void)decoder;
    playback_ctx_t *ctx = (playback_ctx_t *)client_data;

    if (metadata->type == FLAC__METADATA_TYPE_STREAMINFO) {
        ctx->info_b.sample_rate = metadata->data.stream_info.sample_rate;
        ctx->info_b.bits_per_sample = metadata->data.stream_info.bits_per_sample;
        ctx->info_b.total_samples = metadata->data.stream_info.total_samples;
        ctx->stream_b.min_block_size = metadata->data.stream_info.min_blocksize;
        ctx->stream_b.max_block_size = metadata->data.stream_info.max_blocksize;
        ctx->stream_b.min_frame_size = metadata->data.stream_info.min_framesize;
        ctx->stream_b.max_frame_size = metadata->data.stream_info.max_framesize;
        ctx->stream_b.channels = metadata->data.stream_info.channels;
        if (ctx->info_b.sample_rate > 0) {
            ctx->info_b.duration_seconds = (double)ctx->info_b.total_samples / ctx->info_b.sample_rate;
        }
        ctx->info_b.valid = true;
        fprintf(stderr, "[PLAYBACK] File B header: %u Hz, %u-bit, %u ch, %" PRIu64 " samples\n",
                ctx->info_b.sample_rate, ctx->info_b.bits_per_sample,
                ctx->stream_b.channels, ctx->info_b.total_samples);
    }
}

// Error callback
static void decoder_error_cb(
    const FLAC__StreamDecoder *decoder,
    FLAC__StreamDecoderErrorStatus status,
    void *client_data)
{
    (void)decoder;
    (void)client_data;
    fprintf(stderr, "[PLAYBACK] FLAC decoder error: %s\n",
            FLAC__StreamDecoderErrorStatusString[status]);
}

#endif // LIBFLAC_ENABLED

//-----------------------------------------------------------------------------
// File Validation
//-----------------------------------------------------------------------------

bool gui_playback_validate_file(const char *filepath, playback_file_info_t *info) {
    if (!filepath || !info) return false;

    memset(info, 0, sizeof(*info));
    strncpy(info->filepath, filepath, sizeof(info->filepath) - 1);

#if LIBFLAC_ENABLED == 1
    // Use FLAC metadata API to read stream info without full decode
    // FLAC__metadata_get_streaminfo fills in an existing struct
    FLAC__StreamMetadata metadata;

    if (!FLAC__metadata_get_streaminfo(filepath, &metadata)) {
        fprintf(stderr, "[PLAYBACK] Failed to read FLAC metadata from: %s\n", filepath);
        return false;
    }

    FLAC__StreamMetadata_StreamInfo *si = &metadata.data.stream_info;
    playback_streaminfo_aux_t stream = {
        .min_block_size = si->min_blocksize,
        .max_block_size = si->max_blocksize,
        .min_frame_size = si->min_framesize,
        .max_frame_size = si->max_framesize,
        .channels = si->channels
    };

    info->sample_rate = si->sample_rate;
    info->bits_per_sample = si->bits_per_sample;
    info->total_samples = si->total_samples;
    playback_total_source_t source = PLAYBACK_TOTAL_SOURCE_UNRESOLVED;
    double real_rate_hz = 0.0;
    uint64_t resolved_total = gui_playback_resolve_total_samples(
        filepath,
        info->sample_rate,
        info->bits_per_sample,
        info->total_samples,
        &stream,
        &source,
        &real_rate_hz);
    if (resolved_total > 0) {
        info->total_samples = resolved_total;
    }

    if (real_rate_hz <= 0.0) {
        real_rate_hz = gui_playback_resolve_real_rate_hz(filepath, info->sample_rate, NULL);
    }
    if (real_rate_hz > 0.0 && info->total_samples > 0) {
        info->duration_seconds = (double)info->total_samples / real_rate_hz;
    } else {
        info->duration_seconds = 0.0;
    }
    fprintf(stderr,
            "[PLAYBACK] Validate: resolved total=%" PRIu64 " (source=%s, real_rate=%.0fHz, %.2f sec)\n",
            info->total_samples,
            gui_playback_total_source_name(source),
            real_rate_hz,
            info->duration_seconds);

    // Validate compatibility with MISRC format
    // Expected: 40kHz, 8/12/16-bit, mono
    bool compatible = true;

    if (info->sample_rate != 40000) {
        fprintf(stderr, "[PLAYBACK] Warning: Sample rate %u Hz (expected 40000 Hz)\n", info->sample_rate);
        // Allow non-40kHz files, but warn
    }

    if (stream.channels != 1) {
        fprintf(stderr, "[PLAYBACK] Error: FLAC has %u channels (expected mono)\n", stream.channels);
        compatible = false;
    }

    if (info->bits_per_sample != 8 && info->bits_per_sample != 12 && info->bits_per_sample != 16) {
        fprintf(stderr, "[PLAYBACK] Warning: Bit depth %u (expected 8, 12, or 16)\n", info->bits_per_sample);
        // Still allow, will attempt conversion
    }

    info->valid = compatible;
    return compatible;

#else
    fprintf(stderr, "[PLAYBACK] FLAC support not compiled in\n");
    return false;
#endif
}

//-----------------------------------------------------------------------------
// Playback Thread
//-----------------------------------------------------------------------------

// Raw buffer write size (must match extraction thread's expected read size)
#define RAW_BUFFER_SAMPLES 65536
#define RAW_BUFFER_BYTES (RAW_BUFFER_SAMPLES * sizeof(uint32_t))

static int playback_thread_func(void *ctx_ptr) {
    playback_ctx_t *ctx = (playback_ctx_t *)ctx_ptr;
    gui_app_t *app = ctx->app;
    thrd_set_priority(THRD_PRIORITY_CRITICAL);

    fprintf(stderr, "[PLAYBACK] Playback thread started (writing to BUF_CAPTURE_RF)\n");

    // Allocate decode buffers for FLAC output
    int16_t *buf_a = (int16_t *)malloc(RAW_BUFFER_SAMPLES * sizeof(int16_t));
    int16_t *buf_b = (int16_t *)malloc(RAW_BUFFER_SAMPLES * sizeof(int16_t));

    if (!buf_a || !buf_b) {
        fprintf(stderr, "[PLAYBACK] Failed to allocate output buffers\n");
        free(buf_a);
        free(buf_b);
        atomic_store(&ctx->state, PLAYBACK_STATE_STOPPED);
        return -1;
    }

    atomic_store(&app->stream_synced, true);
    uint32_t runtime_rate_khz = (uint32_t)llround(ctx->realtime_sample_rate_hz / 1000.0);
    if (runtime_rate_khz == 0) runtime_rate_khz = PLAYBACK_SAMPLE_RATE;
    atomic_store(&app->sample_rate, runtime_rate_khz);

    uint64_t batch_count = 0;
    const uint32_t raw_silence = encode_raw_sample(0, 0);

    while (atomic_load(&ctx->running)) {
        // Check for seek requests (must work even while paused)
        if (atomic_load(&ctx->seek_requested_a)) {
            uint64_t target = atomic_load(&ctx->seek_target_a);
            uint64_t total_a = ctx->info_a.total_samples;
            if (total_a > 0 && target >= total_a) {
                target = total_a - 1;
            }
            atomic_store(&ctx->seek_requested_a, false);

        // 08.2024 - Windows no flac output issue fix     
#if LIBFLAC_ENABLED == 1
            if (ctx->decoder_a) {
                FLAC__stream_decoder_seek_absolute(ctx->decoder_a, target);
                ctx->decode_available_a = 0;
                ctx->decode_pos_a = 0;
            }
#endif
            atomic_store(&ctx->current_sample_a, target);
            if (!ctx->file_b) {
                atomic_store(&ctx->current_sample, target);
            }
            continue;
        }

        if (atomic_load(&ctx->seek_requested_b)) {
            uint64_t target = atomic_load(&ctx->seek_target_b);
            uint64_t total_b = ctx->info_b.total_samples;
            if (total_b > 0 && target >= total_b) {
                target = total_b - 1;
            }
            atomic_store(&ctx->seek_requested_b, false);

#if LIBFLAC_ENABLED == 1
            if (ctx->decoder_b) {
                FLAC__stream_decoder_seek_absolute(ctx->decoder_b, target);
                ctx->decode_available_b = 0;
                ctx->decode_pos_b = 0;
            }
#endif
            atomic_store(&ctx->current_sample_b, target);
            if (!ctx->file_a) {
                atomic_store(&ctx->current_sample, target);
            }
            continue;
        }

        // Check for pause state
        if (atomic_load(&ctx->state) == PLAYBACK_STATE_PAUSED) {
            thrd_sleep_ms(10);
            continue;
        }

        // Decode more samples if needed
#if LIBFLAC_ENABLED == 1
        // Decode channel A
        while (ctx->decoder_a && ctx->decode_available_a - ctx->decode_pos_a < RAW_BUFFER_SAMPLES) {
            if (FLAC__stream_decoder_get_state(ctx->decoder_a) == FLAC__STREAM_DECODER_END_OF_STREAM) {
                break;
            }
            if (!FLAC__stream_decoder_process_single(ctx->decoder_a)) {
                break;
            }
        }

        // Decode channel B
        while (ctx->decoder_b && ctx->decode_available_b - ctx->decode_pos_b < RAW_BUFFER_SAMPLES) {
            if (FLAC__stream_decoder_get_state(ctx->decoder_b) == FLAC__STREAM_DECODER_END_OF_STREAM) {
                break;
            }
            if (!FLAC__stream_decoder_process_single(ctx->decoder_b)) {
                break;
            }
        }
#endif

        // Calculate how many samples we can output
        size_t avail_a = ctx->decode_available_a - ctx->decode_pos_a;
        size_t avail_b = ctx->decode_available_b - ctx->decode_pos_b;
        bool channel_a_loaded = (ctx->file_a != NULL);
        bool channel_b_loaded = (ctx->file_b != NULL);
        size_t samples_to_output = RAW_BUFFER_SAMPLES;

        // Check for EOF on each channel independently and loop each one
#if LIBFLAC_ENABLED == 1
        bool eof_a = (ctx->decoder_a != NULL) && (avail_a == 0 &&
                      FLAC__stream_decoder_get_state(ctx->decoder_a) == FLAC__STREAM_DECODER_END_OF_STREAM);
        bool eof_b = (ctx->decoder_b != NULL) && (avail_b == 0 &&
                      FLAC__stream_decoder_get_state(ctx->decoder_b) == FLAC__STREAM_DECODER_END_OF_STREAM);

        // Loop channel A independently
        if (eof_a) {
            fprintf(stderr, "[PLAYBACK] Channel A EOF, looping\n");
            FLAC__stream_decoder_seek_absolute(ctx->decoder_a, 0);
            ctx->decode_available_a = 0;
            ctx->decode_pos_a = 0;
            atomic_store(&ctx->current_sample_a, 0);
        }

        // Loop channel B independently
        if (eof_b) {
            fprintf(stderr, "[PLAYBACK] Channel B EOF, looping\n");
            FLAC__stream_decoder_seek_absolute(ctx->decoder_b, 0);
            ctx->decode_available_b = 0;
            ctx->decode_pos_b = 0;
            atomic_store(&ctx->current_sample_b, 0);
        }

        // If either channel just looped, continue to refill buffers
        if (eof_a || eof_b) {
            continue;
        }
#endif

        // Limit output to available samples
        if (channel_a_loaded && avail_a < samples_to_output) {
            samples_to_output = avail_a;
        }
        if (channel_b_loaded && avail_b < samples_to_output) {
            samples_to_output = avail_b;
        }

        if (samples_to_output == 0) {
            thrd_sleep_ms(1);
            continue;
        }

        // Fill output buffers
        if (channel_a_loaded && avail_a > 0) {
            size_t to_copy = (avail_a < samples_to_output) ? avail_a : samples_to_output;
            memcpy(buf_a, ctx->decode_buf_a + ctx->decode_pos_a, to_copy * sizeof(int16_t));
            ctx->decode_pos_a += to_copy;
            // Zero-pad if needed
            if (to_copy < samples_to_output) {
                memset(buf_a + to_copy, 0, (samples_to_output - to_copy) * sizeof(int16_t));
            }
        } else {
            memset(buf_a, 0, samples_to_output * sizeof(int16_t));
        }

        if (channel_b_loaded && avail_b > 0) {
            size_t to_copy = (avail_b < samples_to_output) ? avail_b : samples_to_output;
            memcpy(buf_b, ctx->decode_buf_b + ctx->decode_pos_b, to_copy * sizeof(int16_t));
            ctx->decode_pos_b += to_copy;
            if (to_copy < samples_to_output) {
                memset(buf_b + to_copy, 0, (samples_to_output - to_copy) * sizeof(int16_t));
            }
        } else {
            memset(buf_b, 0, samples_to_output * sizeof(int16_t));
        }

        // Compact decode buffers if needed (shift remaining data to start)
        if (channel_a_loaded && ctx->decode_pos_a > ctx->decode_buf_size / 2) {
            size_t remaining = ctx->decode_available_a - ctx->decode_pos_a;
            if (remaining > 0) {
                memmove(ctx->decode_buf_a, ctx->decode_buf_a + ctx->decode_pos_a,
                        remaining * sizeof(int16_t));
            }
            ctx->decode_available_a = remaining;
            ctx->decode_pos_a = 0;
        }
        if (channel_b_loaded && ctx->decode_pos_b > ctx->decode_buf_size / 2) {
            size_t remaining = ctx->decode_available_b - ctx->decode_pos_b;
            if (remaining > 0) {
                memmove(ctx->decode_buf_b, ctx->decode_buf_b + ctx->decode_pos_b,
                        remaining * sizeof(int16_t));
            }
            ctx->decode_available_b = remaining;
            ctx->decode_pos_b = 0;
        }

        // Encode decoded samples to raw 32-bit format and write to BUF_CAPTURE_RF
        // The extraction thread will read this, update statistics, display, CVBS, and recording
        uint32_t *raw_buf = (uint32_t *)bufmgr_write_begin(&app->buffers, BUF_CAPTURE_RF,
                                                           RAW_BUFFER_BYTES, NULL);
        if (raw_buf) {
            // Encode int16 samples to raw format
            for (size_t i = 0; i < samples_to_output; i++) {
                raw_buf[i] = encode_raw_sample(buf_a[i], buf_b[i]);
            }
            for (size_t i = samples_to_output; i < RAW_BUFFER_SAMPLES; i++) {
                raw_buf[i] = raw_silence;
            }
            bufmgr_write_end(&app->buffers, BUF_CAPTURE_RF, RAW_BUFFER_BYTES);
        } else {
            // Buffer full - this shouldn't happen often with proper sizing
            // The extraction thread may be slow or not running
            fprintf(stderr, "[PLAYBACK] Warning: BUF_CAPTURE_RF full, frame dropped\n");
        }

        // Update playback position (extraction thread updates total_samples, etc.)
        if (channel_a_loaded) {
            uint64_t total_a = ctx->info_a.total_samples;
            uint64_t next_a = atomic_load(&ctx->current_sample_a) + (uint64_t)samples_to_output;
            if (total_a > 0 && next_a >= total_a) {
                next_a %= total_a;
            }
            atomic_store(&ctx->current_sample_a, next_a);
        }
        if (channel_b_loaded) {
            uint64_t total_b = ctx->info_b.total_samples;
            uint64_t next_b = atomic_load(&ctx->current_sample_b) + (uint64_t)samples_to_output;
            if (total_b > 0 && next_b >= total_b) {
                next_b %= total_b;
            }
            atomic_store(&ctx->current_sample_b, next_b);
        }
        if (channel_a_loaded) {
            atomic_store(&ctx->current_sample, atomic_load(&ctx->current_sample_a));
        } else if (channel_b_loaded) {
            atomic_store(&ctx->current_sample, atomic_load(&ctx->current_sample_b));
        } else {
            atomic_fetch_add(&ctx->current_sample, samples_to_output);
        }
        atomic_store(&app->last_callback_time_ms, get_time_ms());

        batch_count++;

        // Throttle based on playback speed
        playback_speed_t speed = atomic_load(&ctx->speed);
        if (speed != PLAYBACK_SPEED_MAX) {
            float multiplier = speed_multipliers[speed];
            if (multiplier > 0) {
                // Real-time pacing at the resolved true sample rate.
                double rate_hz = ctx->realtime_sample_rate_hz;
                if (!(rate_hz > 0.0)) {
                    rate_hz = (double)PLAYBACK_SAMPLE_RATE * 1000.0;
                }
                float real_time_ms = (float)(((double)samples_to_output * 1000.0) / rate_hz);
                uint32_t delay_ms = (uint32_t)(real_time_ms / multiplier);
                if (delay_ms > 0) {
                    thrd_sleep_ms(delay_ms);
                }
            }
        }
    }

    fprintf(stderr, "[PLAYBACK] Playback thread exiting after %llu batches\n",
            (unsigned long long)batch_count);

    free(buf_a);
    free(buf_b);

    return 0;
}

//-----------------------------------------------------------------------------
// Public API
//-----------------------------------------------------------------------------

int gui_playback_start(gui_app_t *app, const char *file_a, const char *file_b) {
    if (!app) return -1;
    if (!file_a && !file_b) {
        fprintf(stderr, "[PLAYBACK] No files specified\n");
        return -1;
    }

#if LIBFLAC_ENABLED != 1
    fprintf(stderr, "[PLAYBACK] FLAC support not compiled in\n");
    gui_app_set_status(app, "FLAC support not available");
    return -1;
#else

    // Stop any existing playback
    if (gui_playback_is_running(app)) {
        gui_playback_stop(app);
    }

    fprintf(stderr, "[PLAYBACK] Starting playback\n");
    if (file_a) fprintf(stderr, "[PLAYBACK]   Channel A: %s\n", file_a);
    if (file_b) fprintf(stderr, "[PLAYBACK]   Channel B: %s\n", file_b);

    // Reset state
    memset(&s_playback, 0, sizeof(s_playback));
    s_playback.app = app;

    // Allocate decode buffers (larger than output buffer to allow streaming)
    s_playback.decode_buf_size = PLAYBACK_BUFFER_SIZE * 8;
    s_playback.decode_buf_a = (int16_t *)malloc(s_playback.decode_buf_size * sizeof(int16_t));
    s_playback.decode_buf_b = (int16_t *)malloc(s_playback.decode_buf_size * sizeof(int16_t));

    if (!s_playback.decode_buf_a || !s_playback.decode_buf_b) {
        fprintf(stderr, "[PLAYBACK] Failed to allocate decode buffers\n");
        free(s_playback.decode_buf_a);
        free(s_playback.decode_buf_b);
        return -1;
    }

    // Open and initialize decoders
    if (file_a) {
        strncpy(s_playback.info_a.filepath, file_a, sizeof(s_playback.info_a.filepath) - 1);

        s_playback.file_a = fopen(file_a, "rb");
        if (!s_playback.file_a) {
            fprintf(stderr, "[PLAYBACK] Failed to open file A: %s\n", file_a);
            goto error_cleanup;
        }

        s_playback.decoder_a = FLAC__stream_decoder_new();
        if (!s_playback.decoder_a) {
            fprintf(stderr, "[PLAYBACK] Failed to create decoder A\n");
            goto error_cleanup;
        }

        FLAC__stream_decoder_set_md5_checking(s_playback.decoder_a, false);

        FLAC__StreamDecoderInitStatus status = FLAC__stream_decoder_init_FILE(
            s_playback.decoder_a,
            s_playback.file_a,
            decoder_write_cb_a,
            decoder_metadata_cb_a,
            decoder_error_cb,
            &s_playback
        );

        if (status != FLAC__STREAM_DECODER_INIT_STATUS_OK) {
            fprintf(stderr, "[PLAYBACK] Failed to init decoder A: %s\n",
                    FLAC__StreamDecoderInitStatusString[status]);
            goto error_cleanup;
        }

        // Process metadata to get file info
        FLAC__stream_decoder_process_until_end_of_metadata(s_playback.decoder_a);
        gui_playback_apply_resolved_file_info(&s_playback.info_a, &s_playback.stream_a, "File A");
    }

    if (file_b) {
        strncpy(s_playback.info_b.filepath, file_b, sizeof(s_playback.info_b.filepath) - 1);

        s_playback.file_b = fopen(file_b, "rb");
        if (!s_playback.file_b) {
            fprintf(stderr, "[PLAYBACK] Failed to open file B: %s\n", file_b);
            goto error_cleanup;
        }

        s_playback.decoder_b = FLAC__stream_decoder_new();
        if (!s_playback.decoder_b) {
            fprintf(stderr, "[PLAYBACK] Failed to create decoder B\n");
            goto error_cleanup;
        }

        FLAC__stream_decoder_set_md5_checking(s_playback.decoder_b, false);

        FLAC__StreamDecoderInitStatus status = FLAC__stream_decoder_init_FILE(
            s_playback.decoder_b,
            s_playback.file_b,
            decoder_write_cb_b,
            decoder_metadata_cb_b,
            decoder_error_cb,
            &s_playback
        );

        if (status != FLAC__STREAM_DECODER_INIT_STATUS_OK) {
            fprintf(stderr, "[PLAYBACK] Failed to init decoder B: %s\n",
                    FLAC__StreamDecoderInitStatusString[status]);
            goto error_cleanup;
        }

        FLAC__stream_decoder_process_until_end_of_metadata(s_playback.decoder_b);
        gui_playback_apply_resolved_file_info(&s_playback.info_b, &s_playback.stream_b, "File B");
    }

    // Calculate total samples (max of both channels)
    s_playback.total_samples = s_playback.info_a.total_samples;
    if (s_playback.info_b.total_samples > s_playback.total_samples) {
        s_playback.total_samples = s_playback.info_b.total_samples;
    }

    double rate_a_hz = gui_playback_real_rate_from_info(&s_playback.info_a);
    double rate_b_hz = gui_playback_real_rate_from_info(&s_playback.info_b);
    s_playback.realtime_sample_rate_hz = 0.0;
    if (rate_a_hz > 0.0) {
        s_playback.realtime_sample_rate_hz = rate_a_hz;
    }
    if (rate_b_hz > 0.0 &&
        (s_playback.realtime_sample_rate_hz <= 0.0 || rate_b_hz > s_playback.realtime_sample_rate_hz)) {
        s_playback.realtime_sample_rate_hz = rate_b_hz;
    }
    if (s_playback.realtime_sample_rate_hz <= 0.0) {
        s_playback.realtime_sample_rate_hz = (double)PLAYBACK_SAMPLE_RATE * 1000.0;
    }
    if (s_playback.total_samples > 0) {
        s_playback.total_duration_seconds = (double)s_playback.total_samples / s_playback.realtime_sample_rate_hz;
    } else {
        s_playback.total_duration_seconds = 0.0;
    }
    fprintf(stderr,
            "[PLAYBACK] Runtime rate=%.0fHz total_samples=%" PRIu64 " duration=%.2f sec\n",
            s_playback.realtime_sample_rate_hz,
            s_playback.total_samples,
            s_playback.total_duration_seconds);
    bufmgr_reset_stats(&app->buffers, BUF_COUNT);

    // Reset app statistics
    atomic_store(&app->total_samples, 0);
    atomic_store(&app->samples_a, 0);
    atomic_store(&app->samples_b, 0);
    atomic_store(&app->frame_count, 0);
    atomic_store(&app->missed_frame_count, 0);
    atomic_store(&app->error_count, 0);
    atomic_store(&app->parser_error_count, 0);
    atomic_store(&app->system_error_count, 0);
    atomic_store(&app->error_count_a, 0);
    atomic_store(&app->error_count_b, 0);
    atomic_store(&app->clip_count_a_pos, 0);
    atomic_store(&app->clip_count_a_neg, 0);
    atomic_store(&app->clip_count_b_pos, 0);
    atomic_store(&app->clip_count_b_neg, 0);
    atomic_store(&app->rb_wait_count, 0);
    atomic_store(&app->rb_drop_count, 0);
    atomic_store(&app->stream_synced, false);
    uint32_t playback_rate_khz = (uint32_t)llround(s_playback.realtime_sample_rate_hz / 1000.0);
    if (playback_rate_khz == 0) playback_rate_khz = PLAYBACK_SAMPLE_RATE;
    atomic_store(&app->sample_rate, playback_rate_khz);
    atomic_store(&app->last_callback_time_ms, get_time_ms());
    app->capture_backend_upstream = false;
    app->capture_has_channel_b = (s_playback.file_b != NULL);
    app->capture_mode_runtime_misrc = app->user_capture_mode_misrc;
    app->capture_start_time = GetTime();
    app->reconnect_pending = false;
    app->reconnect_attempts = 0;
    {
        time_t t = time(NULL);
        struct tm tmv;
#if defined(_WIN32) || defined(_WIN64)
        localtime_s(&tmv, &t);
#else
        localtime_r(&t, &tmv);
#endif
        snprintf(app->capture_timestamp, sizeof(app->capture_timestamp), "%04d.%02d.%02d_%02d.%02d.%02d",
                 (tmv.tm_year + 1900),
                 tmv.tm_mon + 1,
                 tmv.tm_mday,
                 tmv.tm_hour,
                 tmv.tm_min,
                 tmv.tm_sec);
    }

    app->display_samples_available_a = 0;
    app->display_samples_available_b = 0;

    // Initialize playback state
    atomic_store(&s_playback.state, PLAYBACK_STATE_PLAYING);
    atomic_store(&s_playback.speed, PLAYBACK_SPEED_1X);
    atomic_store(&s_playback.loop_enabled, true);  // Loop by default
    atomic_store(&s_playback.seek_requested_a, false);
    atomic_store(&s_playback.seek_requested_b, false);
    atomic_store(&s_playback.seek_target_a, 0);
    atomic_store(&s_playback.seek_target_b, 0);
    atomic_store(&s_playback.current_sample, 0);
    atomic_store(&s_playback.current_sample_a, 0);
    atomic_store(&s_playback.current_sample_b, 0);
    atomic_store(&s_playback.running, true);

    app->is_capturing = true;

    // Start extraction thread - reads from BUF_CAPTURE_RF, writes to BUF_DISPLAY
    // Also handles statistics, peak detection, and recording if enabled
    int r = gui_extract_start(app);
    if (r < 0) {
        fprintf(stderr, "[PLAYBACK] Failed to start extraction thread\n");
        gui_app_set_status(app, "Failed to start extraction");
        goto error_cleanup;
    }

    // Start display thread - processes BUF_DISPLAY for oscilloscope/CVBS
    if (app->display_thread) {
        r = gui_display_thread_start(app->display_thread, app, &app->buffers);
        if (r < 0) {
            fprintf(stderr, "[PLAYBACK] Failed to start display thread (non-fatal)\n");
            // Non-fatal - display will use legacy path
        }
    }

    // Start playback thread - decodes FLAC and writes to BUF_CAPTURE_RF
    thrd_t thread;
    if (thrd_create_with_priority(&thread,
                                  playback_thread_func,
                                  &s_playback,
                                  THRD_PRIORITY_CRITICAL) != thrd_success) {
        fprintf(stderr, "[PLAYBACK] Failed to create playback thread\n");
        // Stop extraction thread before cleanup
        gui_extract_stop();
        if (app->display_thread) {
            gui_display_thread_stop(app->display_thread);
        }
        app->is_capturing = false;
        goto error_cleanup;
    }
    s_playback.playback_thread = (void *)(uintptr_t)thread;

    gui_app_set_status(app, "Playback started");

    return 0;

error_cleanup:
    if (s_playback.decoder_a) {
        FLAC__stream_decoder_delete(s_playback.decoder_a);
        s_playback.decoder_a = NULL;
    }
    if (s_playback.decoder_b) {
        FLAC__stream_decoder_delete(s_playback.decoder_b);
        s_playback.decoder_b = NULL;
    }
    if (s_playback.file_a) {
        fclose(s_playback.file_a);
        s_playback.file_a = NULL;
    }
    if (s_playback.file_b) {
        fclose(s_playback.file_b);
        s_playback.file_b = NULL;
    }
    free(s_playback.decode_buf_a);
    free(s_playback.decode_buf_b);
    s_playback.decode_buf_a = NULL;
    s_playback.decode_buf_b = NULL;
    app->is_capturing = false;
    app->capture_timestamp[0] = '\0';
    app->capture_backend_upstream = false;
    app->capture_has_channel_b = true;

    gui_app_set_status(app, "Playback failed to start");
    return -1;

#endif // LIBFLAC_ENABLED
}

void gui_playback_stop(gui_app_t *app) {
    if (!atomic_load(&s_playback.running)) return;

    fprintf(stderr, "[PLAYBACK] Stopping playback\n");

    // Set is_capturing to false BEFORE stopping extraction thread
    // The extraction thread checks this flag to know when to exit
    app->is_capturing = false;

    atomic_store(&s_playback.running, false);
    atomic_store(&s_playback.state, PLAYBACK_STATE_STOPPED);

    // Stop playback thread first (it writes to BUF_CAPTURE_RF)
    if (s_playback.playback_thread) {
        thrd_t thread = (thrd_t)(uintptr_t)s_playback.playback_thread;
        thrd_join(thread, NULL);
        s_playback.playback_thread = NULL;
    }

    // Stop display thread (reads from BUF_DISPLAY written by extraction)
    if (app->display_thread) {
        gui_display_thread_stop(app->display_thread);
    }

    // Stop extraction thread (reads BUF_CAPTURE_RF, writes BUF_DISPLAY)
    gui_extract_stop();

#if LIBFLAC_ENABLED == 1
    if (s_playback.decoder_a) {
        FLAC__stream_decoder_finish(s_playback.decoder_a);
        FLAC__stream_decoder_delete(s_playback.decoder_a);
        s_playback.decoder_a = NULL;
    }
    if (s_playback.decoder_b) {
        FLAC__stream_decoder_finish(s_playback.decoder_b);
        FLAC__stream_decoder_delete(s_playback.decoder_b);
        s_playback.decoder_b = NULL;
    }
#endif

    // Note: FILE handles are managed by FLAC decoder after init_FILE
    s_playback.file_a = NULL;
    s_playback.file_b = NULL;

    free(s_playback.decode_buf_a);
    free(s_playback.decode_buf_b);
    s_playback.decode_buf_a = NULL;
    s_playback.decode_buf_b = NULL;
    app->capture_timestamp[0] = '\0';
    app->capture_backend_upstream = false;
    app->capture_has_channel_b = true;

    atomic_store(&app->stream_synced, false);

    gui_app_set_status(app, "Playback stopped");
}

bool gui_playback_is_running(gui_app_t *app) {
    (void)app;
    return atomic_load(&s_playback.running);
}

playback_state_t gui_playback_get_state(gui_app_t *app) {
    (void)app;
    return (playback_state_t)atomic_load(&s_playback.state);
}

void gui_playback_pause(gui_app_t *app) {
    (void)app;
    if (atomic_load(&s_playback.state) == PLAYBACK_STATE_PLAYING) {
        atomic_store(&s_playback.state, PLAYBACK_STATE_PAUSED);
    }
}

void gui_playback_resume(gui_app_t *app) {
    (void)app;
    if (atomic_load(&s_playback.state) == PLAYBACK_STATE_PAUSED) {
        atomic_store(&s_playback.state, PLAYBACK_STATE_PLAYING);
    }
}

void gui_playback_toggle_pause(gui_app_t *app) {
    playback_state_t state = gui_playback_get_state(app);
    if (state == PLAYBACK_STATE_PLAYING) {
        gui_playback_pause(app);
    } else if (state == PLAYBACK_STATE_PAUSED) {
        gui_playback_resume(app);
    }
}

void gui_playback_seek_normalized(gui_app_t *app, double position) {
    if (position < 0.0) position = 0.0;
    if (position > 1.0) position = 1.0;
    if (s_playback.info_a.total_samples > 0) {
        uint64_t target_a = (uint64_t)floor(position * (double)s_playback.info_a.total_samples);
        if (target_a >= s_playback.info_a.total_samples) {
            target_a = s_playback.info_a.total_samples - 1;
        }
        gui_playback_seek_sample_channel(app, 0, target_a);
    }
    if (s_playback.info_b.total_samples > 0) {
        uint64_t target_b = (uint64_t)floor(position * (double)s_playback.info_b.total_samples);
        if (target_b >= s_playback.info_b.total_samples) {
            target_b = s_playback.info_b.total_samples - 1;
        }
        gui_playback_seek_sample_channel(app, 1, target_b);
    }
}

void gui_playback_seek_sample_channel(gui_app_t *app, int channel, uint64_t sample) {
    (void)app;
    bool channel_b = (channel == 1);
    uint64_t total_samples = channel_b ? s_playback.info_b.total_samples : s_playback.info_a.total_samples;
    if (total_samples == 0) {
        return;
    }
    if (sample >= total_samples) {
        sample = total_samples - 1;
    }
    if (channel_b) {
        atomic_store(&s_playback.seek_target_b, sample);
        atomic_store(&s_playback.seek_requested_b, true);
    } else {
        atomic_store(&s_playback.seek_target_a, sample);
        atomic_store(&s_playback.seek_requested_a, true);
    }
}

void gui_playback_seek_sample(gui_app_t *app, uint64_t sample) {
    (void)app;
    if (s_playback.info_a.total_samples > 0) {
        gui_playback_seek_sample_channel(app, 0, sample);
    }
    if (s_playback.info_b.total_samples > 0) {
        gui_playback_seek_sample_channel(app, 1, sample);
    }
    if (s_playback.info_a.total_samples == 0 && s_playback.info_b.total_samples == 0) {
        if (sample > s_playback.total_samples) {
            sample = s_playback.total_samples;
        }
        atomic_store(&s_playback.current_sample, sample);
    }
}

uint64_t gui_playback_get_position_samples_channel(gui_app_t *app, int channel) {
    (void)app;
    if (channel == 1) {
        return atomic_load(&s_playback.current_sample_b);
    }
    return atomic_load(&s_playback.current_sample_a);
}

uint64_t gui_playback_get_position_samples(gui_app_t *app) {
    (void)app;
    if (s_playback.file_a != NULL) {
        return atomic_load(&s_playback.current_sample_a);
    }
    if (s_playback.file_b != NULL) {
        return atomic_load(&s_playback.current_sample_b);
    }
    return atomic_load(&s_playback.current_sample);
}

double gui_playback_get_position_normalized(gui_app_t *app) {
    (void)app;
    if (s_playback.total_samples == 0) return 0.0;
    uint64_t sample = atomic_load(&s_playback.current_sample);
    if (sample >= s_playback.total_samples) {
        sample %= s_playback.total_samples;
    }
    return (double)sample / (double)s_playback.total_samples;
}

double gui_playback_get_position_seconds(gui_app_t *app) {
    (void)app;
    if (s_playback.total_samples == 0) return 0.0;
    uint64_t samples = atomic_load(&s_playback.current_sample);
    if (samples >= s_playback.total_samples) {
        samples %= s_playback.total_samples;
    }
    if (s_playback.total_duration_seconds > 0.0) {
        return ((double)samples / (double)s_playback.total_samples) * s_playback.total_duration_seconds;
    }
    if (s_playback.realtime_sample_rate_hz > 0.0) {
        return (double)samples / s_playback.realtime_sample_rate_hz;
    }
    return (double)samples / ((double)PLAYBACK_SAMPLE_RATE * 1000.0);
}

uint64_t gui_playback_get_total_samples(gui_app_t *app) {
    (void)app;
    return s_playback.total_samples;
}

double gui_playback_get_duration_seconds(gui_app_t *app) {
    (void)app;
    if (s_playback.total_duration_seconds > 0.0) {
        return s_playback.total_duration_seconds;
    }
    if (s_playback.realtime_sample_rate_hz > 0.0 && s_playback.total_samples > 0) {
        return (double)s_playback.total_samples / s_playback.realtime_sample_rate_hz;
    }
    return (double)s_playback.total_samples / ((double)PLAYBACK_SAMPLE_RATE * 1000.0);
}

void gui_playback_set_speed(gui_app_t *app, playback_speed_t speed) {
    (void)app;
    if (speed >= PLAYBACK_SPEED_COUNT) speed = PLAYBACK_SPEED_1X;
    atomic_store(&s_playback.speed, speed);
}

playback_speed_t gui_playback_get_speed(gui_app_t *app) {
    (void)app;
    return (playback_speed_t)atomic_load(&s_playback.speed);
}

bool gui_playback_get_file_info_a(gui_app_t *app, playback_file_info_t *info) {
    (void)app;
    if (!info) return false;
    *info = s_playback.info_a;
    return s_playback.info_a.valid;
}

bool gui_playback_get_file_info_b(gui_app_t *app, playback_file_info_t *info) {
    (void)app;
    if (!info) return false;
    *info = s_playback.info_b;
    return s_playback.info_b.valid;
}

void gui_playback_set_loop(gui_app_t *app, bool loop) {
    (void)app;
    atomic_store(&s_playback.loop_enabled, loop);
}

bool gui_playback_get_loop(gui_app_t *app) {
    (void)app;
    return atomic_load(&s_playback.loop_enabled);
}
