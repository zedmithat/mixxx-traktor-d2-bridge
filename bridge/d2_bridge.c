#define _DEFAULT_SOURCE

#include <stdio.h>
#include <stdint.h>
#include <errno.h>
#include <string.h>
#include <strings.h>
#include <stdlib.h>
#include <unistd.h>
#include <signal.h>
#include <sys/stat.h>
#include <time.h>
#include <math.h>
#include <pthread.h>
#include <sqlite3.h>
#include <alsa/asoundlib.h>
#include <ft2build.h>
#include FT_FREETYPE_H

#include "ctlra.h"
#include "devices/ni_kontrol_d2.h"

#define WIDTH 480
#define HEIGHT 272
#define D2_WAVEFORM_POINTS 32768
#define D2_WAVEFORM_HEIGHT 122
#define D2_MAX_BEATS 4096
#define D2_BROWSE_ROWS 9
#define D2_PHYSICAL_DECKS 2
#define D2_ASSET_SLOTS 5
#define D2_LOAD_SCRATCH(deck) ((deck) + D2_PHYSICAL_DECKS)
#define D2_VERBOSE 0
#define D2_EVENT_LOG(...) \
    do { if (D2_VERBOSE) fprintf(stdout, __VA_ARGS__); } while (0)
#define D2_EVENT_FLUSH() \
    do { if (D2_VERBOSE) fflush(stdout); } while (0)

static volatile int running = 1;
static volatile sig_atomic_t d2_capture_requested = 0;
static snd_seq_t *midi_seq = NULL;
static int midi_port = -1;
/* FreeType mutates both FT_Face and glyph slots while measuring/rendering.
 * The USB callback runs on the HID owner thread while the animated Deck is
 * produced on a worker, so every rendering thread owns an independent
 * FreeType library, face and glyph cache. No font mutex can stall input. */
static _Thread_local FT_Library d2_ft_library;
static _Thread_local FT_Face d2_ft_face;
static _Thread_local int d2_ft_ready = 0;

/* Every HUD glyph is cached once in its native alpha mask.  The live 60 Hz
 * path therefore only alpha-blends pixels; it never asks FreeType to rasterize
 * the same digits and labels on every frame. */
#define D2_FONT_SIZES 3
#define D2_FONT_CACHE_GLYPHS 128
#define D2_FONT_CACHE_SIDE 48
struct d2_cached_glyph {
    unsigned char bitmap[D2_FONT_CACHE_SIDE * D2_FONT_CACHE_SIDE];
    int width;
    int rows;
    int left;
    int top;
    int advance;
    int ready;
};
static _Thread_local struct d2_cached_glyph
    d2_font_cache[D2_FONT_SIZES][D2_FONT_CACHE_GLYPHS];

struct d2_screen_state {
    int track_id;
    float bpm;
    float position;
    float remaining;
    float duration;
    float rate;
    float beat_distance;
    float phase_master;
    float phase_active;
    int phase_master_step;
    int phase_active_step;
    int phase_master_deck;
    int phase_follower_deck;
    int phase_valid;
    float beat_prev_position;
    float beat_next_position;
    int beatgrid_valid;
    float beatgrid_first_position;
    float beatgrid_interval;
    int beatgrid_ready;
    float beatmap_position[D2_MAX_BEATS];
    int beatmap_source_index[D2_MAX_BEATS];
    int beatmap_count;
    int beatmap_ready;
    int zoom_level;
    int time_mode;
    float loop_size;
    int quantize;
    int keylock;
    int beatgrid_edit;
    int visual_key;
    int fx_touch_mask;
    float fx_parameter[4];
    int fx_enabled[4];
    int fx_selection[3]; /* Mixxx loaded_effect index, 0 means no effect */
    char fx_name[3][64]; /* authoritative EffectManifest display names */
    int stem_count;
    float stem_volume[4];
    int stem_muted[4];
    int browse_focus; /* 0 = track list, 1 = library tree */
    int browse_sort_column; /* Mixxx TrackModel::SortColumnId */
    int browse_sort_order;  /* 0 = ascending, 1 = descending */
    float hotcue_position[8];
    uint32_t hotcue_color[8];
    int playing;
    struct timespec position_updated_at;
    char title[80];
    char artist[80];
    char musical_key[16];
    char location[512];
};

static struct d2_screen_state d2_screen_state[D2_ASSET_SLOTS] = {
    {0},
    {.rate = 1.0f, .zoom_level = 2,
     .loop_size = 4.0f,
     .browse_sort_column = 2,
     .stem_volume = {1,1,1,1}, .title = "DECK 1", .artist = "MIXXX",
     .hotcue_position = {-1,-1,-1,-1,-1,-1,-1,-1}},
    {.rate = 1.0f, .zoom_level = 2,
     .loop_size = 4.0f,
     .browse_sort_column = 2,
     .stem_volume = {1,1,1,1}, .title = "DECK 2", .artist = "MIXXX",
     .hotcue_position = {-1,-1,-1,-1,-1,-1,-1,-1}},
};
/* Individual FX touch masks are transient; 0x0f is the deliberate persistent
 * FX Settings view opened by FX SELECT and closes on the next press. */
static uint64_t d2_fx_touch_updated_us[3] = {0, 0, 0};

struct d2_led_state {
    int outputs_enabled;
    int play;
    int cue;
    int sync;
    int flux;
    int shift;
    int loop;
    int active_channel;
    int mode; /* 1 Hotcue, 2 Loop, 3 Freeze, 4 Sampler, 5 Beatjump */
    int fx_unit;
    int fx_assign_mask;
    int fx_enabled[4];
    int performance_on[4];
    uint32_t pad_rgb[8];
};

static struct d2_led_state d2_led_state[3] = {
    {0},
    {.outputs_enabled = 1, .active_channel = 1, .mode = 1, .fx_unit = 1,
     .performance_on = {1, 1, 1, 1}},
    {.outputs_enabled = 1, .active_channel = 2, .mode = 1, .fx_unit = 2,
     .performance_on = {1, 1, 1, 1}},
};

static uint64_t d2_monotonic_us(void)
{
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (uint64_t)ts.tv_sec * 1000000ULL +
           (uint64_t)ts.tv_nsec / 1000ULL;
}
/* Immutable track assets are built off-thread and published as one pointer.
 * Compatibility pointers keep the renderer compact while each worker writes
 * only to its private scratch slot. */
struct d2_track_assets {
    uint8_t waveform[D2_WAVEFORM_POINTS];
    uint8_t waveform_low[D2_WAVEFORM_POINTS];
    uint8_t waveform_mid[D2_WAVEFORM_POINTS];
    uint8_t waveform_high[D2_WAVEFORM_POINTS];
    uint8_t wave_strip[D2_WAVEFORM_HEIGHT][D2_WAVEFORM_POINTS * 2];
    uint8_t cover_art[48 * 48 * 2];
    int waveform_ready;
    int wave_strip_ready;
    int cover_art_ready;
};

static struct d2_track_assets *d2_track_assets[3] = {NULL, NULL, NULL};
static uint8_t *d2_waveform[D2_ASSET_SLOTS] = {NULL};
static uint8_t *d2_waveform_low[D2_ASSET_SLOTS] = {NULL};
static uint8_t *d2_waveform_mid[D2_ASSET_SLOTS] = {NULL};
static uint8_t *d2_waveform_high[D2_ASSET_SLOTS] = {NULL};
static uint8_t (*d2_wave_strip[D2_ASSET_SLOTS])[D2_WAVEFORM_POINTS * 2] = {NULL};
static uint8_t *d2_cover_art[D2_ASSET_SLOTS] = {NULL};
static int d2_waveform_ready[D2_ASSET_SLOTS] = {0};
static int d2_wave_strip_ready[D2_ASSET_SLOTS] = {0};
static int d2_cover_art_ready[D2_ASSET_SLOTS] = {0};
/* Two native RGB565 framebuffers per physical display. The compositor never
 * draws into a buffer that is being copied to libctlra's USB transfer frame. */
static uint8_t d2_render_buffer[3][2][WIDTH * HEIGHT * 2];
static unsigned d2_render_buffer_index[3] = {0, 0, 0};
static int d2_render_buffer_ready[3] = {0, 0, 0};
static uint64_t d2_render_generation[3] = {0, 0, 0};
static uint64_t d2_usb_generation[3] = {0, 0, 0};
/* Exact copies of the last complete RGB565 frames handed to libctlra.  These
 * mirrors are diagnostic-only: capturing never reads from, interrupts or
 * fragments an in-flight USB transfer. */
static uint8_t d2_capture_frame[3][WIDTH * HEIGHT * 2];
static int d2_capture_frame_ready[3] = {0, 0, 0};
static pthread_mutex_t d2_frame_mutex = PTHREAD_MUTEX_INITIALIZER;
static pthread_mutex_t d2_state_mutex = PTHREAD_MUTEX_INITIALIZER;
static pthread_t d2_render_thread;
static int d2_render_thread_started = 0;

struct d2_track_load_job {
    int pending;
    int deck;
    int track_id;
    float duration;
    uint64_t generation;
    char location[512];
};

static struct d2_track_load_job d2_track_load_job[3];
static uint64_t d2_track_generation[3] = {0, 0, 0};
static pthread_mutex_t d2_track_load_mutex = PTHREAD_MUTEX_INITIALIZER;
static pthread_cond_t d2_track_load_cond = PTHREAD_COND_INITIALIZER;
static pthread_t d2_track_load_thread[3];
static int d2_track_load_thread_started[3] = {0, 0, 0};
static int d2_track_load_stop = 0;

static void d2_bind_asset_slot(int slot, struct d2_track_assets *assets)
{
    if (slot < 0 || slot >= D2_ASSET_SLOTS)
        return;
    d2_waveform[slot] = assets ? assets->waveform : NULL;
    d2_waveform_low[slot] = assets ? assets->waveform_low : NULL;
    d2_waveform_mid[slot] = assets ? assets->waveform_mid : NULL;
    d2_waveform_high[slot] = assets ? assets->waveform_high : NULL;
    d2_wave_strip[slot] = assets ? assets->wave_strip : NULL;
    d2_cover_art[slot] = assets ? assets->cover_art : NULL;
    d2_waveform_ready[slot] = assets ? assets->waveform_ready : 0;
    d2_wave_strip_ready[slot] = assets ? assets->wave_strip_ready : 0;
    d2_cover_art_ready[slot] = assets ? assets->cover_art_ready : 0;
}

static void d2_copy_text(char *destination, size_t destination_size,
                         const char *source)
{
    if (!destination || destination_size == 0)
        return;
    if (!source)
        source = "";
    size_t length = strnlen(source, destination_size - 1);
    memmove(destination, source, length);
    destination[length] = '\0';
}

static void d2_visible_deck_title(char *destination, size_t destination_size,
                                  const char *source)
{
    d2_copy_text(destination, destination_size, source);
    char *mix_suffix = strpbrk(destination, "([{");
    if (mix_suffix)
        *mix_suffix = '\0';
    size_t title_length = strlen(destination);
    while (title_length > 0 && destination[title_length - 1] == ' ')
        destination[--title_length] = '\0';
}
struct d2_browse_entry {
    int track_id;
    char title[80];
    char artist[64];
    char musical_key[12];
    float bpm;
    int rating;
    int available;
};
static struct d2_browse_entry d2_browse_entries[D2_BROWSE_ROWS] = {
    [D2_BROWSE_ROWS / 2] =
        {0, "NO TRACK SELECTED", "TURN BROWSE TO SELECT", "", 0.0f, 0, 0},
};

/* Browse metadata must never reopen the Mixxx database for every encoder
 * detent. Keep one read-only statement and a small LRU because moving a
 * nine-row window reuses eight of its nine tracks. */
#define D2_BROWSE_CACHE_ENTRIES 96
struct d2_browse_cache_entry {
    int track_id;
    struct d2_browse_entry entry;
    char location[512];
    uint64_t last_used;
};
static sqlite3 *d2_browse_db = NULL;
static sqlite3_stmt *d2_browse_stmt = NULL;
static struct d2_browse_cache_entry
    d2_browse_cache[D2_BROWSE_CACHE_ENTRIES];
static uint64_t d2_browse_cache_clock = 0;
static uint64_t d2_browse_generation = 1;
static uint64_t d2_browse_rendered_generation[3] = {0, 0, 0};
static int d2_browse_frame_valid[3] = {0, 0, 0};

/* A loaded TrackPointer may briefly have no library ID (notably restored and
 * file-browser loads).  Mixxx then sends its exact location in 7-bit-safe
 * chunks.  Keep assembly separate per physical deck so simultaneous loads
 * cannot cross-contaminate one another. */
#define D2_ENCODED_LOCATION_MAX 2048
static char d2_encoded_location[3][D2_ENCODED_LOCATION_MAX];
static size_t d2_encoded_location_length[3] = {0, 0, 0};
static int d2_encoded_location_valid[3] = {0, 0, 0};

static void d2_browse_mark_dirty(void)
{
    ++d2_browse_generation;
    if (d2_browse_generation == 0)
        d2_browse_generation = 1;
}

static int d2_hex_value(char value)
{
    if (value >= '0' && value <= '9') return value - '0';
    if (value >= 'A' && value <= 'F') return value - 'A' + 10;
    if (value >= 'a' && value <= 'f') return value - 'a' + 10;
    return -1;
}

static int d2_percent_decode(const char *source, char *destination,
                             size_t destination_size)
{
    size_t in = 0;
    size_t out = 0;

    if (!source || !destination || destination_size == 0)
        return 0;
    while (source[in] != '\0') {
        unsigned char decoded;
        if (source[in] == '%') {
            int high = d2_hex_value(source[in + 1]);
            int low = high >= 0 ? d2_hex_value(source[in + 2]) : -1;
            if (high < 0 || low < 0)
                return 0;
            decoded = (unsigned char)((high << 4) | low);
            in += 3;
        } else {
            decoded = (unsigned char)source[in++];
        }
        if (decoded == '\0' || out + 1 >= destination_size)
            return 0;
        destination[out++] = (char)decoded;
    }
    destination[out] = '\0';
    return out > 0;
}

static int d2_valid_utf8(const char *text)
{
    const unsigned char *cursor = (const unsigned char *)text;

    if (!text)
        return 0;
    while (*cursor) {
        uint32_t codepoint;
        size_t continuation;
        if (*cursor < 0x80) {
            ++cursor;
            continue;
        } else if ((*cursor & 0xe0) == 0xc0) {
            codepoint = *cursor & 0x1f;
            continuation = 1;
            if (codepoint < 2)
                return 0; /* overlong ASCII */
        } else if ((*cursor & 0xf0) == 0xe0) {
            codepoint = *cursor & 0x0f;
            continuation = 2;
        } else if ((*cursor & 0xf8) == 0xf0) {
            codepoint = *cursor & 0x07;
            continuation = 3;
        } else {
            return 0;
        }
        ++cursor;
        for (size_t index = 0; index < continuation; ++index) {
            if ((cursor[index] & 0xc0) != 0x80)
                return 0;
            codepoint = (codepoint << 6) | (cursor[index] & 0x3f);
        }
        if ((continuation == 2 && codepoint < 0x800) ||
            (continuation == 3 && codepoint < 0x10000) ||
            (codepoint >= 0xd800 && codepoint <= 0xdfff) ||
            codepoint > 0x10ffff) {
            return 0;
        }
        cursor += continuation;
    }
    return 1;
}

struct d2_library_browse_state {
    char context[80];
    char sidebar_item[D2_BROWSE_ROWS][48];
    int track_row;
    int track_count;
    int sidebar_row;
    int sidebar_count;
    struct timespec file_mtime;
    int ready;
};

static struct d2_library_browse_state d2_library_browse = {
    .context = "ALL TRACKS",
    .sidebar_item = {{""}, {""}, {"TRACKS"}, {"ALL TRACKS"}, {"PLAYLISTS"},
                     {"CRATES"}, {"FILES"}, {""}, {""}},
    .track_row = -1,
    .track_count = 0,
    .sidebar_row = 0,
    .sidebar_count = 0,
};
static uint8_t d2_button_state[3][58];

enum d2_screen_view {
    D2_VIEW_DECK = 0,
    D2_VIEW_BROWSE = 1,
    D2_VIEW_HOTCUE = 2,
    D2_VIEW_LOOP = 3,
    D2_VIEW_SAMPLER = 4,
    D2_VIEW_FREEZE = 5,
    D2_VIEW_BEATJUMP = 6,
};
static enum d2_screen_view d2_screen_view[3] = {
    D2_VIEW_DECK, D2_VIEW_DECK, D2_VIEW_DECK,
};
struct d2_performance_visible_state {
    enum d2_screen_view view;
    char title[80];
    uint32_t pad_rgb[8];
};
/* The display controller retains the last complete frame.  Record exactly
 * what was last submitted so static Browser/Performance surfaces do not
 * consume USB bandwidth by retransmitting an identical 480x272 image.  A
 * view is committed only after its complete frame is in libctlra's transfer
 * buffer; returning from another surface therefore always redraws. */
static int d2_last_screen_view[3] = {-1, -1, -1};
static struct d2_performance_visible_state d2_performance_frame[3];
static int d2_performance_frame_valid[3] = {0, 0, 0};

/* Load policy and file validation remain authoritative in Mixxx.  The bridge
 * only displays the exact outcome as a short per-D2 Browse notice. */
#define D2_BROWSE_NOTICE_US 2000000ULL
enum d2_browse_notice_kind {
    D2_BROWSE_NOTICE_NONE = 0,
    D2_BROWSE_NOTICE_DECK_PLAYING,
    D2_BROWSE_NOTICE_TRACK_MISSING,
    D2_BROWSE_NOTICE_NO_SELECTION,
    D2_BROWSE_NOTICE_LOAD_FAILED,
};
static enum d2_browse_notice_kind d2_browse_notice_kind[3] = {
    D2_BROWSE_NOTICE_NONE, D2_BROWSE_NOTICE_NONE, D2_BROWSE_NOTICE_NONE,
};
static uint64_t d2_browse_notice_until_us[3] = {0, 0, 0};

static void d2_set_browse_notice(int player,
                                 enum d2_browse_notice_kind kind,
                                 uint64_t now_us)
{
    if (player < 1 || player > 2 || kind == D2_BROWSE_NOTICE_NONE)
        return;
    d2_browse_notice_kind[player] = kind;
    d2_browse_notice_until_us[player] = now_us + D2_BROWSE_NOTICE_US;
    d2_browse_mark_dirty();
}

static void d2_clear_browse_notice(int player)
{
    if (player < 1 || player > 2 ||
        d2_browse_notice_kind[player] == D2_BROWSE_NOTICE_NONE)
        return;
    d2_browse_notice_kind[player] = D2_BROWSE_NOTICE_NONE;
    d2_browse_notice_until_us[player] = 0;
    d2_browse_mark_dirty();
}

static enum d2_browse_notice_kind d2_active_browse_notice(
        int player, uint64_t now_us)
{
    if (player < 1 || player > 2 ||
        d2_browse_notice_kind[player] == D2_BROWSE_NOTICE_NONE)
        return D2_BROWSE_NOTICE_NONE;
    if (now_us >= d2_browse_notice_until_us[player]) {
        d2_browse_notice_kind[player] = D2_BROWSE_NOTICE_NONE;
        d2_browse_notice_until_us[player] = 0;
        d2_browse_mark_dirty();
        return D2_BROWSE_NOTICE_NONE;
    }
    return d2_browse_notice_kind[player];
}
static const char *d2_pad_labels[8] = {
    "1", "2", "3", "4", "5", "6", "7", "8",
};
static const char *const d2_loop_pad_labels[8] = {
    "1/4", "1/2", "1", "2", "4", "8", "16", "32",
};
static const char *const d2_beatjump_pad_labels[8] = {
    "-1", "+1", "-4", "+4", "-8", "+8", "-16", "+16",
};

static const char *d2_performance_pad_label(enum d2_screen_view view, int pad)
{
    if (pad < 0 || pad >= 8)
        return "";
    if (view == D2_VIEW_BEATJUMP)
        return d2_beatjump_pad_labels[pad];
    if (view == D2_VIEW_LOOP || view == D2_VIEW_FREEZE)
        return d2_loop_pad_labels[pad];
    return d2_pad_labels[pad];
}

static int d2_is_performance_view(enum d2_screen_view view)
{
    return view == D2_VIEW_HOTCUE || view == D2_VIEW_LOOP ||
           view == D2_VIEW_SAMPLER || view == D2_VIEW_FREEZE ||
           view == D2_VIEW_BEATJUMP;
}

static int d2_view_has_dedicated_compositor(enum d2_screen_view view)
{
    return view == D2_VIEW_DECK || view == D2_VIEW_BROWSE ||
           d2_is_performance_view(view);
}

static const char *d2_performance_heading(enum d2_screen_view view)
{
    switch (view) {
    case D2_VIEW_HOTCUE: return "HOTCUE";
    case D2_VIEW_LOOP: return "LOOP";
    case D2_VIEW_SAMPLER: return "SAMPLER";
    case D2_VIEW_FREEZE: return "FREEZE";
    case D2_VIEW_BEATJUMP: return "BEATJUMP";
    default: return "PERFORMANCE";
    }
}

static const char *d2_performance_instruction(enum d2_screen_view view)
{
    switch (view) {
    case D2_VIEW_HOTCUE: return "PRESS PAD: HOT CUE";
    case D2_VIEW_LOOP: return "TOGGLE PAD: LOOP SIZE";
    case D2_VIEW_SAMPLER: return "PRESS PAD: PLAY SAMPLE";
    case D2_VIEW_FREEZE: return "HOLD PAD: BEAT ROLL";
    case D2_VIEW_BEATJUMP: return "PRESS PAD: BEAT JUMP";
    default: return "";
    }
}

static void d2_make_performance_visible_state(
        struct d2_performance_visible_state *visible,
        enum d2_screen_view view, const char *deck_title,
        const uint32_t pad_rgb[8])
{
    if (!visible)
        return;
    memset(visible, 0, sizeof(*visible));
    visible->view = view;
    d2_copy_text(visible->title, sizeof(visible->title), deck_title);
    for (int pad = 0; pad < 8; ++pad) {
        visible->pad_rgb[pad] =
            pad_rgb ? pad_rgb[pad] & 0x00ffffffU : 0;
    }
}

static int d2_performance_visible_equal(
        const struct d2_performance_visible_state *left,
        const struct d2_performance_visible_state *right)
{
    return left && right && left->view == right->view &&
           strcmp(left->title, right->title) == 0 &&
           memcmp(left->pad_rgb, right->pad_rgb,
                  sizeof(left->pad_rgb)) == 0;
}

static int d2_performance_frame_is_current(
        int player, const struct d2_performance_visible_state *visible)
{
    if (player < 1 || player > 2 || !visible)
        return 0;
    return d2_performance_frame_valid[player] &&
           d2_last_screen_view[player] == (int)visible->view &&
           d2_performance_visible_equal(
               &d2_performance_frame[player], visible);
}

static void d2_commit_performance_frame(
        int player, const struct d2_performance_visible_state *visible)
{
    if (player < 1 || player > 2 || !visible)
        return;
    d2_performance_frame[player] = *visible;
    d2_performance_frame_valid[player] = 1;
    d2_last_screen_view[player] = (int)visible->view;
}

static int d2_browse_frame_is_current(int player)
{
    if (player < 1 || player > 2)
        return 0;
    return d2_browse_frame_valid[player] &&
           d2_browse_rendered_generation[player] == d2_browse_generation &&
           d2_last_screen_view[player] == D2_VIEW_BROWSE;
}

static int d2_try_performance_snapshot(
        int player, char *deck_title, size_t deck_title_size,
        uint32_t pad_rgb[8])
{
    if (player < 1 || player > 2 || !deck_title ||
        deck_title_size == 0 || !pad_rgb)
        return 0;
    /* ctlra_idle_iter() owns HID polling and calls screen_callback(). Never
     * let a display snapshot wait behind the other deck's full render. A
     * busy lock simply defers this static frame to the next 33 ms callback. */
    if (pthread_mutex_trylock(&d2_state_mutex) != 0)
        return 0;
    d2_visible_deck_title(deck_title, deck_title_size,
                          d2_screen_state[player].title);
    memcpy(pad_rgb, d2_led_state[player].pad_rgb,
           sizeof(d2_led_state[player].pad_rgb));
    pthread_mutex_unlock(&d2_state_mutex);
    return 1;
}

static int d2_shell_quote(char *output, size_t output_size, const char *input)
{
    size_t used = 0;
    if (output_size < 3)
        return 0;
    output[used++] = '\'';
    for (const char *p = input; *p; ++p) {
        const char *replacement = (*p == '\'') ? "'\\\\''" : NULL;
        if (replacement) {
            size_t n = strlen(replacement);
            if (used + n + 2 > output_size)
                return 0;
            memcpy(output + used, replacement, n);
            used += n;
        } else {
            if (used + 2 > output_size)
                return 0;
            output[used++] = *p;
        }
    }
    output[used++] = '\'';
    output[used] = '\0';
    return 1;
}

static void d2_load_cover_art(int deck, const char *location)
{
    char quoted_location[2048];
    char command[2600];
    FILE *pipe;
    if (deck < 0 || deck >= D2_ASSET_SLOTS || !d2_cover_art[deck] ||
        !location || !location[0])
        return;
    d2_cover_art_ready[deck] = 0;
    memset(d2_cover_art[deck], 0, 48 * 48 * 2);
    if (!d2_shell_quote(quoted_location, sizeof(quoted_location), location))
        return;
    snprintf(command, sizeof(command),
             "ffmpeg -nostdin -v error -i %s -an "
             "-vf 'scale=48:48:force_original_aspect_ratio=decrease,"
             "pad=48:48:(ow-iw)/2:(oh-ih)/2' -frames:v 1 "
             "-f rawvideo -pix_fmt rgb565be - 2>/dev/null",
             quoted_location);
    pipe = popen(command, "r");
    if (!pipe)
        return;
    size_t bytes = fread(d2_cover_art[deck], 1, 48 * 48 * 2, pipe);
    int status = pclose(pipe);
    if (status == 0 && bytes == 48 * 48 * 2)
        d2_cover_art_ready[deck] = 1;
}

/* Read Mixxx's own high-resolution, frequency-separated waveform cache.
 * The helper writes low/mid/high values as 2048 RGB triplets. */
static int d2_load_mixxx_waveform(int deck, int analysis_id)
{
    uint8_t raw[D2_WAVEFORM_POINTS][3];
    char command[256];
    FILE *pipe;

    if (deck < 0 || deck >= D2_ASSET_SLOTS || !d2_waveform[deck] ||
        !d2_waveform_low[deck] || !d2_waveform_mid[deck] ||
        !d2_waveform_high[deck] || analysis_id <= 0)
        return 0;
    snprintf(command, sizeof(command),
             "/home/pi/openAV-Ctlra/build/d2_waveform_extract "
             "/home/pi/.mixxx/analysis/%d", analysis_id);
    pipe = popen(command, "r");
    if (!pipe)
        return 0;
    size_t read_count = fread(raw, sizeof(raw[0]), D2_WAVEFORM_POINTS, pipe);
    int exit_status = pclose(pipe);
    if (read_count != D2_WAVEFORM_POINTS || exit_status != 0)
        return 0;
    for (int i = 0; i < D2_WAVEFORM_POINTS; ++i) {
        d2_waveform_low[deck][i] = raw[i][0];
        d2_waveform_mid[deck][i] = raw[i][1];
        d2_waveform_high[deck][i] = raw[i][2];
        int peak = raw[i][0];
        if (raw[i][1] > peak) peak = raw[i][1];
        if (raw[i][2] > peak) peak = raw[i][2];
        d2_waveform[deck][i] = (uint8_t)(4 + peak * 54 / 255);
    }
    d2_waveform_ready[deck] = 2;
    printf("D2 WAVEFORM: deck=%d source=mixxx analysis=%d\n", deck, analysis_id);
    fflush(stdout);
    return 1;
}

static void d2_build_wave_strip(int deck);

/* Mixxx stores constant-tempo grids as BeatGrid-2.0 protobufs in its library.
 * Decode them once through the tiny protobuf helper, not in the render loop. */
static void d2_load_mixxx_beatgrid(int deck, int track_id, float duration)
{
    char command[160];
    const char *helper_path;
    char mode[8] = "";
    FILE *pipe;
    int first_frame = -1;
    double sample_rate = 0.0;
    double bpm = 0.0;
    float parsed_map_position[D2_MAX_BEATS];
    int parsed_map_source_index[D2_MAX_BEATS];
    int parsed_map_count = 0;
    int parsed_grid = 0;
    int parsed_map = 0;
    float parsed_grid_first = 0.0f;
    float parsed_grid_interval = 0.0f;

    if (deck < 0 || deck >= D2_ASSET_SLOTS ||
        track_id <= 0 || duration <= 1.0f)
        return;
    helper_path = getenv("D2_BEATGRID_HELPER");
    if (!helper_path || helper_path[0] != '/' || strlen(helper_path) > 120 ||
        strpbrk(helper_path, " \t\r\n'\"`$;&|<>") != NULL)
        helper_path = "/home/pi/openAV-Ctlra/build/d2_beatgrid_extract";
    snprintf(command, sizeof(command),
             "%s %d", helper_path, track_id);
    pipe = popen(command, "r");
    if (!pipe)
        return;
    if (fscanf(pipe, "%7s", mode) != 1) {
        pclose(pipe);
        return;
    }
    if (strcmp(mode, "GRID") == 0 &&
        fscanf(pipe, "%d %lf %lf", &first_frame, &sample_rate, &bpm) == 3 &&
        sample_rate > 0.0 && isfinite(sample_rate) &&
        bpm >= 20.0 && bpm <= 400.0 && isfinite(bpm)) {
        float first_position = (float)(first_frame / (sample_rate * duration));
        float interval = 60.0f / ((float)bpm * duration);
        if (first_position >= -0.25f && first_position <= 1.0f &&
            interval > 0.0f && interval <= 1.0f &&
            isfinite(first_position) && isfinite(interval)) {
            parsed_grid_first = first_position;
            parsed_grid_interval = interval;
            parsed_grid = 1;
        }
    } else if (strcmp(mode, "MAP") == 0) {
        int source_count = 0;
        if (fscanf(pipe, "%d %lf", &source_count, &sample_rate) == 2 &&
            source_count >= 2 && source_count <= 1000000 &&
            sample_rate > 0.0 && isfinite(sample_rate)) {
            int valid = source_count <= D2_MAX_BEATS;
            int previous_frame = 0;
            for (int i = 0; i < source_count; ++i) {
                int frame = 0;
                if (fscanf(pipe, "%d", &frame) != 1) {
                    valid = 0;
                    break;
                }
                /* Mixxx treats negative finite FramePos values as valid
                 * lead-in markers and imports every BeatMap entry regardless
                 * of its legacy enabled flag.  Never use -1 as a sentinel or
                 * compress the list: either operation changes the source beat
                 * index and therefore any future bar-anchor calculation. */
                if (i > 0 && frame <= previous_frame) {
                    valid = 0;
                }
                float position = (float)(frame / (sample_rate * duration));
                if (!isfinite(position)) {
                    valid = 0;
                }
                if (i < D2_MAX_BEATS) {
                    parsed_map_position[i] = position;
                    parsed_map_source_index[i] = i;
                }
                previous_frame = frame;
            }
            if (valid) {
                parsed_map_count = source_count;
                parsed_map = 1;
            }
        }
    }
    int exit_status = pclose(pipe);
    if (exit_status == 0 && parsed_grid) {
        d2_screen_state[deck].beatgrid_first_position = parsed_grid_first;
        d2_screen_state[deck].beatgrid_interval = parsed_grid_interval;
        d2_screen_state[deck].beatgrid_ready = 1;
        d2_screen_state[deck].beatmap_ready = 0;
        d2_screen_state[deck].beatmap_count = 0;
        printf("D2 BEATGRID: deck=%d first=%.6f interval=%.8f bpm=%.3f\n",
               deck, parsed_grid_first, parsed_grid_interval, bpm);
    } else if (exit_status == 0 && parsed_map) {
        memcpy(d2_screen_state[deck].beatmap_position,
               parsed_map_position,
               (size_t)parsed_map_count * sizeof(parsed_map_position[0]));
        memcpy(d2_screen_state[deck].beatmap_source_index,
               parsed_map_source_index,
               (size_t)parsed_map_count * sizeof(parsed_map_source_index[0]));
        d2_screen_state[deck].beatmap_count = parsed_map_count;
        d2_screen_state[deck].beatmap_ready = 1;
        d2_screen_state[deck].beatgrid_ready = 0;
        printf("D2 BEATMAP: deck=%d beats=%d\n", deck, parsed_map_count);
    } else {
        /* Commit only a fully decoded helper response.  The track-identity
         * transition already cleared stale beat state; a transient helper
         * failure must never publish a partial map to the render thread. */
        printf("D2 BEAT DATA: deck=%d track_id=%d unavailable\n",
               deck, track_id);
    }
    fflush(stdout);
}

/* Mixxx sends state roughly every 200 ms, while the D2 screen refreshes far
 * more often. Advance the playhead locally between packets so the waveform
 * scrolls under its fixed needle instead of visibly jumping. */
static float d2_effective_position(const struct d2_screen_state *state)
{
    struct timespec now;
    float position = state ? state->position : 0.0f;
    float duration;
    double elapsed;

    if (!state || !state->playing || state->duration <= 1.0f ||
        state->position_updated_at.tv_sec == 0)
        return position;
    clock_gettime(CLOCK_MONOTONIC, &now);
    elapsed = (double)(now.tv_sec - state->position_updated_at.tv_sec) +
              (double)(now.tv_nsec - state->position_updated_at.tv_nsec) / 1e9;
    duration = state->duration > 1.0f ? state->duration :
               state->remaining / fmaxf(0.001f, 1.0f - state->position);
    float rate = state->rate > 0.0f ? state->rate : 1.0f;
    /* playposition is already a normalized 0..1 value.  It is the sole time
     * anchor for interpolation: beat_prev/beat_next are engine-frame values
     * used only for beat markers and must never drive the transport clock.
     * Keeping those domains separate prevents a stale/rounded beat phase from
     * adding a second elapsed-time term (the visible 2x countdown/rubber band).
     */
    position += (float)(elapsed * rate / duration);
    if (position < 0.0f) return 0.0f;
    if (position > 1.0f) return 1.0f;
    return position;
}

/* Build a compact but real waveform from the actual loaded audio. Mixxx
 * already analyses the same file; this independent summary lets the native
 * bridge render it without accessing Qt's private waveform objects. */
static void d2_load_real_waveform(int deck, const char *location, float duration)
{
    float sums[D2_WAVEFORM_POINTS] = {0};
    unsigned counts[D2_WAVEFORM_POINTS] = {0};
    char quoted_location[2048];
    char command[2300];
    FILE *pipe;
    float samples[1024];
    size_t sample_offset = 0;
    float maximum = 0.0f;

    if (deck < 0 || deck >= D2_ASSET_SLOTS || !d2_waveform[deck] ||
        !d2_waveform_low[deck] || !d2_waveform_mid[deck] ||
        !d2_waveform_high[deck] || !location || !*location ||
        duration <= 0.0f)
        return;
    d2_waveform_ready[deck] = 0;
    memset(d2_waveform[deck], 0, D2_WAVEFORM_POINTS);
    /* A failed/unfinished Mixxx analysis must not inherit the previous
     * track's filtered bands.  Stale low/mid/high arrays were responsible
     * for artificial repeated shapes when the raw fallback was used. */
    memset(d2_waveform_low[deck], 0, D2_WAVEFORM_POINTS);
    memset(d2_waveform_mid[deck], 0, D2_WAVEFORM_POINTS);
    memset(d2_waveform_high[deck], 0, D2_WAVEFORM_POINTS);
    if (!d2_shell_quote(quoted_location, sizeof(quoted_location), location))
        return;
    snprintf(command, sizeof(command),
             "ffmpeg -nostdin -v error -i %s -ac 1 -ar 1000 -f f32le - 2>/dev/null",
             quoted_location);
    pipe = popen(command, "r");
    if (!pipe)
        return;
    for (;;) {
        size_t read_count = fread(samples, sizeof(float),
                                  sizeof(samples) / sizeof(samples[0]), pipe);
        for (size_t i = 0; i < read_count; ++i) {
            int point = (int)(((double)(sample_offset + i) *
                               D2_WAVEFORM_POINTS) / (duration * 1000.0));
            if (point < 0) point = 0;
            if (point >= D2_WAVEFORM_POINTS) point = D2_WAVEFORM_POINTS - 1;
            float amplitude = samples[i] < 0.0f ? -samples[i] : samples[i];
            sums[point] += amplitude;
            counts[point]++;
        }
        sample_offset += read_count;
        if (read_count < sizeof(samples) / sizeof(samples[0]))
            break;
    }
    pclose(pipe);
    for (int i = 0; i < D2_WAVEFORM_POINTS; ++i) {
        if (counts[i] > 0)
            sums[i] /= (float)counts[i];
        if (sums[i] > maximum)
            maximum = sums[i];
    }
    if (maximum <= 0.00001f)
        return;
    for (int i = 0; i < D2_WAVEFORM_POINTS; ++i) {
        int scaled = (int)(sums[i] * 58.0f / maximum + 0.5f);
        if (scaled < 3) scaled = 3;
        if (scaled > 58) scaled = 58;
        d2_waveform[deck][i] = (uint8_t)scaled;
    }
    d2_waveform_ready[deck] = 1;
    printf("D2 WAVEFORM: deck=%d points=%d file=%s\n",
           deck, D2_WAVEFORM_POINTS, location);
    fflush(stdout);
}

static int d2_resolve_track_id_by_location(const char *location)
{
    static const char sql[] =
        "SELECT l.id FROM library l "
        "JOIN track_locations tl ON tl.id = l.location "
        "WHERE tl.location = ? AND l.mixxx_deleted = 0 "
        "ORDER BY l.id DESC LIMIT 1";
    sqlite3 *db = NULL;
    sqlite3_stmt *stmt = NULL;
    int track_id = 0;

    if (!location || location[0] == '\0')
        return 0;
    if (sqlite3_open_v2("/home/pi/.mixxx/mixxxdb.sqlite", &db,
                        SQLITE_OPEN_READONLY, NULL) != SQLITE_OK)
        goto done;
    sqlite3_busy_timeout(db, 150);
    if (sqlite3_prepare_v2(db, sql, -1, &stmt, NULL) != SQLITE_OK)
        goto done;
    sqlite3_bind_text(stmt, 1, location, -1, SQLITE_TRANSIENT);
    if (sqlite3_step(stmt) == SQLITE_ROW)
        track_id = sqlite3_column_int(stmt, 0);

done:
    if (stmt)
        sqlite3_finalize(stmt);
    if (db)
        sqlite3_close(db);
    return track_id > 0 ? track_id : 0;
}

/* Resolve all immutable track data from the exact database identity exported
 * by Mixxx's EngineBuffer.  Duration is only a rendering measurement; it must
 * never be used as an identity because different tracks routinely share the
 * same rounded length. */
static int d2_load_track_metadata(int deck, int track_id, float duration)
{
    static const char sql[] =
        "SELECT l.artist, l.title, l.key, l.duration, tl.location, "
        "(SELECT id FROM track_analysis "
        " WHERE track_id = l.id AND type = '1' ORDER BY id DESC LIMIT 1) "
        "FROM library l "
        "JOIN track_locations tl ON tl.id = l.location "
        "WHERE l.id = ? AND l.mixxx_deleted = 0 LIMIT 1";
    sqlite3 *db = NULL;
    sqlite3_stmt *stmt = NULL;
    int loaded = 0;

    if (deck < 0 || deck >= D2_ASSET_SLOTS || track_id <= 0)
        return 0;

    if (sqlite3_open_v2("/home/pi/.mixxx/mixxxdb.sqlite", &db,
                        SQLITE_OPEN_READONLY, NULL) != SQLITE_OK) {
        fprintf(stderr, "D2 metadata: cannot open Mixxx database\n");
        goto done;
    }
    sqlite3_busy_timeout(db, 150);
    if (sqlite3_prepare_v2(db, sql, -1, &stmt, NULL) != SQLITE_OK) {
        fprintf(stderr, "D2 metadata: query preparation failed: %s\n",
                sqlite3_errmsg(db));
        goto done;
    }
    sqlite3_bind_int(stmt, 1, track_id);
    int step_result = sqlite3_step(stmt);
    if (step_result == SQLITE_ROW) {
        const unsigned char *artist = sqlite3_column_text(stmt, 0);
        const unsigned char *title = sqlite3_column_text(stmt, 1);
        const unsigned char *musical_key = sqlite3_column_text(stmt, 2);
        float database_duration = (float)sqlite3_column_double(stmt, 3);
        const unsigned char *location = sqlite3_column_text(stmt, 4);
        int waveform_analysis_id = sqlite3_column_int(stmt, 5);
        float render_duration = duration > 1.0f ? duration : database_duration;
        snprintf(d2_screen_state[deck].artist,
                 sizeof(d2_screen_state[deck].artist), "%s",
                 artist ? (const char *)artist : "");
        snprintf(d2_screen_state[deck].title,
                 sizeof(d2_screen_state[deck].title), "%s",
                 title ? (const char *)title : "DECK");
        snprintf(d2_screen_state[deck].musical_key,
                 sizeof(d2_screen_state[deck].musical_key), "%s",
                 musical_key ? (const char *)musical_key : "--");
        snprintf(d2_screen_state[deck].location,
                 sizeof(d2_screen_state[deck].location), "%s",
                 location ? (const char *)location : "");
        if (!d2_load_mixxx_waveform(deck, waveform_analysis_id)) {
            d2_load_real_waveform(deck,
                                  d2_screen_state[deck].location,
                                  render_duration);
        }
        if (d2_waveform_ready[deck])
            d2_build_wave_strip(deck);
        d2_load_cover_art(deck, d2_screen_state[deck].location);
        d2_load_mixxx_beatgrid(deck, track_id, render_duration);
        printf("D2 METADATA: deck=%d track_id=%d location=%s "
               "duration=%.3f artist=%s title=%s\n",
               deck, track_id, d2_screen_state[deck].location,
               render_duration, d2_screen_state[deck].artist,
               d2_screen_state[deck].title);
        fflush(stdout);
        loaded = 1;
    } else {
        fprintf(stderr,
                "D2 metadata: track_id=%d unavailable for deck=%d rc=%d %s\n",
                track_id, deck, step_result, sqlite3_errmsg(db));
    }

done:
    if (stmt)
        sqlite3_finalize(stmt);
    if (db)
        sqlite3_close(db);
    return loaded;
}

static void d2_begin_track_identity(int deck, int track_id)
{
    struct d2_screen_state *state;
    struct d2_track_assets *old_assets;

    if (deck < 1 || deck > 2)
        return;
    state = &d2_screen_state[deck];
    if (++d2_track_generation[deck] == 0)
        d2_track_generation[deck] = 1;
    pthread_mutex_lock(&d2_track_load_mutex);
    d2_track_load_job[deck].pending = 0;
    pthread_mutex_unlock(&d2_track_load_mutex);

    old_assets = d2_track_assets[deck];
    d2_track_assets[deck] = NULL;
    d2_bind_asset_slot(deck, NULL);
    state->track_id = track_id > 0 ? track_id : 0;
    state->position = 0.0f;
    state->remaining = 0.0f;
    state->duration = 0.0f;
    state->bpm = 0.0f;
    state->rate = 1.0f;
    state->playing = 0;
    state->visual_key = 0;
    state->beatgrid_ready = 0;
    state->beatmap_ready = 0;
    state->beatmap_count = 0;
    state->phase_valid = 0;
    state->beatgrid_edit = 0;
    state->position_updated_at = (struct timespec){0, 0};
    state->location[0] = '\0';
    for (int cue = 0; cue < 8; ++cue) {
        state->hotcue_position[cue] = -1.0f;
        state->hotcue_color[cue] = 0;
    }

    snprintf(state->title, sizeof(state->title), "DECK %d", deck);
    snprintf(state->artist, sizeof(state->artist), "MIXXX");
    snprintf(state->musical_key, sizeof(state->musical_key), "--");
    free(old_assets);
}

static void d2_prepare_track_scratch(int slot,
                                     struct d2_track_assets *assets)
{
    struct d2_screen_state *state = &d2_screen_state[slot];
    memset(state, 0, sizeof(*state));
    state->rate = 1.0f;
    state->zoom_level = 2;
    state->loop_size = 4.0f;
    for (int cue = 0; cue < 8; ++cue)
        state->hotcue_position[cue] = -1.0f;
    d2_bind_asset_slot(slot, assets);
}

static int d2_build_track_assets(const struct d2_track_load_job *job,
                                 struct d2_track_assets *assets,
                                 int scratch_slot)
{
    struct d2_screen_state *scratch = &d2_screen_state[scratch_slot];
    int track_id = job->track_id;
    int loaded = 0;

    d2_prepare_track_scratch(scratch_slot, assets);
    scratch->duration = job->duration;
    snprintf(scratch->location, sizeof(scratch->location), "%s",
             job->location);

    if (track_id <= 0 && job->location[0] != '\0')
        track_id = d2_resolve_track_id_by_location(job->location);
    scratch->track_id = track_id > 0 ? track_id : 0;

    if (track_id > 0)
        loaded = d2_load_track_metadata(
                scratch_slot, track_id, job->duration);

    if (!loaded && job->location[0] != '\0') {
        const char *filename = strrchr(job->location, '/');
        d2_load_real_waveform(
                scratch_slot, job->location, job->duration);
        if (d2_waveform_ready[scratch_slot])
            d2_build_wave_strip(scratch_slot);
        d2_load_cover_art(scratch_slot, job->location);
        snprintf(scratch->location, sizeof(scratch->location), "%s",
                 job->location);
        d2_copy_text(scratch->title, sizeof(scratch->title),
                     filename ? filename + 1 : job->location);
        snprintf(scratch->artist, sizeof(scratch->artist), "FILE BROWSER");
        snprintf(scratch->musical_key, sizeof(scratch->musical_key), "--");
        loaded = d2_waveform_ready[scratch_slot] != 0;
    }

    assets->waveform_ready = d2_waveform_ready[scratch_slot];
    assets->wave_strip_ready = d2_wave_strip_ready[scratch_slot];
    assets->cover_art_ready = d2_cover_art_ready[scratch_slot];
    return loaded;
}

static int d2_track_job_matches_locked(const struct d2_track_load_job *job)
{
    const struct d2_screen_state *state = &d2_screen_state[job->deck];
    if (job->generation != d2_track_generation[job->deck])
        return 0;
    if (job->track_id > 0)
        return state->track_id == job->track_id;
    return state->track_id == 0 && job->location[0] != '\0' &&
           strcmp(state->location, job->location) == 0;
}

static void d2_commit_track_assets(const struct d2_track_load_job *job,
                                   struct d2_track_assets *assets,
                                   int scratch_slot, int loaded)
{
    struct d2_track_assets *old_assets = NULL;
    struct d2_screen_state *scratch = &d2_screen_state[scratch_slot];

    pthread_mutex_lock(&d2_state_mutex);
    if (d2_track_job_matches_locked(job)) {
        struct d2_screen_state *state = &d2_screen_state[job->deck];
        state->track_id = scratch->track_id;
        d2_copy_text(state->title, sizeof(state->title),
                     loaded ? scratch->title : "TRACK NOT INDEXED");
        d2_copy_text(state->artist, sizeof(state->artist),
                     loaded ? scratch->artist : "NO VALID MIXXX TRACK ID");
        d2_copy_text(state->musical_key, sizeof(state->musical_key),
                     loaded ? scratch->musical_key : "--");
        if (scratch->location[0] != '\0')
            d2_copy_text(state->location, sizeof(state->location),
                         scratch->location);

        state->beatgrid_first_position = scratch->beatgrid_first_position;
        state->beatgrid_interval = scratch->beatgrid_interval;
        state->beatgrid_ready = scratch->beatgrid_ready;
        state->beatmap_count = scratch->beatmap_count;
        state->beatmap_ready = scratch->beatmap_ready;
        if (scratch->beatmap_count > 0) {
            size_t count = (size_t)scratch->beatmap_count;
            memcpy(state->beatmap_position, scratch->beatmap_position,
                   count * sizeof(state->beatmap_position[0]));
            memcpy(state->beatmap_source_index,
                   scratch->beatmap_source_index,
                   count * sizeof(state->beatmap_source_index[0]));
        }

        old_assets = d2_track_assets[job->deck];
        d2_track_assets[job->deck] = assets;
        d2_bind_asset_slot(job->deck, assets);
        assets = NULL;
    }
    pthread_mutex_unlock(&d2_state_mutex);

    d2_bind_asset_slot(scratch_slot, NULL);
    memset(scratch, 0, sizeof(*scratch));
    free(old_assets);
    free(assets);
}

static void *d2_track_load_thread_main(void *userdata)
{
    int deck = *(const int *)userdata;
    int scratch_slot = D2_LOAD_SCRATCH(deck);

    for (;;) {
        struct d2_track_load_job job;
        pthread_mutex_lock(&d2_track_load_mutex);
        while (!d2_track_load_stop && !d2_track_load_job[deck].pending)
            pthread_cond_wait(&d2_track_load_cond, &d2_track_load_mutex);
        if (d2_track_load_stop) {
            pthread_mutex_unlock(&d2_track_load_mutex);
            break;
        }
        job = d2_track_load_job[deck];
        d2_track_load_job[deck].pending = 0;
        pthread_mutex_unlock(&d2_track_load_mutex);

        struct d2_track_assets *assets = calloc(1, sizeof(*assets));
        if (!assets)
            continue;
        int loaded = d2_build_track_assets(&job, assets, scratch_slot);
        d2_commit_track_assets(&job, assets, scratch_slot, loaded);
    }
    d2_bind_asset_slot(scratch_slot, NULL);
    return NULL;
}

static void d2_queue_track_load(int deck, float duration)
{
    if (deck < 1 || deck > D2_PHYSICAL_DECKS || duration <= 1.0f)
        return;
    pthread_mutex_lock(&d2_track_load_mutex);
    d2_track_load_job[deck].deck = deck;
    d2_track_load_job[deck].track_id = d2_screen_state[deck].track_id;
    d2_track_load_job[deck].duration = duration;
    d2_track_load_job[deck].generation = d2_track_generation[deck];
    snprintf(d2_track_load_job[deck].location,
             sizeof(d2_track_load_job[deck].location), "%s",
             d2_screen_state[deck].location);
    d2_track_load_job[deck].pending = 1;
    pthread_cond_broadcast(&d2_track_load_cond);
    pthread_mutex_unlock(&d2_track_load_mutex);
}

static int d2_track_loaders_start(void)
{
    static int deck_arg[3] = {0, 1, 2};
    pthread_mutex_lock(&d2_track_load_mutex);
    d2_track_load_stop = 0;
    pthread_mutex_unlock(&d2_track_load_mutex);
    for (int deck = 1; deck <= D2_PHYSICAL_DECKS; ++deck) {
        if (pthread_create(&d2_track_load_thread[deck], NULL,
                           d2_track_load_thread_main,
                           &deck_arg[deck]) != 0) {
            pthread_mutex_lock(&d2_track_load_mutex);
            d2_track_load_stop = 1;
            pthread_cond_broadcast(&d2_track_load_cond);
            pthread_mutex_unlock(&d2_track_load_mutex);
            for (int started = 1; started < deck; ++started) {
                pthread_join(d2_track_load_thread[started], NULL);
                d2_track_load_thread_started[started] = 0;
            }
            return -1;
        }
        d2_track_load_thread_started[deck] = 1;
    }
    return 0;
}

static void d2_track_loaders_shutdown(void)
{
    pthread_mutex_lock(&d2_track_load_mutex);
    d2_track_load_stop = 1;
    for (int deck = 1; deck <= D2_PHYSICAL_DECKS; ++deck)
        d2_track_load_job[deck].pending = 0;
    pthread_cond_broadcast(&d2_track_load_cond);
    pthread_mutex_unlock(&d2_track_load_mutex);

    for (int deck = 1; deck <= D2_PHYSICAL_DECKS; ++deck) {
        if (d2_track_load_thread_started[deck]) {
            pthread_join(d2_track_load_thread[deck], NULL);
            d2_track_load_thread_started[deck] = 0;
        }
    }

    pthread_mutex_lock(&d2_state_mutex);
    for (int deck = 1; deck <= D2_PHYSICAL_DECKS; ++deck) {
        struct d2_track_assets *assets = d2_track_assets[deck];
        d2_track_assets[deck] = NULL;
        d2_bind_asset_slot(deck, NULL);
        free(assets);
    }
    pthread_mutex_unlock(&d2_state_mutex);
}

static void d2_format_camelot_key(const char *input, char *output,
                                  size_t output_size)
{
    static const struct {
        const char *key;
        const char *camelot;
    } key_map[] = {
        {"Abm", "1A"}, {"G#m", "1A"}, {"Ebm", "2A"}, {"D#m", "2A"},
        {"Bbm", "3A"}, {"A#m", "3A"}, {"Fm", "4A"}, {"Cm", "5A"},
        {"Gm", "6A"}, {"Dm", "7A"}, {"Am", "8A"}, {"Em", "9A"},
        {"Bm", "10A"}, {"F#m", "11A"}, {"Gbm", "11A"},
        {"C#m", "12A"}, {"Dbm", "12A"},
        {"B", "1B"}, {"F#", "2B"}, {"Gb", "2B"}, {"Db", "3B"},
        {"C#", "3B"}, {"Ab", "4B"}, {"G#", "4B"}, {"Eb", "5B"},
        {"D#", "5B"}, {"Bb", "6B"}, {"A#", "6B"}, {"F", "7B"},
        {"C", "8B"}, {"G", "9B"}, {"D", "10B"}, {"A", "11B"},
        {"E", "12B"},
    };
    int number = 0;
    char mode = '\0';

    if (!output || output_size == 0)
        return;
    if (!input || !input[0]) {
        snprintf(output, output_size, "--");
        return;
    }
    if (sscanf(input, "%d%c", &number, &mode) == 2 && number >= 1 &&
        number <= 12 && (mode == 'A' || mode == 'a' ||
                         mode == 'B' || mode == 'b')) {
        snprintf(output, output_size, "%d%c", number,
                 mode == 'a' ? 'A' : (mode == 'b' ? 'B' : mode));
        return;
    }
    for (size_t index = 0; index < sizeof(key_map) / sizeof(key_map[0]); ++index) {
        if (strcasecmp(input, key_map[index].key) == 0) {
            snprintf(output, output_size, "%s", key_map[index].camelot);
            return;
        }
    }
    snprintf(output, output_size, "%.5s", input);
}

static void d2_browse_key_color(const char *key, int *red, int *green, int *blue)
{
    static const unsigned char palette[12][3] = {
        {230, 76, 180}, {246, 102, 145}, {250, 137, 66}, {220, 226, 68},
        {91, 225, 95}, {245, 181, 44}, {255, 117, 41}, {255, 72, 72},
        {185, 96, 238}, {76, 158, 255}, {52, 215, 241}, {48, 220, 178},
    };
    int number = 0;
    char mode = '\0';

    *red = 175;
    *green = 188;
    *blue = 201;
    if (!key || sscanf(key, "%d%c", &number, &mode) != 2 ||
        number < 1 || number > 12)
        return;
    *red = palette[number - 1][0];
    *green = palette[number - 1][1];
    *blue = palette[number - 1][2];
    if (mode == 'B' || mode == 'b') {
        *red += (255 - *red) / 5;
        *green += (255 - *green) / 5;
        *blue += (255 - *blue) / 5;
    }
}

static int d2_browse_db_prepare(void)
{
    static const char sql[] =
        "SELECT library.artist, library.title, library.bpm, library.key, "
        "library.rating, track_locations.location "
        "FROM library JOIN track_locations "
        "ON library.location = track_locations.id "
        "WHERE library.id = ? AND library.mixxx_deleted = 0";

    if (d2_browse_stmt)
        return 0;
    if (sqlite3_open_v2("/home/pi/.mixxx/mixxxdb.sqlite", &d2_browse_db,
                        SQLITE_OPEN_READONLY, NULL) != SQLITE_OK)
        goto failed;
    sqlite3_busy_timeout(d2_browse_db, 15);
    if (sqlite3_prepare_v2(d2_browse_db, sql, -1, &d2_browse_stmt, NULL) != SQLITE_OK)
        goto failed;
    return 0;

failed:
    if (d2_browse_stmt) {
        sqlite3_finalize(d2_browse_stmt);
        d2_browse_stmt = NULL;
    }
    if (d2_browse_db) {
        sqlite3_close(d2_browse_db);
        d2_browse_db = NULL;
    }
    return -1;
}

static void d2_browse_db_shutdown(void)
{
    if (d2_browse_stmt) {
        sqlite3_finalize(d2_browse_stmt);
        d2_browse_stmt = NULL;
    }
    if (d2_browse_db) {
        sqlite3_close(d2_browse_db);
        d2_browse_db = NULL;
    }
}

static struct d2_browse_cache_entry *d2_browse_cache_find(int track_id)
{
    for (int i = 0; i < D2_BROWSE_CACHE_ENTRIES; ++i) {
        if (d2_browse_cache[i].track_id == track_id) {
            d2_browse_cache[i].last_used = ++d2_browse_cache_clock;
            return &d2_browse_cache[i];
        }
    }
    return NULL;
}

static struct d2_browse_cache_entry *d2_browse_cache_slot(void)
{
    struct d2_browse_cache_entry *oldest = &d2_browse_cache[0];
    for (int i = 0; i < D2_BROWSE_CACHE_ENTRIES; ++i) {
        if (d2_browse_cache[i].track_id == 0)
            return &d2_browse_cache[i];
        if (d2_browse_cache[i].last_used < oldest->last_used)
            oldest = &d2_browse_cache[i];
    }
    return oldest;
}

/* The custom Mixxx control exposes the exact highlighted library row. */
static void d2_load_browse_metadata(int row, int track_id)
{
    struct d2_browse_entry entry = {0};
    struct d2_browse_cache_entry *cached;

    if (row < 0 || row >= D2_BROWSE_ROWS)
        return;
    if (track_id <= 0) {
        if (memcmp(&d2_browse_entries[row], &entry, sizeof(entry)) != 0) {
            d2_browse_entries[row] = entry;
            d2_browse_mark_dirty();
        }
        return;
    }
    cached = d2_browse_cache_find(track_id);
    if (cached) {
        entry = cached->entry;
        /* Availability is intentionally not frozen in the LRU. A USB volume
         * may be unplugged/reinserted while the bridge keeps running. */
        entry.available =
            cached->location[0] != '\0' && access(cached->location, R_OK) == 0;
    } else if (d2_browse_db_prepare() == 0) {
        sqlite3_reset(d2_browse_stmt);
        sqlite3_clear_bindings(d2_browse_stmt);
        sqlite3_bind_int(d2_browse_stmt, 1, track_id);
        entry.track_id = track_id;
        if (sqlite3_step(d2_browse_stmt) == SQLITE_ROW) {
            const unsigned char *artist = sqlite3_column_text(d2_browse_stmt, 0);
            const unsigned char *title = sqlite3_column_text(d2_browse_stmt, 1);
            const unsigned char *musical_key = sqlite3_column_text(d2_browse_stmt, 3);
            const unsigned char *location = sqlite3_column_text(d2_browse_stmt, 5);
            snprintf(entry.title, sizeof(entry.title), "%s",
                     title ? (const char *)title : "UNTITLED");
            snprintf(entry.artist, sizeof(entry.artist), "%s",
                     artist ? (const char *)artist : "UNKNOWN ARTIST");
            d2_format_camelot_key(musical_key ? (const char *)musical_key : "--",
                                  entry.musical_key, sizeof(entry.musical_key));
            entry.bpm = (float)sqlite3_column_double(d2_browse_stmt, 2);
            entry.rating = sqlite3_column_int(d2_browse_stmt, 4);
            entry.available = location && access((const char *)location, R_OK) == 0;
            cached = d2_browse_cache_slot();
            cached->track_id = track_id;
            cached->entry = entry;
            snprintf(cached->location, sizeof(cached->location), "%s",
                     location ? (const char *)location : "");
            cached->last_used = ++d2_browse_cache_clock;
        } else {
            /* USB imports can publish the TrackId just before the database row
             * becomes visible. Keep this row retryable instead of poisoning the
             * LRU with an empty result that would persist until restart. */
            snprintf(entry.title, sizeof(entry.title), "LOADING...");
        }
        /* sqlite3_step keeps a read transaction open until the statement is
         * reset/finalized.  The bridge must not retain that read lock between
         * frames: Mixxx needs to write its library database while USB folders
         * are imported or metadata is committed. */
        sqlite3_reset(d2_browse_stmt);
    } else {
        entry.track_id = track_id;
        snprintf(entry.title, sizeof(entry.title), "LOADING...");
    }

    if (memcmp(&d2_browse_entries[row], &entry, sizeof(entry)) != 0) {
        d2_browse_entries[row] = entry;
        d2_browse_mark_dirty();
    }
}

static void d2_parse_sysex(const unsigned char *data, uint32_t len)
{
    char message[256];
    size_t out = 0;

    if (!data || len < 7 || data[0] != 0xF0 || data[1] != 0x7D)
        return;

    for (uint32_t i = 2; i < len && data[i] != 0xF7; i++) {
        if (out + 1 >= sizeof(message))
            break;
        message[out++] = (char)(data[i] & 0x7F);
    }
    message[out] = '\0';

    int deck = 0;
    char key[16] = {0};
    char value[192] = {0};
    if (sscanf(message, "D2|%d|%15[^|]|%191[^\n]", &deck, key, value) != 3 ||
        deck < 1 || deck > 2) {
        return;
    }

    if (strcmp(key, "BPM") == 0) {
        d2_screen_state[deck].bpm = strtof(value, NULL);
    } else if (strcmp(key, "LOCBEGIN") == 0) {
        unsigned long expected = strtoul(value, NULL, 10);
        d2_begin_track_identity(deck, 0);
        d2_encoded_location_length[deck] = 0;
        d2_encoded_location[deck][0] = '\0';
        d2_encoded_location_valid[deck] =
            expected > 0 && expected < D2_ENCODED_LOCATION_MAX;
    } else if (strcmp(key, "LOCCHUNK") == 0) {
        size_t chunk_length = strlen(value);
        size_t assembled = d2_encoded_location_length[deck];
        if (d2_encoded_location_valid[deck] &&
            assembled + chunk_length < D2_ENCODED_LOCATION_MAX) {
            memcpy(d2_encoded_location[deck] + assembled,
                   value, chunk_length + 1);
            d2_encoded_location_length[deck] += chunk_length;
        } else {
            d2_encoded_location_valid[deck] = 0;
        }
    } else if (strcmp(key, "LOCEND") == 0) {
        char exact_location[sizeof(d2_screen_state[deck].location)];
        if (d2_encoded_location_valid[deck] &&
            d2_percent_decode(d2_encoded_location[deck], exact_location,
                              sizeof(exact_location))) {
            /* Resolving an external/file-browser location touches SQLite and
             * must never run in the ctlra/MIDI polling owner. The loader
             * worker resolves it together with the remaining immutable
             * track assets after LOAD arrives. */
            d2_begin_track_identity(deck, 0);
            snprintf(d2_screen_state[deck].location,
                     sizeof(d2_screen_state[deck].location), "%s",
                     exact_location);
        }
        d2_encoded_location_length[deck] = 0;
        d2_encoded_location[deck][0] = '\0';
        d2_encoded_location_valid[deck] = 0;
    } else if (strcmp(key, "TRACKID") == 0) {
        int track_id = atoi(value);
        d2_begin_track_identity(deck, track_id);
    } else if (strcmp(key, "LOAD") == 0) {
        /* TRACKID is sent first for every successful Mixxx load.  LOAD only
         * supplies the exact engine duration used for normalized rendering;
         * it never selects a database row. */
        float duration = strtof(value, NULL);
        if (duration > 1.0f) {
            d2_screen_state[deck].duration = duration;
            d2_queue_track_load(deck, duration);
        }
    } else if (strcmp(key, "POS") == 0) {
        float position = strtof(value, NULL);
        if (position < 0.0f) position = 0.0f;
        if (position > 1.0f) position = 1.0f;
        /* Transport anchors now arrive at 30 Hz.  Correct most of the small
         * error immediately so the D2 does not keep a visible fraction of a
         * second behind Mixxx, while retaining enough smoothing to avoid
         * jitter from the MIDI/SysEx arrival time. Genuine seek/Cue jumps
         * still snap below. */
        struct d2_screen_state *state = &d2_screen_state[deck];
        if (state->playing && state->duration > 1.0f &&
            state->position_updated_at.tv_sec != 0) {
            float estimate = d2_effective_position(state);
            float error = position - estimate;
            float soft_limit = 0.12f / state->duration; /* 120 ms */
            if (fabsf(error) < soft_limit)
                position = estimate + error * 0.55f;
        }
        state->position = position;
        clock_gettime(CLOCK_MONOTONIC,
                      &state->position_updated_at);
    } else if (strcmp(key, "PLAY") == 0) {
        int playing = atoi(value) != 0;
        d2_screen_state[deck].playing = playing;
    } else if (strcmp(key, "DURATION") == 0) {
        d2_screen_state[deck].duration = strtof(value, NULL);
        if (d2_screen_state[deck].duration < 0.0f)
            d2_screen_state[deck].duration = 0.0f;
    } else if (strcmp(key, "RATE") == 0) {
        float rate = strtof(value, NULL);
        if (rate > 0.25f && rate < 4.0f)
            d2_screen_state[deck].rate = rate;
    } else if (strcmp(key, "BEATDIST") == 0) {
        float beat_distance = strtof(value, NULL);
        if (beat_distance >= 0.0f && beat_distance <= 1.0f)
            d2_screen_state[deck].beat_distance = beat_distance;
    } else if (strcmp(key, "BEATVALID") == 0) {
        d2_screen_state[deck].beatgrid_valid = atoi(value) != 0;
    } else if (strcmp(key, "BEATPREV") == 0) {
        d2_screen_state[deck].beat_prev_position = strtof(value, NULL);
    } else if (strcmp(key, "BEATNEXT") == 0) {
        d2_screen_state[deck].beat_next_position = strtof(value, NULL);
    } else if (strcmp(key, "ZOOM") == 0) {
        int zoom = atoi(value);
        if (zoom == 2 || zoom == 4 || zoom == 8)
            d2_screen_state[deck].zoom_level = zoom;
    } else if (strcmp(key, "TIMEMODE") == 0) {
        d2_screen_state[deck].time_mode = atoi(value) != 0;
    } else if (strcmp(key, "LOOPSIZE") == 0) {
        float loop_size = strtof(value, NULL);
        if (loop_size >= 0.03125f && loop_size <= 512.0f)
            d2_screen_state[deck].loop_size = loop_size;
    } else if (strcmp(key, "QUANTIZE") == 0) {
        d2_screen_state[deck].quantize = atoi(value) != 0;
    } else if (strcmp(key, "KEYLOCK") == 0) {
        d2_screen_state[deck].keylock = atoi(value) != 0;
    } else if (strcmp(key, "KEYVISUAL") == 0) {
        int visual_key = atoi(value);
        if (visual_key >= 0 && visual_key <= 24)
            d2_screen_state[deck].visual_key = visual_key;
    } else if (strcmp(key, "PHASE") == 0) {
        float master_phase = 0.0f;
        float active_phase = 0.0f;
        int master_step = 0;
        int active_step = 0;
        int phase_valid = 1;
        int master_deck = deck;
        int follower_deck = deck == 1 ? 2 : 1;
        int parsed = sscanf(value, "%f,%f,%d,%d,%d,%d,%d", &master_phase,
                            &active_phase, &master_step, &active_step,
                            &phase_valid, &master_deck, &follower_deck);
        if (parsed >= 2 &&
            isfinite(master_phase) && isfinite(active_phase)) {
            master_phase -= floorf(master_phase);
            active_phase -= floorf(active_phase);
            if (master_phase < 0.0f) master_phase += 1.0f;
            if (active_phase < 0.0f) active_phase += 1.0f;
            d2_screen_state[deck].phase_master = master_phase;
            d2_screen_state[deck].phase_active = active_phase;
            if (parsed >= 4) {
                d2_screen_state[deck].phase_master_step =
                    ((master_step % 4) + 4) % 4;
                d2_screen_state[deck].phase_active_step =
                    ((active_step % 4) + 4) % 4;
            }
            d2_screen_state[deck].phase_valid =
                parsed >= 5 ? phase_valid != 0 : 1;
            if (parsed >= 7 && master_deck >= 1 && master_deck <= 2 &&
                follower_deck >= 1 && follower_deck <= 2 &&
                master_deck != follower_deck) {
                d2_screen_state[deck].phase_master_deck = master_deck;
                d2_screen_state[deck].phase_follower_deck = follower_deck;
            } else {
                d2_screen_state[deck].phase_master_deck = deck;
                d2_screen_state[deck].phase_follower_deck = deck == 1 ? 2 : 1;
            }
        }
    } else if (strcmp(key, "LEDPACK") == 0) {
        d2_led_state[deck].outputs_enabled = 1;
        char packed[192];
        snprintf(packed, sizeof(packed), "%s", value);
        char *save = NULL;
        char *token = strtok_r(packed, ",", &save);
        int field = 0;
        while (token && field < 20) {
            int parsed = (int)strtol(token, NULL, field >= 12 ? 16 : 10);
            switch (field) {
            case 0: d2_led_state[deck].play = parsed != 0; break;
            case 1: d2_led_state[deck].cue = parsed != 0; break;
            case 2: d2_led_state[deck].sync = parsed != 0; break;
            case 3: d2_led_state[deck].flux = parsed != 0; break;
            case 4: d2_led_state[deck].shift = parsed != 0; break;
            case 5: d2_led_state[deck].loop = parsed != 0; break;
            case 6:
                if (parsed >= 1 && parsed <= 4)
                    d2_led_state[deck].active_channel = parsed;
                break;
            case 7:
                if (parsed >= 1 && parsed <= 5)
                    d2_led_state[deck].mode = parsed;
                break;
            case 8:
                d2_led_state[deck].fx_unit = deck == 2 ? 2 : 1;
                break;
            case 9:
                for (int fx = 0; fx < 4; ++fx)
                    d2_led_state[deck].fx_enabled[fx] = (parsed >> fx) & 1;
                break;
            case 10:
                for (int strip = 0; strip < 4; ++strip)
                    d2_led_state[deck].performance_on[strip] = (parsed >> strip) & 1;
                break;
            case 11:
                d2_led_state[deck].fx_assign_mask = parsed & 0x0f;
                break;
            default:
                d2_led_state[deck].pad_rgb[field - 12] =
                    (uint32_t)parsed & 0x00ffffffU;
                break;
            }
            token = strtok_r(NULL, ",", &save);
            ++field;
        }
    } else if (strcmp(key, "LEDPLAY") == 0) {
        d2_led_state[deck].play = atoi(value) != 0;
    } else if (strcmp(key, "LEDCUE") == 0) {
        d2_led_state[deck].cue = atoi(value) != 0;
    } else if (strcmp(key, "LEDSYNC") == 0) {
        d2_led_state[deck].sync = atoi(value) != 0;
    } else if (strcmp(key, "LEDFLUX") == 0) {
        d2_led_state[deck].flux = atoi(value) != 0;
    } else if (strcmp(key, "LEDSHIFT") == 0) {
        d2_led_state[deck].shift = atoi(value) != 0;
    } else if (strcmp(key, "LEDLOOP") == 0) {
        d2_led_state[deck].loop = atoi(value) != 0;
    } else if (strcmp(key, "LEDOFF") == 0) {
        d2_led_state[deck].outputs_enabled = 0;
    } else if (strcmp(key, "LEDDECK") == 0) {
        int channel = atoi(value);
        if (channel >= 1 && channel <= 4)
            d2_led_state[deck].active_channel = channel;
    } else if (strcmp(key, "LEDMODE") == 0) {
        int mode = atoi(value);
        if (mode >= 1 && mode <= 5)
            d2_led_state[deck].mode = mode;
    } else if (strcmp(key, "LEDFXSEL") == 0) {
        d2_led_state[deck].fx_unit = deck == 2 ? 2 : 1;
    } else if (strncmp(key, "LEDFX", 5) == 0 && key[5] >= '1' && key[5] <= '4') {
        d2_led_state[deck].fx_enabled[key[5] - '1'] = atoi(value) != 0;
    } else if (strncmp(key, "LEDON", 5) == 0 && key[5] >= '1' && key[5] <= '4') {
        d2_led_state[deck].performance_on[key[5] - '1'] = atoi(value) != 0;
    } else if (strncmp(key, "LEDPAD", 6) == 0 && key[6] >= '1' && key[6] <= '8') {
        d2_led_state[deck].pad_rgb[key[6] - '1'] =
            (uint32_t)strtoul(value, NULL, 10) & 0x00ffffffU;
    } else if (strcmp(key, "BROWSEFOCUS") == 0) {
        int browse_focus = atoi(value) != 0;
        if (d2_screen_state[deck].browse_focus != browse_focus) {
            d2_screen_state[deck].browse_focus = browse_focus;
            d2_browse_mark_dirty();
        }
    } else if (strcmp(key, "BROWSESORT") == 0) {
        int column = 0;
        int order = 0;
        if (sscanf(value, "%d,%d", &column, &order) == 2 &&
            column >= 0 && column < 128) {
            order = order != 0;
            if (d2_screen_state[deck].browse_sort_column != column ||
                d2_screen_state[deck].browse_sort_order != order) {
                d2_screen_state[deck].browse_sort_column = column;
                d2_screen_state[deck].browse_sort_order = order;
                d2_browse_mark_dirty();
            }
        }
    } else if (strcmp(key, "FXTOUCH") == 0) {
        d2_screen_state[deck].fx_touch_mask = atoi(value) & 0x0f;
        d2_fx_touch_updated_us[deck] = d2_monotonic_us();
    } else if (strncmp(key, "FXEN", 4) == 0) {
        int slot = atoi(key + 4) - 1;
        if (slot >= 0 && slot < 4)
            d2_screen_state[deck].fx_enabled[slot] = atoi(value) != 0;
    } else if (strncmp(key, "FXSEL", 5) == 0) {
        int slot = atoi(key + 5) - 1;
        int selection = atoi(value);
        if (slot >= 0 && slot < 3 && selection >= 0) {
            int changed = d2_screen_state[deck].fx_selection[slot] != selection;
            d2_screen_state[deck].fx_selection[slot] = selection;
            /* A selection confirmation always precedes its name snapshot.
             * Do not leave the previous effect's name visible in between. */
            if (changed)
                d2_screen_state[deck].fx_name[slot][0] = '\0';
        }
    } else if (strncmp(key, "FXNAME", 6) == 0 &&
               key[6] >= '1' && key[6] <= '3' && key[7] == '\0') {
        int slot = key[6] - '1';
        char decoded[sizeof(d2_screen_state[deck].fx_name[slot])];
        /* U: versions the wire encoding and, unlike an empty SysEx field,
         * can represent an intentionally empty slot. Malformed input is
         * rejected transactionally so it cannot corrupt a good label. */
        if (strncmp(value, "U:", 2) == 0) {
            if (value[2] == '\0') {
                d2_screen_state[deck].fx_name[slot][0] = '\0';
            } else if (d2_percent_decode(value + 2, decoded,
                                         sizeof(decoded)) &&
                       d2_valid_utf8(decoded)) {
                snprintf(d2_screen_state[deck].fx_name[slot],
                         sizeof(d2_screen_state[deck].fx_name[slot]), "%s",
                         decoded);
            }
        }
    } else if (strncmp(key, "FX", 2) == 0 && key[2] >= '1' && key[2] <= '4') {
        int slot = key[2] - '1';
        float parameter = strtof(value, NULL);
        if (parameter >= 0.0f && parameter <= 1.0f)
            d2_screen_state[deck].fx_parameter[slot] = parameter;
    } else if (strcmp(key, "STEMCOUNT") == 0) {
        int count = atoi(value);
        if (count >= 0 && count <= 4)
            d2_screen_state[deck].stem_count = count;
    } else if (strncmp(key, "STEMVOL", 7) == 0) {
        int stem = atoi(key + 7) - 1;
        float volume = strtof(value, NULL);
        if (stem >= 0 && stem < 4 && volume >= 0.0f && volume <= 1.0f)
            d2_screen_state[deck].stem_volume[stem] = volume;
    } else if (strncmp(key, "STEMMUTE", 8) == 0) {
        int stem = atoi(key + 8) - 1;
        if (stem >= 0 && stem < 4)
            d2_screen_state[deck].stem_muted[stem] = atoi(value) != 0;
    } else if (strcmp(key, "GRIDEDIT") == 0) {
        d2_screen_state[deck].beatgrid_edit = atoi(value) != 0;
    } else if (strncmp(key, "CUECOLOR", 8) == 0) {
        int cue = atoi(key + 8) - 1;
        unsigned long color = strtoul(value, NULL, 10);
        if (cue >= 0 && cue < 8 && color <= 0x00ffffffUL)
            d2_screen_state[deck].hotcue_color[cue] = (uint32_t)color;
    } else if (strncmp(key, "CUE", 3) == 0) {
        int cue = atoi(key + 3) - 1;
        float cue_position = strtof(value, NULL);
        if (cue >= 0 && cue < 8 && cue_position >= -1.0f && cue_position <= 1.0f)
            d2_screen_state[deck].hotcue_position[cue] = cue_position;
    } else if (strcmp(key, "REMAIN") == 0) {
        d2_screen_state[deck].remaining = strtof(value, NULL);
        if (d2_screen_state[deck].remaining < 0.0f)
            d2_screen_state[deck].remaining = 0.0f;
    } else if (strcmp(key, "TITLE") == 0) {
        d2_copy_text(d2_screen_state[deck].title,
                     sizeof(d2_screen_state[deck].title), value);
    } else if (strcmp(key, "ARTIST") == 0) {
        d2_copy_text(d2_screen_state[deck].artist,
                     sizeof(d2_screen_state[deck].artist), value);
    } else if (strncmp(key, "BROWSE", 6) == 0 &&
               key[6] >= '0' && key[6] <= '8' && key[7] == '\0') {
        d2_load_browse_metadata(key[6] - '0', atoi(value));
    } else if (strcmp(key, "VIEW") == 0) {
        d2_browse_mark_dirty();
        if (strcmp(value, "DECK") == 0) {
            d2_screen_view[deck] = D2_VIEW_DECK;
            d2_clear_browse_notice(deck);
        } else if (strcmp(value, "BROWSE") == 0)
            d2_screen_view[deck] = D2_VIEW_BROWSE;
        else if (strcmp(value, "HOTCUE") == 0)
            d2_screen_view[deck] = D2_VIEW_HOTCUE;
        else if (strcmp(value, "LOOP") == 0)
            d2_screen_view[deck] = D2_VIEW_LOOP;
        else if (strcmp(value, "SAMPLER") == 0)
            d2_screen_view[deck] = D2_VIEW_SAMPLER;
        else if (strcmp(value, "FREEZE") == 0)
            d2_screen_view[deck] = D2_VIEW_FREEZE;
        else if (strcmp(value, "BEATJUMP") == 0)
            d2_screen_view[deck] = D2_VIEW_BEATJUMP;
    } else if (strcmp(key, "LOADREJECT") == 0) {
        if (strcmp(value, "PLAYING") == 0)
            d2_set_browse_notice(deck, D2_BROWSE_NOTICE_DECK_PLAYING,
                                 d2_monotonic_us());
    } else if (strcmp(key, "LOADFAIL") == 0) {
        enum d2_browse_notice_kind kind = D2_BROWSE_NOTICE_LOAD_FAILED;
        if (strcmp(value, "MISSING") == 0)
            kind = D2_BROWSE_NOTICE_TRACK_MISSING;
        else if (strcmp(value, "NOSELECTION") == 0)
            kind = D2_BROWSE_NOTICE_NO_SELECTION;
        /* A failure is useful only while the corresponding D2 is still in
         * Browse. Never retain an asynchronous old-deck failure and reveal it
         * when Browse is opened later. */
        if (d2_screen_view[deck] == D2_VIEW_BROWSE)
            d2_set_browse_notice(deck, kind, d2_monotonic_us());
    } else {
        return;
    }

    if (D2_VERBOSE) {
        printf("D2 SCREEN RX: deck=%d %s=%s\n", deck, key, value);
        fflush(stdout);
    }
}


static int midi_init(void)
{
    int err = snd_seq_open(
        &midi_seq,
        "default",
        SND_SEQ_OPEN_DUPLEX,
        0);

    if (err < 0) {
        fprintf(stderr,
                "MIDI open failed: %s\n",
                snd_strerror(err));
        return -1;
    }

    snd_seq_set_client_name(midi_seq, "D2 Bridge");

    midi_port = snd_seq_create_simple_port(
        midi_seq,
        "D2 MIDI",
        SND_SEQ_PORT_CAP_READ |
        SND_SEQ_PORT_CAP_SUBS_READ |
        /* Allow Mixxx (or aconnect) to send SysEx/data back to this bridge. */
        SND_SEQ_PORT_CAP_WRITE |
        SND_SEQ_PORT_CAP_SUBS_WRITE,
        SND_SEQ_PORT_TYPE_APPLICATION);

    if (midi_port < 0) {
        fprintf(stderr,
                "MIDI port failed: %s\n",
                snd_strerror(midi_port));

        snd_seq_close(midi_seq);
        midi_seq = NULL;
        return -1;
    }

    printf("MIDI READY: client=%d port=%d\n",
           snd_seq_client_id(midi_seq),
           midi_port);

    return 0;
}


static void midi_note(int channel, int note, int velocity)
{
    if (!midi_seq)
        return;

    snd_seq_event_t ev;
    snd_seq_ev_clear(&ev);

    if (velocity > 0) {
        snd_seq_ev_set_noteon(&ev, channel, note, velocity);
    } else {
        /* Different Mixxx/PortMidi backends handle release differently.  The
         * preset is Note-On based, while some builds only dispatch a raw
         * Note-Off. Emit both equivalent forms; the JS state machine ignores
         * the harmless duplicate release after it clears preview state. */
        snd_seq_ev_set_noteon(&ev, channel, note, 0);
        snd_seq_ev_set_source(&ev, midi_port);
        snd_seq_ev_set_subs(&ev);
        snd_seq_ev_set_direct(&ev);
        snd_seq_event_output_direct(midi_seq, &ev);

        snd_seq_ev_clear(&ev);
        snd_seq_ev_set_noteoff(&ev, channel, note, 0);
    }

    snd_seq_ev_set_source(&ev, midi_port);
    snd_seq_ev_set_subs(&ev);
    snd_seq_ev_set_direct(&ev);
    snd_seq_event_output_direct(midi_seq, &ev);
}


static void midi_cc(int channel, int cc, int value)
{
    if (!midi_seq)
        return;

    snd_seq_event_t ev;
    snd_seq_ev_clear(&ev);

    snd_seq_ev_set_controller(
        &ev,
        channel,
        cc,
        value);

    snd_seq_ev_set_source(&ev, midi_port);
    snd_seq_ev_set_subs(&ev);
    snd_seq_ev_set_direct(&ev);

    snd_seq_event_output_direct(midi_seq, &ev);
}


static void midi_shutdown(void)
{
    if (midi_seq) {
        snd_seq_close(midi_seq);
        midi_seq = NULL;
        midi_port = -1;
    }
}


static void midi_input_poll(void)
{
    if (!midi_seq)
        return;

    snd_seq_event_t *ev = NULL;

    /* Fetch pending events from the ALSA kernel queue before reading them. */
    while (snd_seq_event_input_pending(midi_seq, 1) > 0) {
        if (snd_seq_event_input(midi_seq, &ev) < 0)
            break;

        if (!ev)
            continue;

        if (ev->type == SND_SEQ_EVENT_SYSEX) {
            if (D2_VERBOSE)
                printf("MIDI SYSEX RX: %d bytes\n", ev->data.ext.len);
            d2_parse_sysex(ev->data.ext.ptr, ev->data.ext.len);
            fflush(stdout);
        }

        snd_seq_free_event(ev);
        ev = NULL;
    }
}

static void stop_handler(int sig)
{
    (void)sig;
    running = 0;
}

static void capture_handler(int sig)
{
    (void)sig;
    d2_capture_requested = 1;
}

/* RGB888 -> RGB565 */
static inline void rgb565(uint8_t *p, int r, int g, int b)
{
    uint16_t v =
        (((uint16_t)(r >> 3)) << 11) |
        (((uint16_t)(g >> 2)) << 5) |
        ((uint16_t)(b >> 3));

    p[0] = (uint8_t)(v >> 8);
    p[1] = (uint8_t)(v & 0xff);
}

/* The XDJ100SX skin asks Qt for Helvetica Neue.  It is not shipped on the
 * Pi, so Liberation Sans is used as the closest installed, metrically
 * compatible sans-serif.  FreeType rasterizes it directly into RGB565 so
 * the deck view is no longer limited by the old 5x7 controller font. */
static void d2_font_init(void)
{
    if (d2_ft_ready)
        return;
    if (FT_Init_FreeType(&d2_ft_library) != 0)
        return;
    if (FT_New_Face(d2_ft_library,
                    "/usr/share/fonts/truetype/liberation/LiberationSans-Regular.ttf",
                    0, &d2_ft_face) != 0) {
        FT_Done_FreeType(d2_ft_library);
        d2_ft_library = NULL;
        return;
    }
    d2_ft_ready = 1;
}

static void d2_font_shutdown(void)
{
    if (d2_ft_face)
        FT_Done_Face(d2_ft_face);
    if (d2_ft_library)
        FT_Done_FreeType(d2_ft_library);
    d2_ft_face = NULL;
    d2_ft_library = NULL;
    d2_ft_ready = 0;
}

static int d2_font_pixel_size(int scale)
{
    return scale == 1 ? 9 : (scale == 2 ? 14 : 21);
}

/* Materialize the complete, coloured waveform once at track-load time.
 * This keeps RGB conversion and amplitude-to-pixel expansion out of the
 * 60 FPS path.  The byte order intentionally uses rgb565() because it is
 * the proven wire order of the current D2 bulk screen driver. */
static void d2_build_wave_strip(int deck)
{
    if (deck < 0 || deck >= D2_ASSET_SLOTS || !d2_wave_strip[deck] ||
        !d2_waveform[deck] || !d2_waveform_low[deck] ||
        !d2_waveform_mid[deck] || !d2_waveform_high[deck] ||
        !d2_waveform_ready[deck])
        return;
    for (int y = 0; y < D2_WAVEFORM_HEIGHT; ++y)
        for (int x = 0; x < D2_WAVEFORM_POINTS; ++x)
            rgb565(&d2_wave_strip[deck][y][x * 2], 1, 3, 6);

    const int center = D2_WAVEFORM_HEIGHT / 2;
    for (int x = 0; x < D2_WAVEFORM_POINTS; ++x) {
        int amplitude = d2_waveform[deck][x];
        if (amplitude > center - 1) amplitude = center - 1;
        int low = d2_waveform_low[deck][x];
        int mid = d2_waveform_mid[deck][x];
        int high = d2_waveform_high[deck][x];

        /* Mixxx's coloured waveform is not a single cyan amplitude line:
         * bass provides the warm outer body, while mids/highs form the
         * brighter layers around the centre line.  Keep all conversion here
         * (at track load), leaving the 60 Hz path as pure RGB565 copies. */
        int energy = low * 50 + mid * 35 + high * 15;
        int outer = amplitude;
        if (energy > 0)
            outer = 5 + (int)(sqrt((double)energy / 100.0) * 4.0);
        if (outer < amplitude * 4 / 5) outer = amplitude * 4 / 5;
        if (outer > center - 1) outer = center - 1;

        int mid_height = 2 + (int)(outer * (0.14 + 0.48 * mid / 255.0));
        int high_height = 1 + (int)(outer * (0.06 + 0.30 * high / 255.0));
        if (mid_height > outer) mid_height = outer;
        if (high_height > mid_height) high_height = mid_height;

        /* Traktor Nexus target hierarchy: warm transient peaks outside,
         * a blue mid-frequency body and a bright cyan high-frequency core. */
        int bass_r = 150;
        int bass_g = 25;
        int bass_b = 12;
        for (int y = center - outer; y <= center + outer; ++y)
            rgb565(&d2_wave_strip[deck][y][x * 2], bass_r, bass_g, bass_b);

        /* Mid-frequency body: muted Traktor blue. */
        int middle_r = 49;
        int middle_g = 91;
        int middle_b = 155;
        for (int y = center - mid_height; y <= center + mid_height; ++y)
            rgb565(&d2_wave_strip[deck][y][x * 2], middle_r, middle_g, middle_b);

        /* High-frequency core: bright ice-cyan, drawn last. */
        int treble_r = 126;
        int treble_g = 197;
        int treble_b = 232;
        for (int y = center - high_height; y <= center + high_height; ++y)
            rgb565(&d2_wave_strip[deck][y][x * 2], treble_r, treble_g, treble_b);
    }
    d2_wave_strip_ready[deck] = 1;
    printf("D2 WAVE STRIP: deck=%d %dx%d RGB565 cached\n", deck,
           D2_WAVEFORM_POINTS, D2_WAVEFORM_HEIGHT);
    fflush(stdout);
}

/* Compact 5x7 bitmap glyphs, scaled in the screen callback. */
static const uint8_t d2_font[128][5] = {
    [' '] = {0x00, 0x00, 0x00, 0x00, 0x00},
    ['-'] = {0x08, 0x08, 0x08, 0x08, 0x08},
    ['.'] = {0x00, 0x60, 0x60, 0x00, 0x00},
    [':'] = {0x00, 0x36, 0x36, 0x00, 0x00},
    ['0'] = {0x3E, 0x51, 0x49, 0x45, 0x3E},
    ['1'] = {0x00, 0x42, 0x7F, 0x40, 0x00},
    ['2'] = {0x42, 0x61, 0x51, 0x49, 0x46},
    ['3'] = {0x21, 0x41, 0x45, 0x4B, 0x31},
    ['4'] = {0x18, 0x14, 0x12, 0x7F, 0x10},
    ['5'] = {0x27, 0x45, 0x45, 0x45, 0x39},
    ['6'] = {0x3C, 0x4A, 0x49, 0x49, 0x30},
    ['7'] = {0x01, 0x71, 0x09, 0x05, 0x03},
    ['8'] = {0x36, 0x49, 0x49, 0x49, 0x36},
    ['9'] = {0x06, 0x49, 0x49, 0x29, 0x1E},
    ['A'] = {0x7E, 0x11, 0x11, 0x11, 0x7E},
    ['B'] = {0x7F, 0x49, 0x49, 0x49, 0x36},
    ['C'] = {0x3E, 0x41, 0x41, 0x41, 0x22},
    ['D'] = {0x7F, 0x41, 0x41, 0x22, 0x1C},
    ['E'] = {0x7F, 0x49, 0x49, 0x49, 0x41},
    ['F'] = {0x7F, 0x09, 0x09, 0x09, 0x01},
    ['G'] = {0x3E, 0x41, 0x49, 0x49, 0x7A},
    ['H'] = {0x7F, 0x08, 0x08, 0x08, 0x7F},
    ['I'] = {0x00, 0x41, 0x7F, 0x41, 0x00},
    ['J'] = {0x20, 0x40, 0x41, 0x3F, 0x01},
    ['K'] = {0x7F, 0x08, 0x14, 0x22, 0x41},
    ['L'] = {0x7F, 0x40, 0x40, 0x40, 0x40},
    ['M'] = {0x7F, 0x02, 0x0C, 0x02, 0x7F},
    ['N'] = {0x7F, 0x04, 0x08, 0x10, 0x7F},
    ['O'] = {0x3E, 0x41, 0x41, 0x41, 0x3E},
    ['P'] = {0x7F, 0x09, 0x09, 0x09, 0x06},
    ['Q'] = {0x3E, 0x41, 0x51, 0x21, 0x5E},
    ['R'] = {0x7F, 0x09, 0x19, 0x29, 0x46},
    ['S'] = {0x46, 0x49, 0x49, 0x49, 0x31},
    ['T'] = {0x01, 0x01, 0x7F, 0x01, 0x01},
    ['U'] = {0x3F, 0x40, 0x40, 0x40, 0x3F},
    ['V'] = {0x1F, 0x20, 0x40, 0x20, 0x1F},
    ['W'] = {0x7F, 0x20, 0x18, 0x20, 0x7F},
    ['X'] = {0x63, 0x14, 0x08, 0x14, 0x63},
    ['Y'] = {0x07, 0x08, 0x70, 0x08, 0x07},
    ['Z'] = {0x61, 0x51, 0x49, 0x45, 0x43},
};

/* Fast raster primitives.  The original prototype evaluated every label at
 * every pixel (several million glyph tests per second).  At 60 FPS that was
 * the actual CPU bottleneck, not waveform analysis or USB bulk bandwidth. */
static inline void d2_put_pixel(uint8_t *pixels, int x, int y,
                                int r, int g, int b)
{
    if (x >= 0 && x < WIDTH && y >= 0 && y < HEIGHT)
        rgb565(&pixels[(y * WIDTH + x) * 2], r, g, b);
}

static void d2_fill_rect(uint8_t *pixels, int x0, int y0, int width,
                         int height, int r, int g, int b)
{
    int x1 = x0 + width;
    int y1 = y0 + height;
    if (x0 < 0) x0 = 0;
    if (y0 < 0) y0 = 0;
    if (x1 > WIDTH) x1 = WIDTH;
    if (y1 > HEIGHT) y1 = HEIGHT;
    for (int y = y0; y < y1; ++y)
        for (int x = x0; x < x1; ++x)
            rgb565(&pixels[(y * WIDTH + x) * 2], r, g, b);
}

static const char *d2_visual_key_text(const struct d2_screen_state *state)
{
    static const char *const keys[25] = {
        "--", "C", "Db", "D", "Eb", "E", "F", "F#", "G", "Ab",
        "A", "Bb", "B", "Cm", "C#m", "Dm", "Ebm", "Em", "Fm",
        "F#m", "Gm", "G#m", "Am", "Bbm", "Bm"
    };
    if (state && state->visual_key >= 1 && state->visual_key <= 24)
        return keys[state->visual_key];
    return state && state->musical_key[0] ? state->musical_key : "--";
}

static void d2_draw_text(uint8_t *pixels, const char *text, int x, int y,
                         int scale, int r, int g, int b);

static void d2_draw_phase_row(uint8_t *pixels, int phase_step, int y,
                              int r, int g, int b, int filled)
{
    const int box_width = 34;
    const int box_height = 9;
    const int period = 40;
    int active_box = ((phase_step % 4) + 4) % 4;

    /* The geometry never moves and no progressive fill is used.  Exactly one
     * of the four fixed cells lights for each beat-phase quarter. */
    for (int box = 0; box < 4; ++box) {
        int x = 161 + box * period;
        d2_fill_rect(pixels, x, y, box_width, box_height, r, g, b);
        d2_fill_rect(pixels, x + 2, y + 2, box_width - 4, box_height - 4,
                     0, 0, 0);
        if (box == active_box)
            d2_fill_rect(pixels, x + 2, y + 2, box_width - 4, box_height - 4,
                         filled ? r : (r * 4 / 5),
                         filled ? g : (g * 4 / 5),
                         filled ? b : (b * 4 / 5));
    }
}

static void d2_draw_phase_meter(uint8_t *pixels,
                                const struct d2_screen_state *state,
                                int player)
{
    (void)player;
    if (!state || !state->phase_valid)
        return;
    const char *master_label = state->phase_master_deck == 2 ? "B" : "A";
    const char *follower_label = state->phase_follower_deck == 1 ? "A" : "B";
    d2_fill_rect(pixels, 156, 22, 168, 25, 0, 0, 0);
    /* Both physical displays use the same row order. Amber is always the
     * Sync Leader/reference deck; white is always the follower. Explicit
     * A/B labels make a leader change understandable instead of making one
     * screen's meter disappear or silently reverse its meaning. */
    d2_draw_text(pixels, master_label, 145, 18, 1,
                 244, 150, 18);
    d2_draw_text(pixels, follower_label, 145, 31, 1,
                 225, 229, 234);
    d2_draw_phase_row(pixels, state->phase_master_step,
                      24, 244, 150, 18, 0);
    d2_draw_phase_row(pixels, state->phase_active_step,
                      36, 225, 229, 234, 1);
}

static void d2_draw_text_bitmap(uint8_t *pixels, const char *text, int x, int y,
                                int scale, int r, int g, int b)
{
    if (!text || scale < 1)
        return;
    for (int character = 0; text[character]; ++character) {
        unsigned char glyph = (unsigned char)text[character];
        if (glyph >= 'a' && glyph <= 'z') glyph -= 'a' - 'A';
        if (glyph >= 128) glyph = '?';
        for (int column = 0; column < 5; ++column)
            for (int row = 0; row < 7; ++row)
                if (d2_font[glyph][column] & (1u << row))
                    d2_fill_rect(pixels, x + (character * 6 + column) * scale,
                                 y + row * scale, scale, scale, r, g, b);
    }
}

static uint32_t d2_utf8_next(const char **cursor)
{
    const unsigned char *s = (const unsigned char *)*cursor;
    uint32_t codepoint;
    if (!*s)
        return 0;
    if (s[0] < 0x80) {
        *cursor = (const char *)(s + 1);
        return s[0];
    }
    if ((s[0] & 0xe0) == 0xc0 && (s[1] & 0xc0) == 0x80) {
        codepoint = ((uint32_t)(s[0] & 0x1f) << 6) | (s[1] & 0x3f);
        *cursor = (const char *)(s + 2);
        return codepoint;
    }
    if ((s[0] & 0xf0) == 0xe0 && (s[1] & 0xc0) == 0x80 &&
        (s[2] & 0xc0) == 0x80) {
        codepoint = ((uint32_t)(s[0] & 0x0f) << 12) |
                    ((uint32_t)(s[1] & 0x3f) << 6) | (s[2] & 0x3f);
        *cursor = (const char *)(s + 3);
        return codepoint;
    }
    if ((s[0] & 0xf8) == 0xf0 && (s[1] & 0xc0) == 0x80 &&
        (s[2] & 0xc0) == 0x80 && (s[3] & 0xc0) == 0x80) {
        codepoint = ((uint32_t)(s[0] & 0x07) << 18) |
                    ((uint32_t)(s[1] & 0x3f) << 12) |
                    ((uint32_t)(s[2] & 0x3f) << 6) | (s[3] & 0x3f);
        *cursor = (const char *)(s + 4);
        return codepoint;
    }
    *cursor = (const char *)(s + 1);
    return '?';
}

static void d2_blend_pixel(uint8_t *pixels, int x, int y,
                           int r, int g, int b, unsigned alpha)
{
    if (x < 0 || x >= WIDTH || y < 0 || y >= HEIGHT || alpha == 0)
        return;
    uint8_t *pixel = &pixels[(y * WIDTH + x) * 2];
    uint16_t current = ((uint16_t)pixel[0] << 8) | pixel[1];
    int old_r = ((current >> 11) & 0x1f) * 255 / 31;
    int old_g = ((current >> 5) & 0x3f) * 255 / 63;
    int old_b = (current & 0x1f) * 255 / 31;
    rgb565(pixel, (r * (int)alpha + old_r * (255 - (int)alpha)) / 255,
           (g * (int)alpha + old_g * (255 - (int)alpha)) / 255,
           (b * (int)alpha + old_b * (255 - (int)alpha)) / 255);
}

static void d2_draw_text(uint8_t *pixels, const char *text, int x, int y,
                         int scale, int r, int g, int b)
{
    const int pixel_size = d2_font_pixel_size(scale);
    if (!text || scale < 1 || !d2_ft_ready) {
        d2_draw_text_bitmap(pixels, text, x, y, scale, r, g, b);
        return;
    }
    const char *cursor = text;
    int pen_x = x;
    const int baseline = y + pixel_size;
    while (*cursor) {
        uint32_t codepoint = d2_utf8_next(&cursor);
        int size_index = scale - 1;
        struct d2_cached_glyph *cached =
            codepoint < D2_FONT_CACHE_GLYPHS ?
            &d2_font_cache[size_index][codepoint] : NULL;
        if (cached && !cached->ready) {
            FT_Set_Pixel_Sizes(d2_ft_face, 0, pixel_size);
            if (FT_Load_Char(d2_ft_face, codepoint, FT_LOAD_RENDER) == 0) {
                FT_GlyphSlot glyph = d2_ft_face->glyph;
                FT_Bitmap *bitmap = &glyph->bitmap;
                cached->width = bitmap->width;
                cached->rows = bitmap->rows;
                cached->left = glyph->bitmap_left;
                cached->top = glyph->bitmap_top;
                cached->advance = glyph->advance.x >> 6;
                if (cached->width <= D2_FONT_CACHE_SIDE &&
                    cached->rows <= D2_FONT_CACHE_SIDE) {
                    for (int row = 0; row < cached->rows; ++row)
                        memcpy(&cached->bitmap[row * D2_FONT_CACHE_SIDE],
                               &bitmap->buffer[row * bitmap->pitch],
                               (size_t)cached->width);
                } else {
                    cached = NULL;
                }
            } else {
                cached = NULL;
            }
            if (cached)
                cached->ready = 1;
        }
        if (cached && cached->ready) {
            for (int row = 0; row < cached->rows; ++row)
                for (int column = 0; column < cached->width; ++column)
                    d2_blend_pixel(pixels, pen_x + cached->left + column,
                                   baseline - cached->top + row, r, g, b,
                                   cached->bitmap[row * D2_FONT_CACHE_SIDE + column]);
            pen_x += cached->advance;
        } else {
            FT_Set_Pixel_Sizes(d2_ft_face, 0, pixel_size);
            if (FT_Load_Char(d2_ft_face, codepoint, FT_LOAD_RENDER) != 0)
                continue;
            FT_GlyphSlot glyph = d2_ft_face->glyph;
            FT_Bitmap *bitmap = &glyph->bitmap;
            for (unsigned row = 0; row < bitmap->rows; ++row)
                for (unsigned column = 0; column < bitmap->width; ++column)
                    d2_blend_pixel(pixels, pen_x + glyph->bitmap_left + (int)column,
                                   baseline - glyph->bitmap_top + (int)row,
                                   r, g, b, bitmap->buffer[row * bitmap->pitch + column]);
            pen_x += glyph->advance.x >> 6;
        }
        if (pen_x >= WIDTH)
            break;
    }
}

static int d2_measure_text_width(const char *text, int scale)
{
    if (!text || !*text || scale < 1)
        return 0;
    if (!d2_ft_ready)
        return (int)strlen(text) * 6 * scale;

    const int pixel_size = d2_font_pixel_size(scale);
    const char *cursor = text;
    int width = 0;
    FT_Set_Pixel_Sizes(d2_ft_face, 0, pixel_size);
    while (*cursor) {
        uint32_t codepoint = d2_utf8_next(&cursor);
        if (FT_Load_Char(d2_ft_face, codepoint, FT_LOAD_DEFAULT) == 0)
            width += d2_ft_face->glyph->advance.x >> 6;
    }
    return width;
}

static void d2_fit_text(char *destination, size_t destination_size,
                        const char *source, int scale, int max_width)
{
    char candidate[96];
    size_t length;

    if (!destination || destination_size == 0)
        return;
    snprintf(destination, destination_size, "%s", source ? source : "");
    if (d2_measure_text_width(destination, scale) <= max_width)
        return;

    snprintf(candidate, sizeof(candidate), "%s", destination);
    length = strlen(candidate);
    while (length > 0) {
        do {
            --length;
        } while (length > 0 &&
                 (((unsigned char)candidate[length] & 0xc0) == 0x80));
        candidate[length] = '\0';
        snprintf(destination, destination_size, "%s...", candidate);
        if (d2_measure_text_width(destination, scale) <= max_width)
            return;
    }
    destination[0] = '\0';
}

static void d2_fx_slot_label(char *destination, size_t destination_size,
                             const struct d2_screen_state *state, int effect,
                             int max_width)
{
    char full_label[96];
    int selected = state->fx_selection[effect];

    if (selected <= 0) {
        snprintf(full_label, sizeof(full_label), "FX%d EMPTY", effect + 1);
    } else if (state->fx_name[effect][0]) {
        snprintf(full_label, sizeof(full_label), "FX%d %s", effect + 1,
                 state->fx_name[effect]);
    } else {
        snprintf(full_label, sizeof(full_label), "FX%d LOADING", effect + 1);
    }
    d2_fit_text(destination, destination_size, full_label, 1, max_width);
}

static void d2_draw_text_centered(uint8_t *pixels, const char *text,
                                  int center_x, int y, int scale,
                                  int r, int g, int b)
{
    int width = d2_measure_text_width(text, scale);
    d2_draw_text(pixels, text, center_x - width / 2, y, scale, r, g, b);
}

static void d2_render_dynamic_performance_view(
        uint8_t *pixels, enum d2_screen_view view, const char *deck_title,
        const uint32_t pad_rgb[8],
        int accent_r, int accent_g, int accent_b)
{
    const char *heading = d2_performance_heading(view);
    const char *instruction = d2_performance_instruction(view);

    d2_fill_rect(pixels, 0, 0, WIDTH, HEIGHT, 12, 14, 18);
    d2_fill_rect(pixels, 0, 0, WIDTH, 62, 21, 23, 29);
    d2_fill_rect(pixels, 0, 59, WIDTH, 3,
                 accent_r, accent_g, accent_b);
    d2_draw_text(pixels, heading, 18, 10, 3, 240, 240, 245);
    if (deck_title && deck_title[0])
        d2_draw_text(pixels, deck_title, 18, 42, 1, 180, 190, 205);

    /* Keep the already-approved 440x128 performance area, but make every
     * physical pad a live cell. LEDPACK is the one authoritative color state
     * shared by the hardware LEDs and this screen. */
    for (int pad = 0; pad < 8; ++pad) {
        int column = pad % 4;
        int row = pad / 4;
        int x = 24 + column * 110;
        int y = 80 + row * 64;
        uint32_t rgb = pad_rgb ? pad_rgb[pad] & 0x00ffffffU : 0;
        int r = (int)((rgb >> 16) & 0xff);
        int g = (int)((rgb >> 8) & 0xff);
        int b = (int)(rgb & 0xff);
        if (r + g + b < 18)
            r = g = b = 10;
        d2_fill_rect(pixels, x, y, 98, 52,
                     accent_r / 2, accent_g / 2, accent_b / 2);
        d2_fill_rect(pixels, x + 2, y + 2, 94, 48, r, g, b);
        /* The pad value is the primary information in these views.  Use the
         * 21 px font and retain the y offset so the larger glyph's visible
         * ink is optically centred in the 52 px cell.  The previous 14 px
         * label sat visibly high and looked undersized on the physical D2. */
        d2_draw_text_centered(pixels,
                              d2_performance_pad_label(view, pad),
                              x + 49, y + 13, 3, 250, 250, 255);
    }

    d2_draw_text_centered(pixels, instruction, WIDTH / 2, 214, 1,
                          180, 186, 196);
    d2_draw_text(pixels, "DECK TO EXIT", 20, 240, 1, 150, 155, 165);
    d2_fill_rect(pixels, 0, 0, WIDTH, 1, accent_r, accent_g, accent_b);
    d2_fill_rect(pixels, 0, HEIGHT - 1, WIDTH, 1,
                 accent_r, accent_g, accent_b);
    d2_fill_rect(pixels, 0, 0, 1, HEIGHT, accent_r, accent_g, accent_b);
    d2_fill_rect(pixels, WIDTH - 1, 0, 1, HEIGHT,
                 accent_r, accent_g, accent_b);
}

static void d2_draw_browse_art(uint8_t *pixels, int x, int y,
                               const struct d2_browse_entry *entry,
                               int selected)
{
    int has_track = entry && entry->track_id > 0;
    int available = has_track && entry->available;
    d2_fill_rect(pixels, x, y, 23, 23, selected ? 255 : 32,
                 selected ? 170 : 38, selected ? 45 : 48);
    d2_fill_rect(pixels, x + 2, y + 2, 19, 19,
                 available ? 18 : 52, available ? 34 : 12,
                 available ? 42 : 12);
    if (available) {
        /* Honest track glyph: the old track_id modulo digit looked like a
         * cue/index value even though it represented no Mixxx state. */
        d2_fill_rect(pixels, x + 12, y + 5, 2, 11, 76, 218, 235);
        d2_fill_rect(pixels, x + 13, y + 5, 5, 2, 76, 218, 235);
        d2_fill_rect(pixels, x + 8, y + 14, 6, 5, 76, 218, 235);
    } else if (has_track) {
        d2_fill_rect(pixels, x + 6, y + 6, 11, 2, 235, 70, 55);
        d2_fill_rect(pixels, x + 6, y + 15, 11, 2, 235, 70, 55);
        for (int offset = 0; offset < 8; ++offset) {
            d2_fill_rect(pixels, x + 7 + offset, y + 7 + offset,
                         2, 2, 235, 70, 55);
            d2_fill_rect(pixels, x + 14 - offset, y + 7 + offset,
                         2, 2, 235, 70, 55);
        }
    } else {
        d2_fill_rect(pixels, x + 7, y + 11, 9, 2, 82, 90, 98);
    }
}

static void d2_draw_browse_notice(uint8_t *pixels,
                                  enum d2_browse_notice_kind kind)
{
    const int x = 67;
    const int y = 102;
    const int width = 346;
    const int height = 68;
    const char *heading = "LOAD FAILED";
    const char *detail = "CHECK FILE";
    int border_r = 232;
    int border_g = 58;
    int border_b = 42;
    int detail_r = 244;
    int detail_g = 92;
    int detail_b = 64;

    if (kind == D2_BROWSE_NOTICE_DECK_PLAYING) {
        heading = "DECK PLAYING";
        detail = "STOP TO LOAD";
    } else if (kind == D2_BROWSE_NOTICE_TRACK_MISSING) {
        heading = "TRACK OFFLINE";
        detail = "FILE NOT FOUND";
        border_r = 240;
        border_g = 142;
        border_b = 28;
        detail_r = 255;
        detail_g = 190;
        detail_b = 70;
    } else if (kind == D2_BROWSE_NOTICE_NO_SELECTION) {
        heading = "NO TRACK SELECTED";
        detail = "TURN BROWSE";
        border_r = 0;
        border_g = 170;
        border_b = 205;
        detail_r = 45;
        detail_g = 210;
        detail_b = 235;
    }

    /* Opaque compositing keeps the safety message readable over every album
     * color and costs only one cached Browse redraw at show/hide time. */
    d2_fill_rect(pixels, x, y, width, height, 7, 8, 10);
    d2_fill_rect(pixels, x, y, width, 2, border_r, border_g, border_b);
    d2_fill_rect(pixels, x, y + height - 2, width, 2,
                 border_r, border_g, border_b);
    d2_fill_rect(pixels, x, y, 2, height, border_r, border_g, border_b);
    d2_fill_rect(pixels, x + width - 2, y, 2, height,
                 border_r, border_g, border_b);
    d2_draw_text_centered(
        pixels, heading, x + width / 2, 111, 2, 255, 255, 255);
    d2_draw_text_centered(
        pixels, detail, x + width / 2, 140, 2,
        detail_r, detail_g, detail_b);
}

static void d2_trim_line(char *text)
{
    size_t len;
    if (!text)
        return;
    len = strlen(text);
    while (len > 0 && (text[len - 1] == '\n' || text[len - 1] == '\r'))
        text[--len] = '\0';
}

static int d2_load_library_browse_state(void)
{
    const char *path = "/tmp/mixxx-d2-library-state";
    struct stat st;
    FILE *file;
    char line[256];

    if (stat(path, &st) != 0)
        return 0;
    if (d2_library_browse.ready &&
        st.st_mtim.tv_sec == d2_library_browse.file_mtime.tv_sec &&
        st.st_mtim.tv_nsec == d2_library_browse.file_mtime.tv_nsec)
        return 0;
    file = fopen(path, "r");
    if (!file)
        return 0;
    while (fgets(line, sizeof(line), file)) {
        char *tab = strchr(line, '\t');
        d2_trim_line(line);
        if (!tab)
            continue;
        *tab++ = '\0';
        if (strcmp(line, "CONTEXT") == 0) {
            snprintf(d2_library_browse.context,
                     sizeof(d2_library_browse.context), "%s", tab);
        } else if (strcmp(line, "TRACK") == 0) {
            sscanf(tab, "%d\t%d", &d2_library_browse.track_row,
                   &d2_library_browse.track_count);
        } else if (strcmp(line, "SIDEBAR") == 0) {
            sscanf(tab, "%d\t%d", &d2_library_browse.sidebar_row,
                   &d2_library_browse.sidebar_count);
        } else if (strncmp(line, "ITEM", 4) == 0) {
            int row = atoi(line + 4);
            if (row >= 0 && row < D2_BROWSE_ROWS)
                snprintf(d2_library_browse.sidebar_item[row],
                         sizeof(d2_library_browse.sidebar_item[row]), "%s", tab);
        }
    }
    fclose(file);
    d2_library_browse.file_mtime = st.st_mtim;
    d2_library_browse.ready = 1;
    return 1;
}

static void d2_render_browse_fast(uint8_t *pixels, int player,
                                  const struct d2_screen_state *state)
{
    int track_selected_row = d2_library_browse.track_row;
    int sidebar_selected_row = d2_library_browse.sidebar_row;
    if (track_selected_row < 0 || track_selected_row >= D2_BROWSE_ROWS)
        track_selected_row = 0;
    if (sidebar_selected_row < 0 || sidebar_selected_row >= D2_BROWSE_ROWS)
        sidebar_selected_row = 0;
    const int row_top = 27;
    const int row_height = 25;
    const int sidebar_open = state && state->browse_focus;
    const int sidebar_width = sidebar_open ? 154 : 0;
    const int accent_r = player == 1 ? 236 : 28;
    const int accent_g = player == 1 ? 69 : 170;
    const int accent_b = player == 1 ? 66 : 234;
    char context[45];
    char sort_label[18];

    snprintf(context, sizeof(context), "%.31s",
             d2_library_browse.context[0] ?
             d2_library_browse.context : "ALL TRACKS");
    const char *sort_name = "SORT";
    if (state) {
        if (state->browse_sort_column == 2)
            sort_name = "TITLE";
        else if (state->browse_sort_column == 15)
            sort_name = "BPM";
        else if (state->browse_sort_column == 20)
            sort_name = "KEY";
    }
    snprintf(sort_label, sizeof(sort_label), "%s %s", sort_name,
             state && state->browse_sort_order ? "DESC" : "ASC");

    d2_fill_rect(pixels, 0, 0, WIDTH, HEIGHT, 3, 5, 7);
    d2_fill_rect(pixels, 0, 0, WIDTH, 26, 25, 29, 34);
    d2_fill_rect(pixels, 0, 25, WIDTH, 1, 0, 112, 138);
    d2_draw_text(pixels, "BROWSER >", 5, 5, 1, 188, 198, 208);
    d2_draw_text(pixels, sidebar_open ? "LIBRARY >" : "TRACKS >",
                 54, 5, 1, 205, 213, 220);
    d2_draw_text(pixels, context, 106, 5, 1, 76, 218, 235);
    d2_draw_text(pixels, sort_label, 374, 5, 1, 224, 229, 116);
    d2_draw_text(pixels, player == 1 ? "A" : "B", 469, 5, 1,
                 accent_r, accent_g, accent_b);

    for (int row = 0; row < D2_BROWSE_ROWS; ++row) {
        const struct d2_browse_entry *entry = &d2_browse_entries[row];
        int y = row_top + row * row_height;
        int selected = row == track_selected_row;
        int text_r = !entry->available ? 150 :
            (selected && !sidebar_open ? 135 : 222);
        int text_g = !entry->available ? 82 :
            (selected && !sidebar_open ? 232 : 228);
        int text_b = !entry->available ? 82 :
            (selected && !sidebar_open ? 250 : 235);
        int key_r, key_g, key_b;
        char title[32], artist[24], bpm[8], key[7];
        snprintf(title, sizeof(title), sidebar_open ? "%.22s" : "%.22s",
                 entry->title[0] ? entry->title : "-");
        snprintf(artist, sizeof(artist), "%.17s",
                 entry->artist[0] ? entry->artist : "-");
        snprintf(bpm, sizeof(bpm),
                 !entry->available && entry->track_id > 0 ? "OFF" :
                 (entry->bpm > 0.0f ? "%03.0f" : "---"), entry->bpm);
        d2_format_camelot_key(entry->musical_key[0] ?
                              entry->musical_key : "--",
                              key, sizeof(key));
        d2_browse_key_color(key, &key_r, &key_g, &key_b);

        d2_fill_rect(pixels, sidebar_width, y, WIDTH - sidebar_width, row_height,
                     selected && !sidebar_open ? 5 : (row & 1 ? 7 : 4),
                     selected && !sidebar_open ? 18 : (row & 1 ? 9 : 6),
                     selected && !sidebar_open ? 22 : (row & 1 ? 12 : 9));
        if (selected && !sidebar_open) {
            d2_fill_rect(pixels, sidebar_width, y, WIDTH - sidebar_width, 1,
                         0, 144, 174);
            d2_fill_rect(pixels, sidebar_width, y + row_height - 2,
                         WIDTH - sidebar_width, 1, 0, 144, 174);
            d2_fill_rect(pixels, sidebar_width, y, 3, row_height,
                         0, 185, 215);
        }
        d2_fill_rect(pixels, sidebar_width, y + row_height - 1,
                     WIDTH - sidebar_width, 1, 29, 34, 40);
        if (!sidebar_open) {
            d2_draw_browse_art(pixels, 3, y + 1, entry, selected);
            d2_draw_text(pixels, title, 29, y + 2, 2, text_r, text_g, text_b);
            d2_draw_text(pixels, artist, 224, y + 2, 2,
                         selected ? 216 : 160, selected ? 226 : 171,
                         selected ? 232 : 184);
        } else {
            d2_draw_text(pixels, title, 161, y + 2, 2,
                         text_r, text_g, text_b);
        }
        d2_draw_text(pixels, bpm, 372, y + 2, 2,
                     entry->available ? 218 : 230,
                     entry->available ? 230 : 60,
                     entry->available ? 92 : 60);
        d2_draw_text(pixels, key, 421, y + 2, 2,
                     entry->available ? key_r : 90,
                     entry->available ? key_g : 90,
                     entry->available ? key_b : 90);
        for (int dot = 0; dot < 5; ++dot)
            d2_fill_rect(pixels, 463 + dot * 3, y + 10, 2, 4,
                         dot < entry->rating ? 232 : 74,
                         dot < entry->rating ? 215 : 80,
                         dot < entry->rating ? 84 : 90);
    }

    if (sidebar_open) {
        d2_fill_rect(pixels, 0, row_top, sidebar_width, 225, 8, 11, 14);
        d2_fill_rect(pixels, sidebar_width - 2, row_top, 2, 225, 0, 112, 138);
        for (int row = 0; row < D2_BROWSE_ROWS; ++row) {
            int y = row_top + row * row_height;
            int selected = row == sidebar_selected_row;
            char label[23];
            snprintf(label, sizeof(label), "%.20s",
                     d2_library_browse.sidebar_item[row][0] ?
                     d2_library_browse.sidebar_item[row] : "-");
            d2_fill_rect(pixels, 0, y, sidebar_width - 2, row_height,
                         selected ? 5 : 8, selected ? 18 : 11,
                         selected ? 22 : 14);
            if (selected) {
                d2_fill_rect(pixels, 0, y, sidebar_width - 2, 1, 0, 144, 174);
                d2_fill_rect(pixels, 0, y + row_height - 2,
                             sidebar_width - 2, 1, 0, 144, 174);
                d2_fill_rect(pixels, 0, y, 3, row_height, 0, 185, 215);
            }
            d2_fill_rect(pixels, 0, y + row_height - 1,
                         sidebar_width - 2, 1, 29, 34, 40);
            d2_draw_text(pixels, selected ? ">" : " ", 6, y + 6, 1,
                         255, 255, 255);
            d2_draw_text(pixels, label, 16, y + 6, 1,
                         selected ? 255 : 199, selected ? 255 : 207,
                         selected ? 255 : 216);
        }
    }

    d2_fill_rect(pixels, 0, 252, WIDTH, 20, 19, 23, 28);
    d2_fill_rect(pixels, 0, 251, WIDTH, 1, 50, 58, 66);
    if (sidebar_open) {
        d2_draw_text(pixels, "BACK: TRACKS", 5, 256, 1, 190, 198, 207);
        d2_draw_text(pixels, "TURN: FOLDER", 180, 256, 1, 190, 198, 207);
        d2_draw_text(pixels, "PRESS: OPEN", 402, 256, 1,
                     accent_r, accent_g, accent_b);
    } else {
        d2_draw_text(pixels, "BACK: LIBRARY", 5, 256, 1, 190, 198, 207);
        d2_draw_text(pixels, "L1:T L2:BPM L3:KEY L4:DIR", 139, 256, 1,
                     190, 198, 207);
        d2_draw_text(pixels, player == 1 ? "PRESS: LOAD A" : "PRESS: LOAD B",
                     398, 256, 1, accent_r, accent_g, accent_b);
    }
}

static int d2_beat_position_to_x(float beat_position, float play_position,
                                 float source_step)
{
    if (!isfinite(beat_position) || !isfinite(play_position) ||
        !isfinite(source_step) || source_step <= 0.0f)
        return -1;
    return 240 + (int)lroundf(((beat_position - play_position) *
                               (D2_WAVEFORM_POINTS - 1)) / source_step);
}

static int d2_fx_overlay_active(int player,
                                const struct d2_screen_state *state,
                                int slot, int touch_mask)
{
    if (player < 1 || player > 2 || !state || slot < 0 || slot > 3)
        return 0;
    int active_channel = d2_led_state[player].active_channel;
    int unit_assigned = active_channel >= 1 && active_channel <= 4 &&
        (d2_led_state[player].fx_assign_mask &
         (1 << (active_channel - 1))) != 0;
    return (slot == 0 ? unit_assigned : state->fx_enabled[slot - 1]) ||
           (touch_mask & (1 << slot));
}

static int d2_fx_unit_for_player(int player)
{
    if (player < 1 || player > 2)
        return 1;
    return player;
}

static const char *d2_fx_unit_label(int player)
{
    return d2_fx_unit_for_player(player) == 2 ? "FX UNIT 2" : "FX UNIT 1";
}

static void d2_render_deck_fast(uint8_t *pixels, int player,
                                struct d2_screen_state *state,
                                float position, const char *title,
                                int accent_r, int accent_g, int accent_b)
{
    char bpm[16], time_text[20], tempo[16], loop_text[8], header_title[44];
    int track_loaded = state->duration > 1.0f;
    float elapsed_time = fmaxf(0.0f, position * state->duration);
    float remaining_time = state->duration > 0.0f ?
        fmaxf(0.0f, state->duration - elapsed_time) :
        fmaxf(0.0f, state->remaining);
    float shown_time = state->time_mode ? elapsed_time : remaining_time;
    int seconds = (int)shown_time;
    int centiseconds = (int)((shown_time - seconds) * 100.0f);
    float tempo_value = ((state->rate > 0.0f ? state->rate : 1.0f) - 1.0f) * 100.0f;
    const int wave_left = 3, wave_top = 63, wave_width = 474;
    const int wave_center = wave_top + D2_WAVEFORM_HEIGHT / 2;
    const int zoom_level = state->zoom_level == 4 || state->zoom_level == 8 ?
                           state->zoom_level : 2;
    const float source_step = 2.0f / (float)zoom_level;

    if (track_loaded && state->bpm > 0.0f)
        snprintf(bpm, sizeof(bpm), "%.2f", state->bpm);
    else
        snprintf(bpm, sizeof(bpm), "--");
    snprintf(time_text, sizeof(time_text), state->time_mode ? "%02d:%02d.%02d" :
             "-%02d:%02d.%02d", seconds / 60, seconds % 60, centiseconds);
    snprintf(tempo, sizeof(tempo), "%+.2f%%", tempo_value);
    if (state->loop_size > 0.0f && state->loop_size < 0.375f)
        snprintf(loop_text, sizeof(loop_text), "1/4");
    else if (state->loop_size > 0.0f && state->loop_size < 0.75f)
        snprintf(loop_text, sizeof(loop_text), "1/2");
    else
        snprintf(loop_text, sizeof(loop_text), "%.0f",
                 state->loop_size > 0.0f ? state->loop_size : 4.0f);
    snprintf(header_title, sizeof(header_title), "%.38s", title);

    d2_fill_rect(pixels, 0, 0, WIDTH, HEIGHT, 0, 0, 0);
    d2_fill_rect(pixels, 0, 18, WIDTH, 1, accent_r, accent_g, accent_b);
    d2_fill_rect(pixels, wave_left, wave_top, wave_width,
                 D2_WAVEFORM_HEIGHT, 0, 0, 0);

    /* The existing 2x4 header footprint is retained exactly, but now carries
     * real master/active beat phase instead of decorative status boxes. */
    d2_draw_phase_meter(pixels, state, player);

    /* A 426-pixel viewport is copied out of the pre-rasterized strip.  The
     * common case is 94 small memcpy calls, rather than RGB conversion and
     * amplitude expansion for every waveform pixel on every frame. */
    float strip_center = position * (D2_WAVEFORM_POINTS - 1);
    int strip_start = (int)(strip_center - wave_width * source_step * 0.5f);
    if (d2_wave_strip_ready[player]) {
        if (source_step == 1.0f) {
            int copy_start = strip_start < 0 ? 0 : strip_start;
            int copy_end = strip_start + wave_width;
            if (copy_end > D2_WAVEFORM_POINTS) copy_end = D2_WAVEFORM_POINTS;
            if (copy_end > copy_start) {
                int destination_x = wave_left + copy_start - strip_start;
                size_t copy_bytes = (size_t)(copy_end - copy_start) * 2;
                for (int y = 0; y < D2_WAVEFORM_HEIGHT; ++y)
                    memcpy(&pixels[((wave_top + y) * WIDTH + destination_x) * 2],
                           &d2_wave_strip[player][y][copy_start * 2], copy_bytes);
            }
        } else {
            for (int y = 0; y < D2_WAVEFORM_HEIGHT; ++y) {
                uint8_t *destination = &pixels[((wave_top + y) * WIDTH + wave_left) * 2];
                for (int local_x = 0; local_x < wave_width; ++local_x) {
                    int source_x = (int)(strip_center +
                        (local_x - wave_width * 0.5f) * source_step);
                    if (source_x >= 0 && source_x < D2_WAVEFORM_POINTS)
                        memcpy(&destination[local_x * 2],
                               &d2_wave_strip[player][y][source_x * 2], 2);
                }
            }
        }
    } else if (d2_waveform_ready[player]) {
        for (int local_x = 0; local_x < wave_width; ++local_x) {
            float source = strip_center +
                           (local_x - wave_width / 2) * source_step;
            if (source < 0.0f) source = 0.0f;
            if (source > D2_WAVEFORM_POINTS - 1) source = D2_WAVEFORM_POINTS - 1;
            int index = (int)source;
            int amplitude = d2_waveform[player][index];
            d2_fill_rect(pixels, wave_left + local_x, wave_center - amplitude,
                         1, amplitude * 2 + 1, 40, 170, 230);
        }
    }

    /* Mixxx supplies the exact fraction elapsed since the previous beat.
     * Project beat markers from this real phase anchor, so the lines move
     * with analysed beats instead of acting as a decorative fixed ruler. */
    if (state->beatmap_ready && state->beatmap_count > 1) {
        float visible_half = (wave_width * 0.5f * source_step) /
                             (D2_WAVEFORM_POINTS - 1);
        float minimum = position - visible_half;
        float maximum = position + visible_half;
        int low = 0;
        int high = state->beatmap_count;
        while (low < high) {
            int middle = low + (high - low) / 2;
            if (state->beatmap_position[middle] < minimum)
                low = middle + 1;
            else
                high = middle;
        }
        for (int beat = low; beat < state->beatmap_count; ++beat) {
            float beat_position = state->beatmap_position[beat];
            if (beat_position > maximum)
                break;
            int x = d2_beat_position_to_x(
                    beat_position, position, source_step);
            if (x < wave_left || x >= wave_left + wave_width)
                continue;
            /* Preserve the original protobuf index.  BeatMap itself carries
             * no time-signature/downbeat field; index zero is therefore only
             * the configured 4/4 visual anchor, never an inferred result of
             * filtering or clipping the source list. */
            int source_index = state->beatmap_source_index[beat];
            int is_bar = ((source_index % 4) + 4) % 4 == 0;
            int tick_height = is_bar ? 13 : 7;
            int red = is_bar ? 210 : 205;
            int green = is_bar ? 34 : 210;
            int blue = is_bar ? 25 : 214;
            d2_fill_rect(pixels, x, wave_top, is_bar ? 2 : 1, tick_height,
                         red, green, blue);
            d2_fill_rect(pixels, x, wave_top + D2_WAVEFORM_HEIGHT - tick_height,
                         is_bar ? 2 : 1, tick_height, red, green, blue);
            d2_fill_rect(pixels, x, 54, is_bar ? 2 : 1, is_bar ? 10 : 6,
                         red, green, blue);
        }
    } else if (state->beatgrid_ready) {
        int current_beat = (int)floorf(
            (position - state->beatgrid_first_position) / state->beatgrid_interval);
        for (int beat = current_beat - 12; beat <= current_beat + 12; ++beat) {
            float beat_position = state->beatgrid_first_position +
                                  beat * state->beatgrid_interval;
            int x = d2_beat_position_to_x(
                    beat_position, position, source_step);
            if (x < wave_left || x >= wave_left + wave_width)
                continue;
            int is_bar = ((beat % 4) + 4) % 4 == 0;
            int tick_height = is_bar ? 12 : 7;
            int red = is_bar ? 210 : 205;
            int green = is_bar ? 34 : 210;
            int blue = is_bar ? 25 : 214;
            d2_fill_rect(pixels, x, wave_top, is_bar ? 2 : 1, tick_height,
                         red, green, blue);
            d2_fill_rect(pixels, x, wave_top + D2_WAVEFORM_HEIGHT - tick_height,
                         is_bar ? 2 : 1, tick_height, red, green, blue);
            d2_fill_rect(pixels, x, 54, is_bar ? 2 : 1, is_bar ? 10 : 6,
                         red, green, blue);
        }
    } else if (state->beatgrid_valid && state->duration > 1.0f && state->bpm > 20.0f) {
        float pixels_per_second = (float)D2_WAVEFORM_POINTS / state->duration;
        float beat_width = pixels_per_second * 60.0f /
                           (state->bpm * source_step);
        if (beat_width >= 8.0f && beat_width <= 220.0f) {
            float previous_beat = 240.0f - state->beat_distance * beat_width;
            for (int marker = -8; marker <= 8; ++marker) {
                int x = (int)(previous_beat + marker * beat_width + 0.5f);
                if (x < wave_left || x >= wave_left + wave_width)
                    continue;
                /* A bar accent requires the absolute beat index. The current
                 * message carries phase only, so render every verified beat
                 * identically instead of inventing an incorrect downbeat. */
                d2_fill_rect(pixels, x, wave_top, 1, 7, 205, 210, 214);
                d2_fill_rect(pixels, x, wave_top + D2_WAVEFORM_HEIGHT - 7,
                             1, 7, 205, 210, 214);
                d2_fill_rect(pixels, x, 54, 1, 6, 205, 210, 214);
            }
        }
    }

    d2_fill_rect(pixels, wave_left, wave_center, wave_width, 1, 45, 90, 112);
    d2_fill_rect(pixels, 240, 52, 1,
                 wave_top + D2_WAVEFORM_HEIGHT - 52, 255, 255, 255);
    d2_fill_rect(pixels, 237, 52, 7, 2, 255, 255, 255);

    /* Capacitive FX overlay. It is composited last over the waveform so touch
     * feedback is immediate and disappears without disturbing deck state. */
    int fx_touch_mask = state->fx_touch_mask;
    if (fx_touch_mask && fx_touch_mask != 0x0f &&
        d2_fx_touch_updated_us[player] != 0 &&
        d2_monotonic_us() - d2_fx_touch_updated_us[player] > 1500000ULL) {
        fx_touch_mask = 0;
        state->fx_touch_mask = 0;
        d2_fx_touch_updated_us[player] = 0;
    }
    if (fx_touch_mask) {
        char fx_labels[4][96];
        snprintf(fx_labels[0], sizeof(fx_labels[0]), "MIX");
        for (int effect = 0; effect < 3; ++effect)
            d2_fx_slot_label(fx_labels[effect + 1],
                             sizeof(fx_labels[effect + 1]), state, effect, 102);
        int fx_unit = d2_fx_unit_for_player(player);
        int unit_r = fx_unit == 2 ? 255 : 20;
        int unit_g = fx_unit == 2 ? 145 : 145;
        int unit_b = fx_unit == 2 ? 24 : 255;
        d2_fill_rect(pixels, 7, 74, 466, 78, 8, 11, 15);
        for (int slot = 0; slot < 4; ++slot) {
            int x = 13 + slot * 115;
            int active = d2_fx_overlay_active(
                player, state, slot, fx_touch_mask);
            int bar = (int)(104.0f * state->fx_parameter[slot]);
            d2_fill_rect(pixels, x, 99, 104, 9, 35, 39, 44);
            d2_fill_rect(pixels, x, 99, bar, 9,
                         active ? accent_r : 86,
                         active ? accent_g : 92,
                         active ? accent_b : 100);
            d2_draw_text(pixels, fx_labels[slot], x, 78, 1,
                         active ? 250 : 165, active ? 250 : 170,
                         active ? 250 : 178);
        }
        /* The physical FX SELECT lamp is brightness-only in the D2 HID
         * report, so RGB colors cannot distinguish the selected unit. Keep
         * the approved player geometry unchanged and expose the selection
         * only inside this transient capacitive-touch overlay. */
        d2_fill_rect(pixels, 191, 116, 98, 23, 19, 23, 28);
        d2_fill_rect(pixels, 191, 116, 98, 2, unit_r, unit_g, unit_b);
        d2_draw_text_centered(pixels, d2_fx_unit_label(player), 240, 120, 1,
                              unit_r, unit_g, unit_b);
    }

    d2_fill_rect(pixels, 0, 195, WIDTH, 43, 0, 0, 0);
    d2_fill_rect(pixels, 0, 194, WIDTH, 1, 72, 78, 84);
    if (d2_cover_art_ready[player]) {
        for (int row = 0; row < 28; ++row) {
            int source_y = row * 48 / 28;
            for (int column = 0; column < 28; ++column) {
                int source_x = column * 48 / 28;
                memcpy(&pixels[((200 + row) * WIDTH + 3 + column) * 2],
                       &d2_cover_art[player][(source_y * 48 + source_x) * 2], 2);
            }
        }
    } else {
        d2_fill_rect(pixels, 3, 200, 28, 28, 28, 31, 34);
        d2_fill_rect(pixels, 5, 202, 24, 24,
                      accent_r / 5, accent_g / 5, accent_b / 5);
        d2_draw_text(pixels, player == 1 ? "A" : "B", 12, 204, 2,
                      accent_r, accent_g, accent_b);
    }
    d2_fill_rect(pixels, 5, 243, 470, 25, 7, 14, 20);
    if (d2_waveform_ready[player]) {
        for (int x = 0; x < 470; ++x) {
            int index = x * D2_WAVEFORM_POINTS / 470;
            int height = d2_waveform[player][index] / 4;
            int r = 45, g = 175, b = 238;
            if (d2_waveform_ready[player] == 2) {
                r = 15 + d2_waveform_low[player][index] * 220 / 255;
                g = 35 + d2_waveform_mid[player][index] * 180 / 255;
                b = 70 + d2_waveform_high[player][index] * 185 / 255;
            }
            if (height > 10) height = 10;
            d2_fill_rect(pixels, 5 + x, 255 - height, 1,
                         height * 2 + 1, r, g, b);
        }
        int overview = 5 + (int)(470.0f * position);
        d2_fill_rect(pixels, overview - 1, 240, 3, 31, 250, 250, 250);

        for (int cue = 0; cue < 8; ++cue) {
            float cue_position = state->hotcue_position[cue];
            if (cue_position < 0.0f || cue_position > 1.0f)
                continue;
            uint32_t color = state->hotcue_color[cue];
            int cue_r = color ? (int)((color >> 16) & 0xff) : 0;
            int cue_g = color ? (int)((color >> 8) & 0xff) : 170;
            int cue_b = color ? (int)(color & 0xff) : 235;
            int cue_x = 5 + (int)(cue_position * 470.0f);
            d2_fill_rect(pixels, cue_x - 4, 236, 9, 8,
                         cue_r, cue_g, cue_b);
            char cue_label[2] = {(char)('1' + cue), '\0'};
            d2_draw_text(pixels, cue_label, cue_x - 2, 235, 1, 0, 0, 0);
        }
    }

    d2_draw_text(pixels, header_title, 3, 0, 2, 242, 245, 247);
    d2_draw_text(pixels, bpm, 328, 0, 2, 235, 240, 244);
    d2_draw_text(pixels, d2_visual_key_text(state), 413, 0, 2,
                 235, 240, 244);
    d2_draw_text(pixels, player == 1 ? "A" : "B", 466, 0, 2,
                 accent_r, accent_g, accent_b);

    d2_draw_text(pixels, "KEY", 57, 198, 1, 220, 225, 230);
    d2_draw_text(pixels, "LOCK", 76, 198, 1,
                 state->keylock ? 245 : 92,
                 state->keylock ? 55 : 96,
                 state->keylock ? 55 : 102);
    d2_draw_text(pixels, d2_visual_key_text(state), 57, 210, 3,
                  250, 205, 60);

    d2_draw_text(pixels, "LOOP", 129, 198, 1, 220, 225, 230);
    d2_fill_rect(pixels, 129, 211, 35, 18,
                 d2_led_state[player].loop ? 0 : 120,
                 d2_led_state[player].loop ? 220 : 125,
                 d2_led_state[player].loop ? 120 : 130);
    d2_fill_rect(pixels, 131, 213, 31, 14, 0, 0, 0);
    d2_draw_text(pixels, loop_text,
                 strlen(loop_text) >= 3 ? 135 :
                 (strlen(loop_text) == 2 ? 139 : 143),
                 210, 2,
                 d2_led_state[player].loop ? 235 : 160,
                 d2_led_state[player].loop ? 250 : 165,
                 d2_led_state[player].loop ? 238 : 170);

    d2_draw_text(pixels, state->time_mode ? "ELAPSED" : "REMAIN",
                 188, 198, 1, 220, 225, 230);
    d2_draw_text(pixels, time_text, 188, 211, 2, 248, 250, 252);
    d2_draw_text(pixels, "QUANTIZE", 235, 198, 1,
                 state->quantize ? 245 : 100,
                 state->quantize ? 55 : 48,
                 state->quantize ? 55 : 52);

    d2_draw_text(pixels, "TEMPO", 316, 198, 1, 220, 225, 230);
    d2_draw_text(pixels, tempo, 316, 211, 2, 248, 250, 252);
    if (d2_led_state[player].sync) {
        d2_fill_rect(pixels, 356, 198, 34, 11, 238, 240, 242);
        d2_draw_text(pixels, "SYNC", 359, 197, 1, 5, 7, 9);
    }

    d2_draw_text(pixels, "BPM", 409, 198, 1, 220, 225, 230);
    d2_draw_text(pixels, bpm, 398, 207, 3, 255, 255, 255);
    char zoom_text[8];
    snprintf(zoom_text, sizeof(zoom_text), "%dX", zoom_level);
    d2_draw_text(pixels, zoom_text, 3, 52, 1, 180, 188, 196);
    if (state->beatgrid_edit)
        d2_draw_text(pixels, "GRID  OFFSET  TEMPO  JUMP  LOOP",
                     28, 52, 1, 245, 62, 48);
}

static void d2_capture_publish(int player, const uint8_t *pixels)
{
    if (player < 1 || player > 2 || !pixels)
        return;
    memcpy(d2_capture_frame[player], pixels, WIDTH * HEIGHT * 2);
    d2_capture_frame_ready[player] = 1;
}

static int d2_write_rgb565_ppm(const char *path, const uint8_t *pixels)
{
    FILE *output = fopen(path, "wb");
    if (!output)
        return -1;

    fprintf(output, "P6\n%d %d\n255\n", WIDTH, HEIGHT);
    for (int i = 0; i < WIDTH * HEIGHT; ++i) {
        uint16_t color = ((uint16_t)pixels[i * 2] << 8) |
                         pixels[i * 2 + 1];
        unsigned char rgb[3] = {
            (unsigned char)(((color >> 11) & 31) * 255 / 31),
            (unsigned char)(((color >> 5) & 63) * 255 / 63),
            (unsigned char)((color & 31) * 255 / 31),
        };
        if (fwrite(rgb, 1, sizeof(rgb), output) != sizeof(rgb)) {
            fclose(output);
            return -1;
        }
    }
    if (fclose(output) != 0)
        return -1;
    return 0;
}

static void d2_capture_write_requested_frames(void)
{
    if (!d2_capture_requested)
        return;
    d2_capture_requested = 0;

    for (int player = 1; player <= 2; ++player) {
        if (!d2_capture_frame_ready[player])
            continue;
        char path[64];
        snprintf(path, sizeof(path), "/tmp/d2-player-%d-live.ppm", player);
        if (d2_write_rgb565_ppm(path, d2_capture_frame[player]) == 0)
            fprintf(stdout, "D2 CAPTURE PLAYER %d %s\n", player, path);
        else
            fprintf(stderr, "D2 CAPTURE FAILED PLAYER %d %s\n",
                    player, path);
    }
    fflush(stdout);
    fflush(stderr);
}

/* Render producer: always overwrite the non-published RGB565 buffer.  USB
 * never sees a partially drawn frame because publication is a single index
 * swap under d2_frame_mutex after the complete 480x272 image is ready. */
static void *d2_render_thread_main(void *userdata)
{
    (void)userdata;
    struct timespec deadline;
    d2_font_init();
    clock_gettime(CLOCK_MONOTONIC, &deadline);

    while (running) {
        deadline.tv_nsec += 16666667L;
        if (deadline.tv_nsec >= 1000000000L) {
            deadline.tv_sec += deadline.tv_nsec / 1000000000L;
            deadline.tv_nsec %= 1000000000L;
        }

        for (int player = 1; player <= 2; ++player) {
            pthread_mutex_lock(&d2_state_mutex);
            if (d2_screen_view[player] == D2_VIEW_DECK) {
                pthread_mutex_lock(&d2_frame_mutex);
                unsigned back = d2_render_buffer_index[player] ^ 1u;
                pthread_mutex_unlock(&d2_frame_mutex);

                struct d2_screen_state *state = &d2_screen_state[player];
                float displayed_position = d2_effective_position(state);
                char deck_title[80] = {0};
                d2_visible_deck_title(deck_title, sizeof(deck_title),
                                      state->title);

                int accent_r = player == 1 ? 220 : 40;
                int accent_g = player == 1 ? 40 : 120;
                int accent_b = player == 1 ? 40 : 230;
                d2_render_deck_fast(d2_render_buffer[player][back], player,
                                    state, displayed_position, deck_title,
                                    accent_r, accent_g, accent_b);

                pthread_mutex_lock(&d2_frame_mutex);
                d2_render_buffer_index[player] = back;
                d2_render_buffer_ready[player] = 1;
                ++d2_render_generation[player];
                pthread_mutex_unlock(&d2_frame_mutex);
            }
            pthread_mutex_unlock(&d2_state_mutex);
        }

        int sleep_result;
        do {
            sleep_result = clock_nanosleep(CLOCK_MONOTONIC, TIMER_ABSTIME,
                                           &deadline, NULL);
        } while (sleep_result == EINTR && running);
    }
    d2_font_shutdown();
    return NULL;
}

static int d2_player_from_dev(struct ctlra_dev_t *dev);

static int32_t screen_callback(
    struct ctlra_dev_t *dev,
    uint32_t screen_idx,
    uint8_t *pixel_data,
    uint32_t bytes,
    struct ctlra_screen_zone_t *zone,
    void *userdata)
{
    (void)screen_idx;
    (void)zone;
    (void)userdata;

    if (bytes < WIDTH * HEIGHT * 2)
        return 0;

    int player = d2_player_from_dev(dev);

    static int screen_trace[3] = {0, 0, 0};

    if (player >= 1 && player <= 2) {
        screen_trace[player]++;

        if (D2_VERBOSE && (screen_trace[player] == 1 ||
            screen_trace[player] % 100 == 0)) {
            printf("SCREEN TRACE PLAYER %d #%d bytes=%u\\n",
                   player,
                   screen_trace[player],
                   bytes);
            fflush(stdout);
        }
    }
    if (player < 1 || player > 2)
        return 0;

    /*
     * ------------------------------------------------------------
     * STATIC D2 DECK UI
     *
     * Player 1 = left deck / red accent
     * Player 2 = right deck / blue accent
     *
     * No Mixxx data yet.
     * This stage verifies the complete UI geometry.
     * ------------------------------------------------------------
     */

    int accent_r = (player == 1) ? 220 : 40;
    int accent_g = (player == 1) ? 40  : 120;
    int accent_b = (player == 1) ? 40  : 230;
    struct d2_screen_state *state = &d2_screen_state[player];
    enum d2_screen_view view = d2_screen_view[player];

    /* The deck is the only continuously animated view.  Use the direct
     * compositor above rather than the old per-pixel UI predicate renderer. */
    if (view == D2_VIEW_DECK) {
        int flush = 0;
        pthread_mutex_lock(&d2_frame_mutex);
        if (d2_render_buffer_ready[player] &&
            d2_usb_generation[player] != d2_render_generation[player]) {
            unsigned front = d2_render_buffer_index[player];
            memcpy(pixel_data, d2_render_buffer[player][front],
                   WIDTH * HEIGHT * 2);
            d2_capture_publish(player, pixel_data);
            d2_usb_generation[player] = d2_render_generation[player];
            d2_last_screen_view[player] = D2_VIEW_DECK;
            flush = 1;
        }
        pthread_mutex_unlock(&d2_frame_mutex);
        /* A zero result means no newly completed full frame is available.
         * It never interrupts an already-submitted D2 transfer. */
        return flush;
    }

    if (view == D2_VIEW_BROWSE) {
        /* Browse is row-based and static between encoder/model updates. Do not
         * keep re-rasterizing and bulk-transferring the same 480x272 frame at
         * 60 Hz while Mixxx is enumerating a USB directory. */
        uint64_t browse_now_us = d2_monotonic_us();
        enum d2_browse_notice_kind browse_notice =
            d2_active_browse_notice(player, browse_now_us);
        if (d2_load_library_browse_state())
            d2_browse_mark_dirty();
        if (d2_browse_frame_is_current(player))
            return 0;
        unsigned back = d2_render_buffer_index[player] ^ 1u;
        uint8_t *back_buffer = d2_render_buffer[player][back];
        d2_render_browse_fast(back_buffer, player, state);
        if (browse_notice != D2_BROWSE_NOTICE_NONE)
            d2_draw_browse_notice(back_buffer, browse_notice);
        memcpy(pixel_data, back_buffer, WIDTH * HEIGHT * 2);
        d2_capture_publish(player, pixel_data);
        d2_render_buffer_index[player] = back;
        d2_browse_rendered_generation[player] = d2_browse_generation;
        d2_browse_frame_valid[player] = 1;
        d2_last_screen_view[player] = D2_VIEW_BROWSE;
        return 1;
    }

    if (d2_is_performance_view(view)) {
        /* Performance views are static except for their eight live pad
         * colors. Keeping HOTCUE and SAMPLER on this same fast compositor is
         * important: the legacy per-pixel glyph predicate loop could delay
        * physical input while either of those views was open. */
        char performance_title[80] = {0};
        uint32_t pad_rgb[8];
        if (!d2_try_performance_snapshot(
                player, performance_title, sizeof(performance_title),
                pad_rgb))
            return 0;
        struct d2_performance_visible_state visible;
        d2_make_performance_visible_state(
            &visible, view, performance_title, pad_rgb);
        if (d2_performance_frame_is_current(player, &visible))
            return 0;
        d2_render_dynamic_performance_view(pixel_data, view,
                                           performance_title,
                                           pad_rgb,
                                           accent_r, accent_g, accent_b);
        d2_capture_publish(player, pixel_data);
        d2_commit_performance_frame(player, &visible);
        return 1;
    }

    /* Every declared screen view returned through its dedicated compositor.
     * Reject an invalid enum instead of reviving the obsolete per-pixel UI. */
    return 0;
}


/*
 * Physical D2 -> Player mapping.
 * The D2 driver selects these by USB serial.
 */
static struct {
    struct ctlra_dev_t *dev;
    const char *serial;
    int player;
} d2_map[2];

static int d2_map_count = 0;

static int d2_player_from_dev(struct ctlra_dev_t *dev)
{
    for (int i = 0; i < d2_map_count; i++) {
        if (d2_map[i].dev == dev)
            return d2_map[i].player;
    }

    return 0;
}

#define D2_BROWSE_MAX_STEPS_PER_REPORT 16

/* Preserve the magnitude reported by libctlra so a fast turn or a wrapped
 * nibble advances the Mixxx selection by the same number of detents.  The
 * bound prevents a corrupt USB report from flooding the MIDI queue. */
static int d2_browse_delta_steps(int delta)
{
    if (delta > D2_BROWSE_MAX_STEPS_PER_REPORT)
        return D2_BROWSE_MAX_STEPS_PER_REPORT;
    if (delta < -D2_BROWSE_MAX_STEPS_PER_REPORT)
        return -D2_BROWSE_MAX_STEPS_PER_REPORT;
    return delta;
}

static void event_callback_locked(
    struct ctlra_dev_t *dev,
    uint32_t num_events,
    struct ctlra_event_t **events,
    void *userdata)
{
    (void)userdata;
    /* Pressing the Browse encoder also trips its capacitive Touch sensor.
     * Ignore that trailing touch briefly so Load -> Deck cannot be raced
     * back into Browse by the same physical gesture. */
    static uint64_t browse_touch_block_until_us[3] = {0, 0, 0};
    /* Focus can change as soon as Note 61 opens a sidebar item.  Latch the
     * chosen note at press time so its release always closes the same MIDI
     * control instead of accidentally releasing Note 62. */
    static uint8_t browse_press_note[3] = {0, 0, 0};

    D2_EVENT_LOG("EVENT FROM PLAYER %d DEV=%p\\n",
                 d2_player_from_dev(dev), (void *)dev);
    D2_EVENT_FLUSH();

    for (uint32_t i = 0; i < num_events; i++) {

        struct ctlra_event_t *e = events[i];

        if (e->type == CTLRA_EVENT_BUTTON) {

            int player = d2_player_from_dev(dev);
            if (player < 1 || player > 2)
                continue;
            if (e->button.id < 58) {
                uint8_t pressed = e->button.pressed ? 1 : 0;
                if (d2_button_state[player][e->button.id] == pressed)
                    continue;
                d2_button_state[player][e->button.id] = pressed;
            }

            D2_EVENT_LOG(
                "BUTTON id=%u %s\n",
                e->button.id,
                e->button.pressed ? "DOWN" : "UP");

            int midi_channel = player - 1;

            /*
             * Browse Encoder:
             *
             * ctlra ID 25 = Browse Encoder Press
             * ctlra ID 26 = Browse Encoder Touch
             *
             * Touch must NOT generate MIDI.
             * Press is Note 61 while the Library tree has focus (open folder)
             * and Note 62 while the track table has focus (native load).
             */
            if (e->button.id == 26) {

                if (player >= 1 && player <= 2 && e->button.pressed) {
                    struct timespec touch_ts;
                    clock_gettime(CLOCK_MONOTONIC, &touch_ts);
                    uint64_t now_us =
                        (uint64_t)touch_ts.tv_sec * 1000000ULL +
                        (uint64_t)touch_ts.tv_nsec / 1000ULL;
                    if (now_us < browse_touch_block_until_us[player]) {
                        D2_EVENT_LOG(
                            "BROWSE TOUCH PLAYER %d suppressed after Load\n",
                            player);
                        D2_EVENT_FLUSH();
                        continue;
                    }
                    d2_clear_browse_notice(player);
                    /* The Pioneered tab control toggles. The library is
                     * global, so switching from one D2 to the other must
                     * keep it open instead of sending a second toggle. */
                    if (d2_screen_view[1] != D2_VIEW_BROWSE &&
                        d2_screen_view[2] != D2_VIEW_BROWSE)
                        midi_note(midi_channel, 100, 127);
                    d2_screen_view[player] = D2_VIEW_BROWSE;
                    /* The Mixxx library selection is global; keep it on the
                     * D2 currently being browsed and leave the other display
                     * on its own player screen. */
                    d2_screen_view[player == 1 ? 2 : 1] = D2_VIEW_DECK;
                    d2_browse_mark_dirty();
                }

                D2_EVENT_LOG(
                    "BROWSE TOUCH PLAYER %d -> MAIN BROWSE MIDI\\n",
                    player);

                D2_EVENT_FLUSH();
                continue;

            } else if (e->button.id == 25) {

                int browse_note;

                if (player >= 1 && player <= 2 && e->button.pressed) {
                    d2_clear_browse_notice(player);
                    struct timespec press_ts;
                    clock_gettime(CLOCK_MONOTONIC, &press_ts);
                    browse_touch_block_until_us[player] =
                        (uint64_t)press_ts.tv_sec * 1000000ULL +
                        (uint64_t)press_ts.tv_nsec / 1000ULL + 750000ULL;
                    /* In Sidebar focus the press activates/opens the selected
                     * folder and Browse must stay visible. Only a press in the
                     * track list loads and returns to the player. */
                    if (d2_screen_state[player].browse_focus) {
                        d2_screen_view[player] = D2_VIEW_BROWSE;
                        d2_browse_mark_dirty();
                    } else {
                        /* Do not assume the library row can be loaded. Offline
                         * playlist entries are valid database records but have
                         * no readable audio file. The JS track_loaded callback
                         * switches to DECK only after Mixxx confirms success. */
                        d2_screen_view[player] = D2_VIEW_BROWSE;
                        d2_browse_mark_dirty();
                    }
                    browse_note =
                        d2_screen_state[player].browse_focus ? 61 : 62;
                    browse_press_note[player] = (uint8_t)browse_note;
                } else {
                    browse_note = browse_press_note[player] ?
                        browse_press_note[player] :
                        (d2_screen_state[player].browse_focus ? 61 : 62);
                    browse_press_note[player] = 0;
                }

                midi_note(midi_channel, browse_note,
                          e->button.pressed ? 127 : 0);

                D2_EVENT_LOG(
                    "BROWSE PRESS PLAYER %d CH %d NOTE %d %s\\n",
                    player,
                    midi_channel + 1,
                    browse_note,
                    e->button.pressed ? "ON" : "OFF");

            } else if (e->button.id == 27) {

                /* Back changes Library focus between the tree and track list.
                 * It deliberately keeps both the D2 and touchscreen in Browse. */
                if (e->button.pressed)
                    d2_clear_browse_notice(player);
                midi_note(midi_channel, 63,
                          e->button.pressed ? 127 : 0);
                D2_EVENT_LOG("BROWSE BACK PLAYER %d -> LIBRARY FOCUS %s\\n",
                             player, e->button.pressed ? "ON" : "OFF");

            } else if (e->button.id == 56) {

                /* CUE needs an unambiguous press/release pair for momentary
                 * preview. Use a dedicated CC rather than MIDI note-off
                 * semantics, which vary between PortMidi backends. */
                midi_cc(midi_channel, 0x70,
                        e->button.pressed ? 127 : 0);
                D2_EVENT_LOG("CUE PLAYER %d CH %d CC 112 %s\\n", player,
                             midi_channel + 1,
                             e->button.pressed ? "ON" : "OFF");

            } else if (e->button.id == 57) {

                /* PLAY is a press-only toggle in JS but is also carried as a
                 * CC so its release can never collide with CUE mapping. */
                midi_cc(midi_channel, 0x71,
                        e->button.pressed ? 127 : 0);
                D2_EVENT_LOG("PLAY PLAYER %d CH %d CC 113 %s\\n", player,
                             midi_channel + 1,
                             e->button.pressed ? "ON" : "OFF");

            } else {

                /*
                 * All other D2 buttons keep their existing mapping:
                 * ctlra ID 0..57 -> MIDI Note 36..93
                 */
                midi_note(
                    midi_channel,
                    36 + e->button.id,
                    e->button.pressed ? 127 : 0);

                D2_EVENT_LOG(
                    "MIDI PLAYER %d CH %d NOTE %d %s\\n",
                    player,
                    midi_channel + 1,
                    36 + e->button.id,
                    e->button.pressed ? "ON" : "OFF");
            }

            D2_EVENT_FLUSH();
        }

        else if (e->type == CTLRA_EVENT_ENCODER) {

            int player = d2_player_from_dev(dev);
            int midi_channel = player - 1;
            int delta = 0;

            if (e->encoder.flags & CTLRA_EVENT_ENCODER_FLAG_INT) {

                delta = e->encoder.delta;

                struct timespec raw_ts;
                clock_gettime(CLOCK_MONOTONIC, &raw_ts);

                uint64_t raw_ms =
                    ((uint64_t)raw_ts.tv_sec * 1000ULL) +
                    ((uint64_t)raw_ts.tv_nsec / 1000000ULL);

                D2_EVENT_LOG(
                    "RAW ENCODER t=%llu PLAYER %d id=%u flags=0x%x delta=%d delta_float=%f\\n",
                    (unsigned long long)raw_ms,
                    player,
                    e->encoder.id,
                    e->encoder.flags,
                    e->encoder.delta,
                    e->encoder.delta_float);

            } else {

                if (e->encoder.delta_float > 0.0f)
                    delta = 1;
                else if (e->encoder.delta_float < 0.0f)
                    delta = -1;
            }

            if (delta != 0) {

                /* The driver already emits one event for each changed Browse
                 * nibble.  A time filter here drops legitimate rapid detents,
                 * so no event is suppressed or coalesced in the bridge. */
                if (e->encoder.id == 4) {
                    d2_clear_browse_notice(player);
                }

                /*
                 * Relative MIDI CC:
                 * 65 = one step clockwise
                 * 63 = one step counter-clockwise
                 */
                int value = (delta > 0) ? 65 : 63;

                /*
                 * Encoder ID 0..7 -> MIDI CC 16..23
                 */
                int cc = 16 + e->encoder.id;

                int step_delta = e->encoder.id == 4 ?
                    d2_browse_delta_steps(delta) : (delta > 0 ? 1 : -1);
                int step_count = step_delta > 0 ? step_delta : -step_delta;
                for (int step = 0; step < step_count; ++step)
                    midi_cc(midi_channel, cc, value);

                D2_EVENT_LOG(
                    "MIDI PLAYER %d CH %d ENCODER id=%u CC=%d VALUE=%d delta=%d steps=%d\n",
                    player,
                    midi_channel + 1,
                    e->encoder.id,
                    cc,
                    value,
                    delta,
                    step_count);
            }

            D2_EVENT_FLUSH();
        }

        else if (e->type == CTLRA_EVENT_SLIDER) {

            int player = d2_player_from_dev(dev);
            int midi_channel = player - 1;

            /*
             * CTLRA slider is approximately 0.0 .. 1.0.
             * Clamp because hardware can report slightly above 1.0.
             */
            float v = e->slider.value;

            if (v < 0.0f)
                v = 0.0f;

            if (v > 1.0f)
                v = 1.0f;

            int value = (int)(v * 127.0f + 0.5f);

            /*
             * Slider ID 0..7 -> MIDI CC 32..39
             */
            int cc = 32 + e->slider.id;

            midi_cc(
                midi_channel,
                cc,
                value);

            D2_EVENT_LOG(
                "MIDI PLAYER %d CH %d SLIDER id=%u CC=%d VALUE=%d raw=%f\n",
                player,
                midi_channel + 1,
                e->slider.id,
                cc,
                value,
                e->slider.value);

            D2_EVENT_FLUSH();
        }
        }
    }

/*
 * libctlra input polling must never wait for a complete framebuffer render.
 * ctlra_idle_iter() invokes this callback synchronously, so lock only while
 * applying the delivered hardware event to shared deck/controller state.
 */
static void event_callback(
    struct ctlra_dev_t *dev,
    uint32_t num_events,
    struct ctlra_event_t **events,
    void *userdata)
{
    pthread_mutex_lock(&d2_state_mutex);
    event_callback_locked(dev, num_events, events, userdata);
    pthread_mutex_unlock(&d2_state_mutex);
}

static void feedback_callback(
    struct ctlra_dev_t *dev,
    void *userdata)
{
    (void)userdata;
    int player = d2_player_from_dev(dev);
    if (player < 1 || player > 2)
        return;

    const struct d2_led_state *led = &d2_led_state[player];
    const struct d2_screen_state *screen = &d2_screen_state[player];
    const uint32_t off = 0x00000000U;
    const uint32_t dim = 0x08000000U;
    const uint32_t bright = 0x7f000000U;
    const uint32_t blue_active = 0x7f0000ffU;
    const uint32_t amber_active = 0x7fff7800U;

    for (uint32_t i = 0; i < NI_KONTROL_D2_LED_COUNT; i++)
        ctlra_dev_light_set(dev, i, off);

    if (!led->outputs_enabled) {
        uint8_t dark[25] = {0};
        ni_kontrol_d2_light_touchstrip(dev, dark, dark);
        ctlra_dev_light_flush(dev, 0);
        return;
    }

    for (int pad = 0; pad < 8; ++pad) {
        uint32_t rgb = led->pad_rgb[pad] & 0x00ffffffU;
        ctlra_dev_light_set(dev, NI_KONTROL_D2_LED_PAD_1 + pad,
                            rgb ? (0x7f000000U | rgb) : off);
    }

    ctlra_dev_light_set(dev, NI_KONTROL_D2_LED_FX_SELECT,
                        led->fx_unit == 2 ? amber_active : blue_active);
    for (int fx = 0; fx < 4; ++fx)
        ctlra_dev_light_set(dev, NI_KONTROL_D2_LED_FX_1 + fx,
                            led->fx_enabled[fx] ? bright : dim);

    if (d2_screen_view[player] == D2_VIEW_BROWSE) {
        ctlra_dev_light_set(dev, NI_KONTROL_D2_LED_SCREEN_LEFT_1,
                            screen->browse_sort_column == 2 ? bright : dim);
        ctlra_dev_light_set(dev, NI_KONTROL_D2_LED_SCREEN_LEFT_2,
                            screen->browse_sort_column == 15 ? bright : dim);
        ctlra_dev_light_set(dev, NI_KONTROL_D2_LED_SCREEN_LEFT_3,
                            screen->browse_sort_column == 20 ? bright : dim);
        ctlra_dev_light_set(dev, NI_KONTROL_D2_LED_SCREEN_LEFT_4,
                            screen->browse_sort_order ? bright : dim);
    } else {
        ctlra_dev_light_set(dev, NI_KONTROL_D2_LED_SCREEN_LEFT_1, bright);
        ctlra_dev_light_set(dev, NI_KONTROL_D2_LED_SCREEN_LEFT_2,
                            screen->keylock ? bright : dim);
        ctlra_dev_light_set(dev, NI_KONTROL_D2_LED_SCREEN_LEFT_3, dim);
        ctlra_dev_light_set(dev, NI_KONTROL_D2_LED_SCREEN_LEFT_4,
                            screen->time_mode ? bright : dim);
    }
    ctlra_dev_light_set(dev, NI_KONTROL_D2_LED_SCREEN_RIGHT_1,
                        led->mode == 1 ? bright : dim);
    ctlra_dev_light_set(dev, NI_KONTROL_D2_LED_SCREEN_RIGHT_2,
                        led->mode == 2 ? bright : dim);
    ctlra_dev_light_set(dev, NI_KONTROL_D2_LED_SCREEN_RIGHT_3,
                        led->mode == 5 ? bright : dim);
    ctlra_dev_light_set(dev, NI_KONTROL_D2_LED_SCREEN_RIGHT_4,
                        screen->quantize ? bright : dim);

    ctlra_dev_light_set(dev, NI_KONTROL_D2_LED_BACK,
                        d2_screen_view[player] == D2_VIEW_BROWSE ? bright : dim);
    ctlra_dev_light_set(dev, NI_KONTROL_D2_LED_CAPTURE, dim);
    ctlra_dev_light_set(dev, NI_KONTROL_D2_LED_EDIT,
                        screen->beatgrid_edit ? bright : dim);
    for (int strip = 0; strip < 4; ++strip)
        ctlra_dev_light_set(dev, NI_KONTROL_D2_LED_ON_1 + strip,
                            led->performance_on[strip] ? bright : dim);

    ctlra_dev_light_set(dev, NI_KONTROL_D2_LED_HOTCUE,
                        led->mode == 1 ? blue_active : dim);
    ctlra_dev_light_set(dev, NI_KONTROL_D2_LED_LOOP,
                        led->mode == 2 ? blue_active : dim);
    ctlra_dev_light_set(dev, NI_KONTROL_D2_LED_FREEZE,
                        led->mode == 3 ? blue_active : dim);
    ctlra_dev_light_set(dev, NI_KONTROL_D2_LED_REMIX,
                        led->mode == 4 ? blue_active : dim);
    ctlra_dev_light_set(dev, NI_KONTROL_D2_LED_FLUX,
                        led->flux ? bright : dim);
    ctlra_dev_light_set(dev, NI_KONTROL_D2_LED_DECK, blue_active);
    ctlra_dev_light_set(dev, NI_KONTROL_D2_LED_SHIFT,
                        led->shift ? bright : dim);
    ctlra_dev_light_set(dev, NI_KONTROL_D2_LED_SYNC,
                        led->sync ? 0x7f00ff00U : 0x08001000U);
    ctlra_dev_light_set(dev, NI_KONTROL_D2_LED_CUE,
                        led->cue ? bright : dim);
    ctlra_dev_light_set(dev, NI_KONTROL_D2_LED_PLAY,
                        led->play ? bright : dim);

    for (int channel = 1; channel <= 4; ++channel)
        ctlra_dev_light_set(dev, NI_KONTROL_D2_LED_DECK_A + channel - 1,
                            (led->fx_assign_mask & (1 << (channel - 1))) ?
                                bright : dim);
    for (int ring = 0; ring < 4; ++ring)
        ctlra_dev_light_set(dev, NI_KONTROL_D2_LED_LOOP_CIRCLE_1 + ring,
                            led->loop ? blue_active : dim);

    uint8_t orange[25] = {0};
    uint8_t blue[25] = {0};
    float position = d2_effective_position(screen);
    int marker = (int)lrintf(position * 24.0f);
    if (marker < 0) marker = 0;
    if (marker > 24) marker = 24;
    for (int i = 0; i <= marker; ++i)
        blue[i] = led->play ? 80 : 32;
    if (led->flux)
        orange[marker] = 127;
    ni_kontrol_d2_light_touchstrip(dev, orange, blue);
    /* The driver compares the finished LED report with the last submitted
     * one. Do not force an identical interrupt OUT write every 10 ms. */
    ctlra_dev_light_flush(dev, 0);
}

static int accept_device(
    struct ctlra_t *ctlra,
    const struct ctlra_dev_info_t *info,
    struct ctlra_dev_t *dev,
    void *userdata)
{
    (void)ctlra;
    (void)userdata;

    printf(
        "Found: %s %s (%04x:%04x)\n",
        info->vendor,
        info->device,
        info->vendor_id,
        info->device_id);

    if (info->vendor_id != 0x17cc ||
        info->device_id != 0x1400)
        return 0;

    printf("D2 ACCEPTED\n");

    /* Physical D2 -> Player mapping */
    if (d2_map_count < 2) {
        d2_map[d2_map_count].dev = dev;
        d2_map[d2_map_count].player = 2 - d2_map_count;
        int player = d2_map[d2_map_count].player;

        /* A reconnected D2 has no physical copy of the process-local cached
         * frame. Force the first Browser/Performance callback to submit a
         * complete image instead of trusting stale cache metadata. */
        d2_last_screen_view[player] = -1;
        d2_performance_frame_valid[player] = 0;
        d2_browse_frame_valid[player] = 0;

        printf("D2 PLAYER MAP: dev=%p -> Player %d\n",
               (void *)dev,
               player);
        fflush(stdout);

        d2_map_count++;
    }

    ctlra_dev_set_event_func(dev, event_callback);
    ctlra_dev_set_feedback_func(dev, feedback_callback);
    ctlra_dev_set_screen_feedback_func(dev, screen_callback);

    return 1;
}


static int d2_render_test_image(const char *path, int use_beatmap)
{
    uint8_t *pixels = calloc((size_t)WIDTH * HEIGHT * 2, 1);
    struct d2_track_assets *assets = calloc(1, sizeof(*assets));
    if (!pixels || !assets) {
        free(pixels);
        free(assets);
        return 1;
    }
    d2_track_assets[1] = assets;
    d2_bind_asset_slot(1, assets);
    for (int i = 0; i < D2_WAVEFORM_POINTS; ++i) {
        float phase = (float)i * 0.015f;
        float pulse = powf(fmaxf(0.0f, sinf(phase)), 3.0f);
        d2_waveform_low[1][i] = (uint8_t)(35 + 205 * pulse);
        d2_waveform_mid[1][i] = (uint8_t)(30 + 150 * fabsf(sinf(phase * 2.1f)));
        d2_waveform_high[1][i] = (uint8_t)(22 + 110 * fabsf(sinf(phase * 5.7f)));
        d2_waveform[1][i] = (uint8_t)(8 + 40 * pulse);
    }
    d2_waveform_ready[1] = 2;
    d2_build_wave_strip(1);
    struct d2_screen_state state = {
        .bpm = 128.0f, .position = 0.42f, .remaining = 203.456f,
        .duration = 420.0f, .rate = 1.0241f, .beatgrid_valid = 1,
        .phase_master = 0.18f, .phase_active = 0.42f,
        .phase_master_step = 0, .phase_active_step = 1,
        .phase_master_deck = 1, .phase_follower_deck = 2, .phase_valid = 1,
        .visual_key = 20, .loop_size = 16.0f, .quantize = 1, .keylock = 1,
        .beatgrid_first_position = 0.001f,
        .beatgrid_interval = 60.0f / (128.0f * 420.0f),
        .beatgrid_ready = 1, .playing = 1,
        .title = "HELEMAAL NAAR DE KLOTE (ALVARO REMIX)",
        .artist = "NEXUS D2 PLAYER", .musical_key = "4A",
        .hotcue_position = {0.03f, 0.18f, 0.42f, 0.58f, 0.76f, -1, -1, -1},
    };
    if (use_beatmap) {
        static const float irregular[] = {
            0.4130f, 0.4162f, 0.4200f, 0.4231f, 0.4274f,
        };
        state.beatgrid_ready = 0;
        state.beatmap_ready = 1;
        state.beatmap_count =
            (int)(sizeof(irregular) / sizeof(irregular[0]));
        for (int i = 0; i < state.beatmap_count; ++i) {
            state.beatmap_position[i] = irregular[i];
            state.beatmap_source_index[i] = i;
        }
    }
    d2_led_state[1].loop = 1;
    d2_led_state[1].sync = 1;
    d2_render_deck_fast(pixels, 1, &state, state.position, state.title,
                        0, 150, 210);
    FILE *output = fopen(path, "wb");
    if (!output) {
        free(pixels);
        d2_track_assets[1] = NULL;
        d2_bind_asset_slot(1, NULL);
        free(assets);
        return 1;
    }
    fprintf(output, "P6\n%d %d\n255\n", WIDTH, HEIGHT);
    for (int i = 0; i < WIDTH * HEIGHT; ++i) {
        uint16_t color = ((uint16_t)pixels[i * 2] << 8) | pixels[i * 2 + 1];
        unsigned char rgb[3] = {
            (unsigned char)(((color >> 11) & 31) * 255 / 31),
            (unsigned char)(((color >> 5) & 63) * 255 / 63),
            (unsigned char)((color & 31) * 255 / 31),
        };
        fwrite(rgb, 1, sizeof(rgb), output);
    }
    fclose(output);
    free(pixels);
    d2_track_assets[1] = NULL;
    d2_bind_asset_slot(1, NULL);
    free(assets);
    return 0;
}

static int d2_render_browse_test_image(const char *path, int browse_focus,
                                       enum d2_browse_notice_kind notice)
{
    uint8_t *pixels = calloc((size_t)WIDTH * HEIGHT * 2, 1);
    if (!pixels)
        return 1;
    static const char *const titles[D2_BROWSE_ROWS] = {
        "HIGHLANDS (DAPAYK SOLO)", "OUHOU", "BRIGHT SEA", "DEEP DISH",
        "WAYS OF THE PEOPLE", "KNOW", "HIDDEN BEAUTIES",
        "MIND RAPE (QUIVVER REMIX)", "RON HARDY"
    };
    static const char *const artists[D2_BROWSE_ROWS] = {
        "STUDIO RAUSCHENBERG", "MARCELO CURA & ORI", "MIGUEL A.F.",
        "SAMMY LEGS & MO", "PETE MOSS", "BROMLEY", "ANNA.",
        "BECKERS & HATFIELD", "AVON STRINGER"
    };
    static const char *const keys[D2_BROWSE_ROWS] = {
        "10A", "8A", "7A", "6A", "4A", "4A", "4A", "4A", "2A"
    };
    for (int row = 0; row < D2_BROWSE_ROWS; ++row) {
        struct d2_browse_entry *entry = &d2_browse_entries[row];
        entry->track_id = row + 1;
        snprintf(entry->title, sizeof(entry->title), "%s", titles[row]);
        snprintf(entry->artist, sizeof(entry->artist), "%s", artists[row]);
        snprintf(entry->musical_key, sizeof(entry->musical_key), "%s", keys[row]);
        entry->bpm = row < 4 ? 123.0f + row : 125.0f;
        entry->rating = row == D2_BROWSE_ROWS / 2 ? 5 : 3;
        entry->available = 1;
    }
    struct d2_screen_state state = {
        .browse_focus = browse_focus,
        .browse_sort_column = 2,
    };
    d2_render_browse_fast(pixels, 1, &state);
    if (notice != D2_BROWSE_NOTICE_NONE)
        d2_draw_browse_notice(pixels, notice);
    FILE *output = fopen(path, "wb");
    if (!output) {
        free(pixels);
        return 1;
    }
    fprintf(output, "P6\n%d %d\n255\n", WIDTH, HEIGHT);
    for (int i = 0; i < WIDTH * HEIGHT; ++i) {
        uint16_t color = ((uint16_t)pixels[i * 2] << 8) | pixels[i * 2 + 1];
        unsigned char rgb[3] = {
            (unsigned char)(((color >> 11) & 31) * 255 / 31),
            (unsigned char)(((color >> 5) & 63) * 255 / 63),
            (unsigned char)((color & 31) * 255 / 31),
        };
        fwrite(rgb, 1, sizeof(rgb), output);
    }
    fclose(output);
    free(pixels);
    return 0;
}

static int d2_render_performance_test_image(const char *path)
{
    uint8_t *pixels = calloc((size_t)WIDTH * HEIGHT * 2, 1);
    if (!pixels)
        return 1;
    struct d2_screen_state state = {
        .title = "PERFORMANCE FEEDBACK",
        .artist = "MIXXX",
    };
    static const uint32_t colors[8] = {
        0x042010U, 0x042010U, 0x087030U, 0x042010U,
        0x00ff60U, 0x042010U, 0x042010U, 0x042010U,
    };
    memcpy(d2_led_state[1].pad_rgb, colors, sizeof(colors));
    d2_render_dynamic_performance_view(pixels, D2_VIEW_LOOP, state.title,
                                       colors, 220, 40, 40);

    FILE *output = fopen(path, "wb");
    if (!output) {
        free(pixels);
        return 1;
    }
    fprintf(output, "P6\n%d %d\n255\n", WIDTH, HEIGHT);
    for (int i = 0; i < WIDTH * HEIGHT; ++i) {
        uint16_t color = ((uint16_t)pixels[i * 2] << 8) |
                         pixels[i * 2 + 1];
        unsigned char rgb[3] = {
            (unsigned char)(((color >> 11) & 31) * 255 / 31),
            (unsigned char)(((color >> 5) & 63) * 255 / 63),
            (unsigned char)((color & 31) * 255 / 31),
        };
        fwrite(rgb, 1, sizeof(rgb), output);
    }
    fclose(output);
    free(pixels);
    return 0;
}

static void d2_parse_test_message(const char *message)
{
    unsigned char sysex[256];
    size_t length = strlen(message);
    if (length + 3 > sizeof(sysex))
        return;
    sysex[0] = 0xF0;
    sysex[1] = 0x7D;
    memcpy(sysex + 2, message, length);
    sysex[length + 2] = 0xF7;
    d2_parse_sysex(sysex, (uint32_t)(length + 3));
}

static int d2_test_white_ink_bounds(const uint8_t *pixels,
                                    int x0, int y0, int width, int height,
                                    int *min_x, int *min_y,
                                    int *max_x, int *max_y)
{
    int found = 0;
    *min_x = x0 + width;
    *min_y = y0 + height;
    *max_x = x0 - 1;
    *max_y = y0 - 1;
    for (int y = y0; y < y0 + height; ++y) {
        for (int x = x0; x < x0 + width; ++x) {
            size_t offset = ((size_t)y * WIDTH + x) * 2;
            uint16_t color = ((uint16_t)pixels[offset] << 8) |
                             pixels[offset + 1];
            int r = ((color >> 11) & 0x1f) * 255 / 31;
            int g = ((color >> 5) & 0x3f) * 255 / 63;
            int b = (color & 0x1f) * 255 / 31;
            if (r < 180 || g < 180 || b < 180)
                continue;
            if (x < *min_x) *min_x = x;
            if (x > *max_x) *max_x = x;
            if (y < *min_y) *min_y = y;
            if (y > *max_y) *max_y = y;
            found = 1;
        }
    }
    return found;
}

static uint64_t d2_test_region_hash(const uint8_t *pixels,
                                    int x0, int y0, int width, int height)
{
    uint64_t hash = 1469598103934665603ULL;
    for (int y = y0; y < y0 + height; ++y) {
        const uint8_t *row = &pixels[((size_t)y * WIDTH + x0) * 2];
        for (int byte = 0; byte < width * 2; ++byte) {
            hash ^= row[byte];
            hash *= 1099511628211ULL;
        }
    }
    return hash;
}

static uint64_t d2_test_render_hud_region(uint8_t *pixels,
                                          int x, int y,
                                          int width, int height)
{
    memset(pixels, 0, (size_t)WIDTH * HEIGHT * 2);
    d2_render_deck_fast(pixels, 1, &d2_screen_state[1],
                        d2_screen_state[1].position,
                        d2_screen_state[1].title, 220, 40, 40);
    return d2_test_region_hash(pixels, x, y, width, height);
}

static int d2_hud_parser_render_contract_test(uint8_t *pixels)
{
    struct d2_screen_state saved_state_1 = d2_screen_state[1];
    struct d2_screen_state saved_state_2 = d2_screen_state[2];
    struct d2_led_state saved_led_1 = d2_led_state[1];
    struct d2_led_state saved_led_2 = d2_led_state[2];
    uint8_t *saved_waveform = d2_waveform[1];
    int saved_waveform_ready = d2_waveform_ready[1];
    int saved_strip_ready = d2_wave_strip_ready[1];
    int saved_cover_ready = d2_cover_art_ready[1];
    uint8_t *test_waveform = calloc(D2_WAVEFORM_POINTS, 1);
    const char *failure = NULL;

    if (!test_waveform)
        return 1;
    for (int point = 0; point < D2_WAVEFORM_POINTS; ++point)
        test_waveform[point] =
            (uint8_t)(4 + ((point * 37U + (unsigned)(point >> 3) * 11U) % 42U));

    d2_screen_state[1] = (struct d2_screen_state) {
        .bpm = 120.0f,
        .position = 0.25f,
        .remaining = 180.0f,
        .duration = 240.0f,
        .rate = 1.0f,
        .zoom_level = 2,
        .loop_size = 4.0f,
        .visual_key = 8,
        .title = "BASE TRACK",
        .artist = "BASE ARTIST",
        .hotcue_position = {-1,-1,-1,-1,-1,-1,-1,-1},
    };
    d2_led_state[1] = (struct d2_led_state) {
        .outputs_enabled = 1,
        .active_channel = 1,
        .mode = 1,
        .fx_unit = 1,
        .performance_on = {1, 1, 1, 1},
    };
    d2_waveform[1] = test_waveform;
    d2_waveform_ready[1] = 1;
    d2_wave_strip_ready[1] = 0;
    d2_cover_art_ready[1] = 0;

    uint64_t before = d2_test_render_hud_region(pixels, 0, 0, 320, 18);
    d2_parse_test_message("D2|1|TITLE|LIVE TRACK");
    if (strcmp(d2_screen_state[1].title, "LIVE TRACK") != 0 ||
        d2_test_render_hud_region(pixels, 0, 0, 320, 18) == before) {
        failure = "hud-title";
        goto cleanup;
    }

    uint64_t bpm_header_before =
        d2_test_render_hud_region(pixels, 325, 0, 78, 18);
    uint64_t bpm_hud_before =
        d2_test_render_hud_region(pixels, 396, 195, 84, 43);
    d2_parse_test_message("D2|1|BPM|129.57");
    if (fabsf(d2_screen_state[1].bpm - 129.57f) > 0.001f ||
        d2_test_render_hud_region(pixels, 325, 0, 78, 18) ==
            bpm_header_before ||
        d2_test_render_hud_region(pixels, 396, 195, 84, 43) ==
            bpm_hud_before) {
        failure = "hud-bpm";
        goto cleanup;
    }

    uint64_t key_header_before =
        d2_test_render_hud_region(pixels, 409, 0, 47, 18);
    uint64_t key_hud_before =
        d2_test_render_hud_region(pixels, 54, 195, 72, 43);
    d2_parse_test_message("D2|1|KEYVISUAL|20");
    if (d2_screen_state[1].visual_key != 20 ||
        d2_test_render_hud_region(pixels, 409, 0, 47, 18) ==
            key_header_before ||
        d2_test_render_hud_region(pixels, 54, 195, 72, 43) ==
            key_hud_before) {
        failure = "hud-key";
        goto cleanup;
    }

    before = d2_test_render_hud_region(pixels, 54, 195, 72, 15);
    d2_parse_test_message("D2|1|KEYLOCK|1");
    if (!d2_screen_state[1].keylock ||
        d2_test_render_hud_region(pixels, 54, 195, 72, 15) == before) {
        failure = "hud-keylock";
        goto cleanup;
    }

    before = d2_test_render_hud_region(pixels, 126, 195, 42, 40);
    d2_parse_test_message("D2|1|LOOPSIZE|16");
    if (fabsf(d2_screen_state[1].loop_size - 16.0f) > 0.001f ||
        d2_test_render_hud_region(pixels, 126, 195, 42, 40) == before) {
        failure = "hud-loop-size";
        goto cleanup;
    }
    before = d2_test_render_hud_region(pixels, 126, 195, 42, 40);
    d2_parse_test_message("D2|1|LEDLOOP|1");
    if (!d2_led_state[1].loop ||
        d2_test_render_hud_region(pixels, 126, 195, 42, 40) == before) {
        failure = "hud-loop-active";
        goto cleanup;
    }

    before = d2_test_render_hud_region(pixels, 184, 195, 112, 43);
    d2_parse_test_message("D2|1|TIMEMODE|1");
    if (!d2_screen_state[1].time_mode ||
        d2_test_render_hud_region(pixels, 184, 195, 112, 43) == before) {
        failure = "hud-time-mode";
        goto cleanup;
    }

    before = d2_test_render_hud_region(pixels, 232, 195, 80, 14);
    d2_parse_test_message("D2|1|QUANTIZE|1");
    if (!d2_screen_state[1].quantize ||
        d2_test_render_hud_region(pixels, 232, 195, 80, 14) == before) {
        failure = "hud-quantize";
        goto cleanup;
    }

    before = d2_test_render_hud_region(pixels, 312, 195, 82, 43);
    d2_parse_test_message("D2|1|RATE|1.02410");
    if (fabsf(d2_screen_state[1].rate - 1.0241f) > 0.00001f ||
        d2_test_render_hud_region(pixels, 312, 195, 82, 43) == before) {
        failure = "hud-tempo";
        goto cleanup;
    }

    before = d2_test_render_hud_region(pixels, 354, 195, 40, 16);
    d2_parse_test_message("D2|1|LEDSYNC|1");
    if (!d2_led_state[1].sync ||
        d2_test_render_hud_region(pixels, 354, 195, 40, 16) == before) {
        failure = "hud-sync";
        goto cleanup;
    }

    before = d2_test_render_hud_region(pixels, 0, 49, 32, 14);
    d2_parse_test_message("D2|1|ZOOM|8");
    if (d2_screen_state[1].zoom_level != 8 ||
        d2_test_render_hud_region(pixels, 0, 49, 32, 14) == before) {
        failure = "hud-zoom";
        goto cleanup;
    }

    d2_parse_test_message("D2|1|BROWSESORT|15,1");
    if (d2_screen_state[1].browse_sort_column != 15 ||
        d2_screen_state[1].browse_sort_order != 1) {
        failure = "browse-sort-state";
        goto cleanup;
    }

    before = d2_test_render_hud_region(pixels, 140, 18, 190, 31);
    d2_parse_test_message("D2|1|PHASE|0.25,0.75,1,3,1,1,2");
    if (!d2_screen_state[1].phase_valid ||
        d2_screen_state[1].phase_master_step != 1 ||
        d2_screen_state[1].phase_active_step != 3 ||
        d2_test_render_hud_region(pixels, 140, 18, 190, 31) == before) {
        failure = "hud-phase";
        goto cleanup;
    }

    before = d2_test_render_hud_region(pixels, 0, 235, 480, 37);
    d2_parse_test_message("D2|1|CUE1|0.750000");
    d2_parse_test_message("D2|1|CUECOLOR1|16744448");
    if (fabsf(d2_screen_state[1].hotcue_position[0] - 0.75f) > 0.00001f ||
        d2_screen_state[1].hotcue_color[0] != 0xff8000U ||
        d2_test_render_hud_region(pixels, 0, 235, 480, 37) == before) {
        failure = "hud-hotcue";
        goto cleanup;
    }

    before = d2_test_render_hud_region(pixels, 0, 235, 480, 37);
    d2_parse_test_message("D2|1|POS|0.600000");
    if (fabsf(d2_screen_state[1].position - 0.6f) > 0.00001f ||
        d2_test_render_hud_region(pixels, 0, 235, 480, 37) == before) {
        failure = "hud-playhead";
        goto cleanup;
    }

    before = d2_test_render_hud_region(pixels, 184, 195, 112, 43);
    d2_parse_test_message("D2|1|DURATION|300.000");
    if (fabsf(d2_screen_state[1].duration - 300.0f) > 0.001f ||
        d2_test_render_hud_region(pixels, 184, 195, 112, 43) == before) {
        failure = "hud-duration";
        goto cleanup;
    }

    d2_screen_state[1].position = 0.25f;
    d2_screen_state[1].duration = 240.0f;
    d2_screen_state[1].rate = 1.0f;
    clock_gettime(CLOCK_MONOTONIC,
                  &d2_screen_state[1].position_updated_at);
    d2_screen_state[1].position_updated_at.tv_nsec -= 100000000L;
    if (d2_screen_state[1].position_updated_at.tv_nsec < 0) {
        --d2_screen_state[1].position_updated_at.tv_sec;
        d2_screen_state[1].position_updated_at.tv_nsec += 1000000000L;
    }
    d2_parse_test_message("D2|1|PLAY|1");
    if (!d2_screen_state[1].playing ||
        d2_effective_position(&d2_screen_state[1]) <= 0.2501f) {
        failure = "hud-transport-clock";
        goto cleanup;
    }

    if (memcmp(&d2_screen_state[2], &saved_state_2,
               sizeof(saved_state_2)) != 0 ||
        memcmp(&d2_led_state[2], &saved_led_2,
               sizeof(saved_led_2)) != 0) {
        failure = "hud-deck-isolation";
        goto cleanup;
    }

cleanup:
    d2_screen_state[1] = saved_state_1;
    d2_screen_state[2] = saved_state_2;
    d2_led_state[1] = saved_led_1;
    d2_led_state[2] = saved_led_2;
    d2_waveform[1] = saved_waveform;
    d2_waveform_ready[1] = saved_waveform_ready;
    d2_wave_strip_ready[1] = saved_strip_ready;
    d2_cover_art_ready[1] = saved_cover_ready;
    free(test_waveform);
    if (failure) {
        fprintf(stderr,
                "D2_FUNCTIONALITY_CONTRACT_TEST_FAILED %s\n", failure);
        return 1;
    }
    return 0;
}

static int d2_load_reject_notice_test(void)
{
    const uint64_t start_us = 1000000ULL;
    uint64_t initial_generation = d2_browse_generation;

    d2_browse_notice_kind[1] = D2_BROWSE_NOTICE_NONE;
    d2_browse_notice_kind[2] = D2_BROWSE_NOTICE_NONE;
    d2_browse_notice_until_us[1] = 0;
    d2_browse_notice_until_us[2] = 0;
    d2_set_browse_notice(
        1, D2_BROWSE_NOTICE_DECK_PLAYING, start_us);
    if (d2_active_browse_notice(1, start_us) !=
            D2_BROWSE_NOTICE_DECK_PLAYING ||
        d2_active_browse_notice(2, start_us) != D2_BROWSE_NOTICE_NONE ||
        d2_browse_generation == initial_generation) {
        fprintf(stderr, "D2_LOAD_REJECT_NOTICE_TEST_FAILED activation\n");
        return 1;
    }

    uint64_t shown_generation = d2_browse_generation;
    if (d2_active_browse_notice(
                1, start_us + D2_BROWSE_NOTICE_US - 1) !=
            D2_BROWSE_NOTICE_DECK_PLAYING ||
        d2_browse_generation != shown_generation) {
        fprintf(stderr, "D2_LOAD_REJECT_NOTICE_TEST_FAILED early-expiry\n");
        return 1;
    }

    if (d2_active_browse_notice(
                1, start_us + D2_BROWSE_NOTICE_US) !=
            D2_BROWSE_NOTICE_NONE ||
        d2_browse_notice_kind[1] != D2_BROWSE_NOTICE_NONE ||
        d2_browse_notice_until_us[1] != 0 ||
        d2_browse_generation == shown_generation) {
        fprintf(stderr, "D2_LOAD_REJECT_NOTICE_TEST_FAILED expiry\n");
        return 1;
    }

    d2_set_browse_notice(2, D2_BROWSE_NOTICE_TRACK_MISSING, start_us);
    uint64_t second_generation = d2_browse_generation;
    if (d2_active_browse_notice(2, start_us) !=
            D2_BROWSE_NOTICE_TRACK_MISSING) {
        fprintf(stderr, "D2_LOAD_REJECT_NOTICE_TEST_FAILED kind\n");
        return 1;
    }
    d2_clear_browse_notice(2);
    if (d2_browse_notice_kind[2] != D2_BROWSE_NOTICE_NONE ||
        d2_browse_notice_until_us[2] != 0 ||
        d2_browse_generation == second_generation) {
        fprintf(stderr, "D2_LOAD_REJECT_NOTICE_TEST_FAILED clear\n");
        return 1;
    }

    /* The most recent authoritative outcome replaces the previous one; no
     * overlapping boxes or queued notices are allowed. */
    d2_set_browse_notice(1, D2_BROWSE_NOTICE_TRACK_MISSING, start_us);
    d2_set_browse_notice(1, D2_BROWSE_NOTICE_LOAD_FAILED, start_us + 10);
    if (d2_active_browse_notice(1, start_us + 10) !=
            D2_BROWSE_NOTICE_LOAD_FAILED ||
        d2_browse_notice_until_us[1] !=
            start_us + 10 + D2_BROWSE_NOTICE_US) {
        fprintf(stderr, "D2_LOAD_REJECT_NOTICE_TEST_FAILED replacement\n");
        return 1;
    }
    d2_clear_browse_notice(1);

    /* LOADFAIL is compositor feedback for an active Browse interaction, not
     * state that may leak into Browse from an earlier Player-screen event. */
    d2_screen_view[1] = D2_VIEW_DECK;
    d2_parse_test_message("D2|1|LOADFAIL|MISSING");
    if (d2_browse_notice_kind[1] != D2_BROWSE_NOTICE_NONE) {
        fprintf(stderr, "D2_LOAD_REJECT_NOTICE_TEST_FAILED view-gate\n");
        return 1;
    }
    d2_screen_view[1] = D2_VIEW_BROWSE;
    d2_parse_test_message("D2|1|LOADFAIL|MISSING");
    if (d2_browse_notice_kind[1] != D2_BROWSE_NOTICE_TRACK_MISSING) {
        fprintf(stderr, "D2_LOAD_REJECT_NOTICE_TEST_FAILED missing-parser\n");
        return 1;
    }
    d2_parse_test_message("D2|1|LOADFAIL|FAILED");
    if (d2_browse_notice_kind[1] != D2_BROWSE_NOTICE_LOAD_FAILED) {
        fprintf(stderr, "D2_LOAD_REJECT_NOTICE_TEST_FAILED failed-parser\n");
        return 1;
    }
    d2_parse_test_message("D2|1|VIEW|DECK");
    if (d2_browse_notice_kind[1] != D2_BROWSE_NOTICE_NONE ||
        d2_screen_view[1] != D2_VIEW_DECK) {
        fprintf(stderr, "D2_LOAD_REJECT_NOTICE_TEST_FAILED success-clear\n");
        return 1;
    }

    printf("D2_LOAD_REJECT_NOTICE_TEST_OK\n");
    return 0;
}

static int d2_beat_geometry_test(void)
{
    const float play_position = 0.5f;
    const float source_step = 32.0f;
    const float one_pixel = source_step / (D2_WAVEFORM_POINTS - 1);
    const float irregular[] = {
        play_position - 7.0f * one_pixel,
        play_position - 2.0f * one_pixel,
        play_position,
        play_position + 3.0f * one_pixel,
        play_position + 11.0f * one_pixel,
    };
    const int expected[] = {233, 238, 240, 243, 251};

    for (size_t i = 0; i < sizeof(irregular) / sizeof(irregular[0]); ++i) {
        int x = d2_beat_position_to_x(
                irregular[i], play_position, source_step);
        if (x != expected[i]) {
            fprintf(stderr,
                    "D2_BEAT_GEOMETRY_TEST_FAILED index=%zu got=%d expected=%d\n",
                    i, x, expected[i]);
            return 1;
        }
    }
    if (d2_beat_position_to_x(-0.01f, 0.0f, source_step) >= 240 ||
        d2_beat_position_to_x(NAN, play_position, source_step) != -1 ||
        d2_beat_position_to_x(play_position, play_position, 0.0f) != -1) {
        fprintf(stderr, "D2_BEAT_GEOMETRY_TEST_FAILED bounds\n");
        return 1;
    }
    printf("D2_BEAT_GEOMETRY_TEST_OK\n");
    return 0;
}

struct d2_font_thread_test_context {
    pthread_barrier_t barrier;
    uint8_t *pixels;
    int ready;
    int width;
    uintptr_t face_slot;
    uintptr_t cache_slot;
};

static void d2_font_thread_test_draw(uint8_t *pixels)
{
    static const char title[] =
        "CACHE \xC3\x87I\xC4\x9ELIK 128.00";
    memset(pixels, 0, (size_t)WIDTH * HEIGHT * 2);
    d2_draw_text(pixels, title, 12, 18, 1, 238, 240, 244);
    d2_draw_text(pixels, title, 12, 52, 2, 0, 210, 238);
    d2_draw_text_centered(pixels, title, WIDTH / 2, 96, 3,
                          245, 150, 20);
}

static void *d2_font_thread_test_worker(void *userdata)
{
    struct d2_font_thread_test_context *context = userdata;
    d2_font_init();
    context->ready = d2_ft_ready;
    context->face_slot = (uintptr_t)&d2_ft_face;
    context->cache_slot = (uintptr_t)&d2_font_cache[0][0];
    context->width = d2_measure_text_width(
        "CACHE \xC3\x87I\xC4\x9ELIK 128.00", 2);
    pthread_barrier_wait(&context->barrier);
    d2_font_thread_test_draw(context->pixels);
    d2_font_shutdown();
    return NULL;
}

static int d2_font_thread_contract_test(void)
{
    struct d2_font_thread_test_context context = {0};
    pthread_t worker;
    uint8_t *main_pixels = calloc((size_t)WIDTH * HEIGHT * 2, 1);
    uint8_t *worker_pixels = calloc((size_t)WIDTH * HEIGHT * 2, 1);
    if (!main_pixels || !worker_pixels) {
        free(main_pixels);
        free(worker_pixels);
        return 1;
    }
    context.pixels = worker_pixels;
    if (pthread_barrier_init(&context.barrier, NULL, 2) != 0) {
        free(main_pixels);
        free(worker_pixels);
        return 1;
    }
    if (pthread_create(&worker, NULL, d2_font_thread_test_worker,
                       &context) != 0) {
        pthread_barrier_destroy(&context.barrier);
        free(main_pixels);
        free(worker_pixels);
        return 1;
    }

    uintptr_t main_face_slot = (uintptr_t)&d2_ft_face;
    uintptr_t main_cache_slot = (uintptr_t)&d2_font_cache[0][0];
    int main_ready = d2_ft_ready;
    int main_width = d2_measure_text_width(
        "CACHE \xC3\x87I\xC4\x9ELIK 128.00", 2);
    pthread_barrier_wait(&context.barrier);
    d2_font_thread_test_draw(main_pixels);
    pthread_join(worker, NULL);
    pthread_barrier_destroy(&context.barrier);

    int failed = context.face_slot == main_face_slot ||
        context.cache_slot == main_cache_slot ||
        context.ready != main_ready || context.width != main_width ||
        memcmp(main_pixels, worker_pixels,
               (size_t)WIDTH * HEIGHT * 2) != 0;
    free(main_pixels);
    free(worker_pixels);
    if (failed) {
        fprintf(stderr,
                "D2_FUNCTIONALITY_CONTRACT_TEST_FAILED font-thread-isolation\n");
        return 1;
    }
    return 0;
}

static int d2_performance_cache_contract_test(void)
{
    const char *failure = NULL;
    struct d2_performance_visible_state saved_frame[3];
    int saved_frame_valid[3];
    int saved_last_view[3];
    uint64_t saved_browse_rendered_generation[3];
    int saved_browse_frame_valid[3];
    uint64_t saved_browse_generation = d2_browse_generation;
    uint32_t pads[8] = {
        0x042010U, 0x042010U, 0x087030U, 0x042010U,
        0x00ff60U, 0x042010U, 0x042010U, 0x042010U,
    };
    uint32_t changed_pads[8];
    uint32_t upper_bits_only[8];
    char title_a[80];
    char title_b[80];

    memcpy(saved_frame, d2_performance_frame, sizeof(saved_frame));
    memcpy(saved_frame_valid, d2_performance_frame_valid,
           sizeof(saved_frame_valid));
    memcpy(saved_last_view, d2_last_screen_view, sizeof(saved_last_view));
    memcpy(saved_browse_rendered_generation,
           d2_browse_rendered_generation,
           sizeof(saved_browse_rendered_generation));
    memcpy(saved_browse_frame_valid, d2_browse_frame_valid,
           sizeof(saved_browse_frame_valid));

    memset(d2_performance_frame, 0, sizeof(d2_performance_frame));
    memset(d2_performance_frame_valid, 0,
           sizeof(d2_performance_frame_valid));
    for (int player = 0; player < 3; ++player)
        d2_last_screen_view[player] = -1;

    char snapshot_title[80];
    uint32_t snapshot_pads[8];
    pthread_mutex_lock(&d2_state_mutex);
    int snapshot_while_busy = d2_try_performance_snapshot(
        1, snapshot_title, sizeof(snapshot_title), snapshot_pads);
    pthread_mutex_unlock(&d2_state_mutex);
    if (snapshot_while_busy != 0 ||
        !d2_try_performance_snapshot(
            1, snapshot_title, sizeof(snapshot_title), snapshot_pads)) {
        failure = "performance-cache-nonblocking-snapshot";
        goto cleanup;
    }

    d2_visible_deck_title(title_a, sizeof(title_a),
                          "CACHE TRACK (ORIGINAL MIX)");
    d2_visible_deck_title(title_b, sizeof(title_b),
                          "CACHE TRACK [EXTENDED MIX]");
    struct d2_performance_visible_state loop_visible;
    struct d2_performance_visible_state repeated_visible;
    struct d2_performance_visible_state upper_bits_visible;
    struct d2_performance_visible_state changed_pad_visible;
    struct d2_performance_visible_state hidden_suffix_visible;
    struct d2_performance_visible_state changed_title_visible;
    struct d2_performance_visible_state changed_view_visible;
    d2_make_performance_visible_state(
        &loop_visible, D2_VIEW_LOOP, title_a, pads);
    d2_make_performance_visible_state(
        &repeated_visible, D2_VIEW_LOOP, title_a, pads);

    if (!d2_performance_visible_equal(
            &loop_visible, &repeated_visible) ||
        d2_performance_frame_is_current(1, &loop_visible)) {
        failure = "performance-cache-first";
        goto cleanup;
    }
    d2_commit_performance_frame(1, &loop_visible);
    if (!d2_performance_frame_is_current(1, &loop_visible)) {
        failure = "performance-cache-stable";
        goto cleanup;
    }

    memcpy(upper_bits_only, pads, sizeof(pads));
    upper_bits_only[0] |= 0xff000000U;
    d2_make_performance_visible_state(
        &upper_bits_visible, D2_VIEW_LOOP, title_a, upper_bits_only);
    if (!d2_performance_visible_equal(
            &upper_bits_visible, &loop_visible)) {
        failure = "performance-cache-rgb-mask";
        goto cleanup;
    }

    memcpy(changed_pads, pads, sizeof(pads));
    changed_pads[0] ^= 0x00010101U;
    d2_make_performance_visible_state(
        &changed_pad_visible, D2_VIEW_LOOP, title_a, changed_pads);
    if (d2_performance_visible_equal(
            &changed_pad_visible, &loop_visible)) {
        failure = "performance-cache-pad";
        goto cleanup;
    }
    d2_make_performance_visible_state(
        &hidden_suffix_visible, D2_VIEW_LOOP, title_b, pads);
    if (strcmp(title_a, title_b) != 0 ||
        !d2_performance_visible_equal(
            &hidden_suffix_visible, &loop_visible)) {
        failure = "performance-cache-hidden-title-suffix";
        goto cleanup;
    }
    d2_make_performance_visible_state(
        &changed_title_visible, D2_VIEW_LOOP, "OTHER TRACK", pads);
    if (d2_performance_visible_equal(
            &changed_title_visible, &loop_visible)) {
        failure = "performance-cache-title";
        goto cleanup;
    }
    d2_make_performance_visible_state(
        &changed_view_visible, D2_VIEW_FREEZE, title_a, pads);
    if (d2_performance_visible_equal(
            &changed_view_visible, &loop_visible)) {
        failure = "performance-cache-view";
        goto cleanup;
    }
    if (d2_performance_frame_is_current(2, &loop_visible)) {
        failure = "performance-cache-deck-isolation";
        goto cleanup;
    }

    /* The same Performance content is no longer current after another full
     * view was submitted; re-entry must send one complete frame. */
    d2_last_screen_view[1] = D2_VIEW_DECK;
    if (d2_performance_frame_is_current(1, &loop_visible)) {
        failure = "performance-cache-reentry";
        goto cleanup;
    }

    d2_browse_generation = 77;
    d2_browse_rendered_generation[1] = 77;
    d2_browse_frame_valid[1] = 1;
    d2_last_screen_view[1] = D2_VIEW_DECK;
    if (d2_browse_frame_is_current(1)) {
        failure = "browse-cache-reentry";
        goto cleanup;
    }
    d2_last_screen_view[1] = D2_VIEW_BROWSE;
    if (!d2_browse_frame_is_current(1) ||
        d2_browse_frame_is_current(2)) {
        failure = "browse-cache-stable-isolation";
        goto cleanup;
    }

cleanup:
    memcpy(d2_performance_frame, saved_frame, sizeof(saved_frame));
    memcpy(d2_performance_frame_valid, saved_frame_valid,
           sizeof(saved_frame_valid));
    memcpy(d2_last_screen_view, saved_last_view, sizeof(saved_last_view));
    memcpy(d2_browse_rendered_generation,
           saved_browse_rendered_generation,
           sizeof(saved_browse_rendered_generation));
    memcpy(d2_browse_frame_valid, saved_browse_frame_valid,
           sizeof(saved_browse_frame_valid));
    d2_browse_generation = saved_browse_generation;
    if (failure) {
        fprintf(stderr, "D2_FUNCTIONALITY_CONTRACT_TEST_FAILED %s\n",
                failure);
        return 1;
    }
    return 0;
}

static int d2_performance_callback_contract_test(uint8_t *pixels)
{
    const char *failure = NULL;
    struct ctlra_dev_t *fake_dev =
        (struct ctlra_dev_t *)(uintptr_t)1;
    int saved_map_count = d2_map_count;
    struct ctlra_dev_t *saved_dev = d2_map[0].dev;
    const char *saved_serial = d2_map[0].serial;
    int saved_player = d2_map[0].player;
    enum d2_screen_view saved_screen_view = d2_screen_view[1];
    char saved_title[sizeof(d2_screen_state[1].title)];
    uint32_t saved_pads[8];
    struct d2_performance_visible_state saved_frame =
        d2_performance_frame[1];
    int saved_frame_valid = d2_performance_frame_valid[1];
    int saved_last_view = d2_last_screen_view[1];
    int saved_render_ready = d2_render_buffer_ready[1];
    unsigned saved_render_index = d2_render_buffer_index[1];
    uint64_t saved_render_generation = d2_render_generation[1];
    uint64_t saved_usb_generation = d2_usb_generation[1];

    if (!pixels)
        return 1;
    d2_copy_text(saved_title, sizeof(saved_title),
                 d2_screen_state[1].title);
    memcpy(saved_pads, d2_led_state[1].pad_rgb, sizeof(saved_pads));

    d2_map_count = 1;
    d2_map[0].dev = fake_dev;
    d2_map[0].serial = "TEST";
    d2_map[0].player = 1;
    d2_screen_view[1] = D2_VIEW_LOOP;
    d2_copy_text(d2_screen_state[1].title,
                 sizeof(d2_screen_state[1].title), "CACHE CALLBACK");
    memset(d2_led_state[1].pad_rgb, 0,
           sizeof(d2_led_state[1].pad_rgb));
    d2_led_state[1].pad_rgb[0] = 0x00ff60U;
    d2_performance_frame_valid[1] = 0;
    d2_last_screen_view[1] = -1;

    int first = screen_callback(fake_dev, 0, pixels,
                                WIDTH * HEIGHT * 2, NULL, NULL);
    int stable = screen_callback(fake_dev, 0, pixels,
                                 WIDTH * HEIGHT * 2, NULL, NULL);
    if (first != 1 || stable != 0) {
        failure = "performance-callback-first-stable";
        goto cleanup;
    }

    d2_led_state[1].pad_rgb[0] = 0xff7800U;
    int pad_changed = screen_callback(fake_dev, 0, pixels,
                                      WIDTH * HEIGHT * 2, NULL, NULL);
    int pad_stable = screen_callback(fake_dev, 0, pixels,
                                     WIDTH * HEIGHT * 2, NULL, NULL);
    if (pad_changed != 1 || pad_stable != 0) {
        failure = "performance-callback-pad";
        goto cleanup;
    }

    d2_screen_view[1] = D2_VIEW_DECK;
    d2_render_buffer_ready[1] = 1;
    d2_render_buffer_index[1] = 0;
    d2_render_generation[1] = 101;
    d2_usb_generation[1] = 100;
    if (screen_callback(fake_dev, 0, pixels,
                        WIDTH * HEIGHT * 2, NULL, NULL) != 1 ||
        d2_last_screen_view[1] != D2_VIEW_DECK) {
        failure = "performance-callback-deck-submit";
        goto cleanup;
    }

    d2_screen_view[1] = D2_VIEW_LOOP;
    int reentry = screen_callback(fake_dev, 0, pixels,
                                  WIDTH * HEIGHT * 2, NULL, NULL);
    int reentry_stable = screen_callback(fake_dev, 0, pixels,
                                         WIDTH * HEIGHT * 2, NULL, NULL);
    if (reentry != 1 || reentry_stable != 0) {
        failure = "performance-callback-reentry";
        goto cleanup;
    }

cleanup:
    d2_map_count = saved_map_count;
    d2_map[0].dev = saved_dev;
    d2_map[0].serial = saved_serial;
    d2_map[0].player = saved_player;
    d2_screen_view[1] = saved_screen_view;
    d2_copy_text(d2_screen_state[1].title,
                 sizeof(d2_screen_state[1].title), saved_title);
    memcpy(d2_led_state[1].pad_rgb, saved_pads, sizeof(saved_pads));
    d2_performance_frame[1] = saved_frame;
    d2_performance_frame_valid[1] = saved_frame_valid;
    d2_last_screen_view[1] = saved_last_view;
    d2_render_buffer_ready[1] = saved_render_ready;
    d2_render_buffer_index[1] = saved_render_index;
    d2_render_generation[1] = saved_render_generation;
    d2_usb_generation[1] = saved_usb_generation;
    if (failure) {
        fprintf(stderr, "D2_FUNCTIONALITY_CONTRACT_TEST_FAILED %s\n",
                failure);
        return 1;
    }
    return 0;
}

static int d2_functionality_contract_test(void)
{
    if (d2_font_thread_contract_test() != 0)
        return 1;
    if (d2_performance_cache_contract_test() != 0)
        return 1;
    if (d2_browse_delta_steps(0) != 0 ||
        d2_browse_delta_steps(3) != 3 ||
        d2_browse_delta_steps(-4) != -4 ||
        d2_browse_delta_steps(999) != D2_BROWSE_MAX_STEPS_PER_REPORT ||
        d2_browse_delta_steps(-999) != -D2_BROWSE_MAX_STEPS_PER_REPORT) {
        fprintf(stderr,
                "D2_FUNCTIONALITY_CONTRACT_TEST_FAILED browse-delta\n");
        return 1;
    }

    uint8_t *pixels = calloc((size_t)WIDTH * HEIGHT * 2, 1);
    if (!pixels)
        return 1;
    if (d2_performance_callback_contract_test(pixels) != 0) {
        free(pixels);
        return 1;
    }
    if (d2_hud_parser_render_contract_test(pixels) != 0) {
        free(pixels);
        return 1;
    }

    struct d2_screen_state state = {
        .rate = 1.0f,
        .zoom_level = 2,
        .loop_size = 4.0f,
        .title = "DECK 1",
        .artist = "MIXXX",
        .hotcue_position = {-1,-1,-1,-1,-1,-1,-1,-1},
    };
    d2_waveform_ready[1] = 0;
    d2_wave_strip_ready[1] = 0;
    d2_cover_art_ready[1] = 0;
    d2_render_deck_fast(pixels, 1, &state, 0.0f, state.title,
                        220, 40, 40);

    size_t live_probe = ((size_t)115 * WIDTH + 100) * 2;
    if (pixels[live_probe] != 0 || pixels[live_probe + 1] != 0) {
        fprintf(stderr, "D2_FUNCTIONALITY_CONTRACT_TEST_FAILED fake-waveform\n");
        free(pixels);
        return 1;
    }
    uint8_t overview_background[2];
    rgb565(overview_background, 7, 14, 20);
    size_t overview_probe = ((size_t)253 * WIDTH + 100) * 2;
    if (memcmp(&pixels[overview_probe], overview_background, 2) != 0) {
        fprintf(stderr, "D2_FUNCTIONALITY_CONTRACT_TEST_FAILED fake-overview\n");
        free(pixels);
        return 1;
    }

    state.fx_touch_mask = 1;
    d2_fx_touch_updated_us[1] = d2_monotonic_us() - 1500001ULL;
    d2_render_deck_fast(pixels, 1, &state, 0.0f, state.title,
                        220, 40, 40);
    d2_render_deck_fast(pixels, 1, &state, 0.0f, state.title,
                        220, 40, 40);
    if (state.fx_touch_mask != 0 || d2_fx_touch_updated_us[1] != 0) {
        fprintf(stderr,
                "D2_FUNCTIONALITY_CONTRACT_TEST_FAILED fx-timeout-latched\n");
        free(pixels);
        return 1;
    }

    state.fx_touch_mask = 0x0f;
    d2_fx_touch_updated_us[1] = d2_monotonic_us() - 3000000ULL;
    d2_render_deck_fast(pixels, 1, &state, 0.0f, state.title,
                        220, 40, 40);
    if (state.fx_touch_mask != 0x0f || d2_fx_touch_updated_us[1] == 0) {
        fprintf(stderr,
                "D2_FUNCTIONALITY_CONTRACT_TEST_FAILED fx-settings-timeout\n");
        free(pixels);
        return 1;
    }

    state.fx_touch_mask = 1;
    d2_fx_touch_updated_us[1] = d2_monotonic_us();
    d2_led_state[1].fx_unit = 1;
    memset(pixels, 0, (size_t)WIDTH * HEIGHT * 2);
    d2_render_deck_fast(pixels, 1, &state, 0.0f, state.title,
                        220, 40, 40);
    size_t fx_unit_probe = ((size_t)116 * WIDTH + 192) * 2;
    uint8_t fx_unit_one_pixel[2];
    memcpy(fx_unit_one_pixel, &pixels[fx_unit_probe], 2);

    d2_parse_test_message("D2|1|LEDFXSEL|2");
    d2_fx_touch_updated_us[1] = d2_monotonic_us();
    memset(pixels, 0, (size_t)WIDTH * HEIGHT * 2);
    d2_render_deck_fast(pixels, 1, &state, 0.0f, state.title,
                        220, 40, 40);
    if (d2_led_state[1].fx_unit != 1 ||
        strcmp(d2_fx_unit_label(1), "FX UNIT 1") != 0 ||
        memcmp(fx_unit_one_pixel, &pixels[fx_unit_probe], 2) != 0) {
        fprintf(stderr,
                "D2_FUNCTIONALITY_CONTRACT_TEST_FAILED fx-unit-ownership\n");
        free(pixels);
        return 1;
    }
    state.fx_touch_mask = 0;
    d2_fx_touch_updated_us[1] = 0;

    d2_led_state[1].pad_rgb[0] = 0x042010U;
    d2_led_state[1].pad_rgb[1] = 0x00ff60U;
    if (!d2_is_performance_view(D2_VIEW_HOTCUE) ||
        !d2_is_performance_view(D2_VIEW_LOOP) ||
        !d2_is_performance_view(D2_VIEW_SAMPLER) ||
        !d2_is_performance_view(D2_VIEW_FREEZE) ||
        !d2_is_performance_view(D2_VIEW_BEATJUMP) ||
        d2_is_performance_view(D2_VIEW_DECK) ||
        d2_is_performance_view(D2_VIEW_BROWSE)) {
        fprintf(stderr,
                "D2_FUNCTIONALITY_CONTRACT_TEST_FAILED performance-dispatch\n");
        free(pixels);
        return 1;
    }
    for (int view = D2_VIEW_DECK; view <= D2_VIEW_BEATJUMP; ++view) {
        if (!d2_view_has_dedicated_compositor((enum d2_screen_view)view)) {
            fprintf(stderr,
                    "D2_FUNCTIONALITY_CONTRACT_TEST_FAILED view-compositor-%d\n",
                    view);
            free(pixels);
            return 1;
        }
    }
    if (d2_view_has_dedicated_compositor((enum d2_screen_view)-1) ||
        d2_view_has_dedicated_compositor((enum d2_screen_view)99)) {
        fprintf(stderr,
                "D2_FUNCTIONALITY_CONTRACT_TEST_FAILED invalid-view-compositor\n");
        free(pixels);
        return 1;
    }
    memset(pixels, 0, (size_t)WIDTH * HEIGHT * 2);
    d2_render_dynamic_performance_view(
        pixels, D2_VIEW_LOOP, state.title, d2_led_state[1].pad_rgb,
        220, 40, 40);
    uint8_t loop_idle_pixel[2];
    uint8_t loop_active_pixel[2];
    rgb565(loop_idle_pixel, 4, 32, 16);
    rgb565(loop_active_pixel, 0, 255, 96);
    size_t loop_idle_probe = ((size_t)84 * WIDTH + 30) * 2;
    size_t loop_active_probe = ((size_t)84 * WIDTH + 140) * 2;
    if (memcmp(&pixels[loop_idle_probe], loop_idle_pixel, 2) != 0 ||
        memcmp(&pixels[loop_active_probe], loop_active_pixel, 2) != 0 ||
        strcmp(d2_performance_heading(D2_VIEW_HOTCUE), "HOTCUE") != 0 ||
        strcmp(d2_performance_heading(D2_VIEW_SAMPLER), "SAMPLER") != 0 ||
        strcmp(d2_performance_instruction(D2_VIEW_HOTCUE),
               "PRESS PAD: HOT CUE") != 0 ||
        strcmp(d2_performance_instruction(D2_VIEW_SAMPLER),
               "PRESS PAD: PLAY SAMPLE") != 0 ||
        strcmp(d2_performance_pad_label(D2_VIEW_HOTCUE, 0), "1") != 0 ||
        strcmp(d2_performance_pad_label(D2_VIEW_SAMPLER, 7), "8") != 0 ||
        strcmp(d2_performance_pad_label(D2_VIEW_LOOP, 0), "1/4") != 0 ||
        strcmp(d2_performance_pad_label(D2_VIEW_FREEZE, 7), "32") != 0 ||
        strcmp(d2_performance_pad_label(D2_VIEW_BEATJUMP, 7), "+16") != 0) {
        fprintf(stderr,
                "D2_FUNCTIONALITY_CONTRACT_TEST_FAILED performance-feedback\n");
        free(pixels);
        return 1;
    }
    for (int pad = 0; pad < 8; ++pad) {
        int cell_x = 24 + (pad % 4) * 110;
        int cell_y = 80 + (pad / 4) * 64;
        int min_x, min_y, max_x, max_y;
        if (!d2_test_white_ink_bounds(pixels, cell_x + 2, cell_y + 2,
                                      94, 48, &min_x, &min_y,
                                      &max_x, &max_y) ||
            max_y - min_y + 1 < 14 ||
            abs((min_x + max_x) - (2 * cell_x + 97)) > 6 ||
            abs((min_y + max_y) - (2 * cell_y + 51)) > 6) {
            fprintf(stderr,
                    "D2_FUNCTIONALITY_CONTRACT_TEST_FAILED "
                    "performance-label-geometry pad=%d bounds=%d,%d-%d,%d\n",
                    pad + 1, min_x, min_y, max_x, max_y);
            free(pixels);
            return 1;
        }
    }

    d2_parse_test_message("D2|1|FXSEL2|17");
    if (d2_screen_state[1].fx_selection[1] != 17 ||
        d2_screen_state[2].fx_selection[1] != 0) {
        fprintf(stderr,
                "D2_FUNCTIONALITY_CONTRACT_TEST_FAILED fx-selection-parser\n");
        free(pixels);
        return 1;
    }
    d2_parse_test_message("D2|1|FXNAME2|U:Pitch%20Shift");
    if (strcmp(d2_screen_state[1].fx_name[1], "Pitch Shift") != 0 ||
        d2_screen_state[2].fx_name[1][0] != '\0') {
        fprintf(stderr,
                "D2_FUNCTIONALITY_CONTRACT_TEST_FAILED fx-name-parser\n");
        free(pixels);
        return 1;
    }
    char fx_label[96];
    d2_fx_slot_label(fx_label, sizeof(fx_label), &d2_screen_state[1], 1, 102);
    if (strstr(fx_label, "Pitch") == NULL ||
        d2_measure_text_width(fx_label, 1) > 102) {
        fprintf(stderr,
                "D2_FUNCTIONALITY_CONTRACT_TEST_FAILED fx-name-label\n");
        free(pixels);
        return 1;
    }
    d2_parse_test_message("D2|1|FXNAME2|U:%FF");
    if (strcmp(d2_screen_state[1].fx_name[1], "Pitch Shift") != 0) {
        fprintf(stderr,
                "D2_FUNCTIONALITY_CONTRACT_TEST_FAILED fx-name-transaction\n");
        free(pixels);
        return 1;
    }
    free(pixels);

    d2_parse_test_message("D2|1|PHASE|0,0,1,2,0");
    if (d2_screen_state[1].phase_valid) {
        fprintf(stderr, "D2_FUNCTIONALITY_CONTRACT_TEST_FAILED phase-invalid\n");
        return 1;
    }
    d2_parse_test_message("D2|1|PHASE|0,0,1,2,1,2,1");
    d2_parse_test_message("D2|1|GRIDEDIT|1");
    d2_parse_test_message("D2|1|CUECOLOR1|1193046");
    if (!d2_screen_state[1].phase_valid ||
        d2_screen_state[1].phase_master_step != 1 ||
        d2_screen_state[1].phase_active_step != 2 ||
        d2_screen_state[1].phase_master_deck != 2 ||
        d2_screen_state[1].phase_follower_deck != 1 ||
        !d2_screen_state[1].beatgrid_edit ||
        d2_screen_state[1].hotcue_color[0] != 0x123456U) {
        fprintf(stderr, "D2_FUNCTIONALITY_CONTRACT_TEST_FAILED parser\n");
        return 1;
    }

    d2_screen_state[1].fx_enabled[0] = 1;
    d2_screen_state[1].fx_enabled[1] = 0;
    d2_screen_state[1].fx_enabled[2] = 1;
    d2_led_state[1].active_channel = 1;
    d2_led_state[1].fx_assign_mask = 1;
    if (!d2_fx_overlay_active(1, &d2_screen_state[1], 0, 0) ||
        !d2_fx_overlay_active(1, &d2_screen_state[1], 1, 0) ||
        d2_fx_overlay_active(1, &d2_screen_state[1], 2, 0) ||
        !d2_fx_overlay_active(1, &d2_screen_state[1], 3, 0)) {
        fprintf(stderr, "D2_FUNCTIONALITY_CONTRACT_TEST_FAILED fx-alignment\n");
        return 1;
    }

    d2_parse_test_message(
        "D2|1|LEDPACK|0,0,0,0,0,0,1,1,1,0,0,0,0,0,0,0,0,0,0,0");
    if (!d2_led_state[1].outputs_enabled) {
        fprintf(stderr, "D2_FUNCTIONALITY_CONTRACT_TEST_FAILED led-enable\n");
        return 1;
    }
    d2_parse_test_message("D2|1|LEDOFF|1");
    if (d2_led_state[1].outputs_enabled) {
        fprintf(stderr, "D2_FUNCTIONALITY_CONTRACT_TEST_FAILED led-off\n");
        return 1;
    }

    printf("D2_FUNCTIONALITY_CONTRACT_TEST_OK\n");
    return 0;
}

int main(int argc, char **argv)
{
    signal(SIGINT, stop_handler);
    signal(SIGTERM, stop_handler);
    signal(SIGUSR1, capture_handler);
    d2_font_init();
    if (!d2_ft_ready)
        fprintf(stderr, "D2 font: FreeType unavailable; using bitmap fallback\n");
    if (argc == 3 && strcmp(argv[1], "--render-test") == 0) {
        int result = d2_render_test_image(argv[2], 0);
        d2_font_shutdown();
        return result;
    }
    if (argc == 3 && strcmp(argv[1], "--render-beatmap-test") == 0) {
        int result = d2_render_test_image(argv[2], 1);
        d2_font_shutdown();
        return result;
    }
    if (argc == 3 && strcmp(argv[1], "--render-browse-test") == 0) {
        int result = d2_render_browse_test_image(
            argv[2], 0, D2_BROWSE_NOTICE_NONE);
        d2_font_shutdown();
        return result;
    }
    if (argc == 3 && strcmp(argv[1], "--render-browse-sidebar-test") == 0) {
        int result = d2_render_browse_test_image(
            argv[2], 1, D2_BROWSE_NOTICE_NONE);
        d2_font_shutdown();
        return result;
    }
    if (argc == 3 && strcmp(argv[1], "--render-performance-test") == 0) {
        int result = d2_render_performance_test_image(argv[2]);
        d2_font_shutdown();
        return result;
    }
    if (argc == 3 && strcmp(argv[1], "--render-load-reject-test") == 0) {
        int result = d2_render_browse_test_image(
            argv[2], 0, D2_BROWSE_NOTICE_DECK_PLAYING);
        d2_font_shutdown();
        return result;
    }
    if (argc == 3 && strcmp(argv[1], "--render-load-missing-test") == 0) {
        int result = d2_render_browse_test_image(
            argv[2], 0, D2_BROWSE_NOTICE_TRACK_MISSING);
        d2_font_shutdown();
        return result;
    }
    if (argc == 3 && strcmp(argv[1], "--track-metadata-test") == 0) {
        int track_id = atoi(argv[2]);
        d2_begin_track_identity(1, track_id);
        struct d2_track_assets *assets = calloc(1, sizeof(*assets));
        if (!assets) {
            d2_font_shutdown();
            return 1;
        }
        d2_track_assets[1] = assets;
        d2_bind_asset_slot(1, assets);
        int result = d2_load_track_metadata(1, track_id, 0.0f);
        printf("D2_TRACK_METADATA_TEST_%s track_id=%d location=%s "
               "waveform=%d cover=%d beatgrid=%d beatmap=%d title=%s\n",
               result ? "OK" : "FAILED", track_id,
               d2_screen_state[1].location,
               d2_waveform_ready[1], d2_cover_art_ready[1],
               d2_screen_state[1].beatgrid_ready,
               d2_screen_state[1].beatmap_ready,
               d2_screen_state[1].title);
        d2_begin_track_identity(1, 0);
        d2_font_shutdown();
        return result ? 0 : 1;
    }
    if (argc == 3 && strcmp(argv[1], "--track-location-test") == 0) {
        int track_id = d2_resolve_track_id_by_location(argv[2]);
        d2_begin_track_identity(1, track_id);
        struct d2_track_assets *assets = calloc(1, sizeof(*assets));
        if (!assets) {
            d2_font_shutdown();
            return 1;
        }
        d2_track_assets[1] = assets;
        d2_bind_asset_slot(1, assets);
        int result = d2_load_track_metadata(1, track_id, 0.0f);
        printf("D2_TRACK_LOCATION_TEST_%s track_id=%d location=%s "
               "waveform=%d strip=%d title=%s\n",
               result ? "OK" : "FAILED", track_id,
               d2_screen_state[1].location,
               d2_waveform_ready[1], d2_wave_strip_ready[1],
               d2_screen_state[1].title);
        d2_begin_track_identity(1, 0);
        d2_font_shutdown();
        return result ? 0 : 1;
    }
    if (argc == 2 && strcmp(argv[1], "--beat-geometry-test") == 0) {
        int result = d2_beat_geometry_test();
        d2_font_shutdown();
        return result;
    }
    if (argc == 2 && strcmp(argv[1], "--load-reject-notice-test") == 0) {
        int result = d2_load_reject_notice_test();
        d2_font_shutdown();
        return result;
    }
    if (argc == 2 && strcmp(argv[1], "--functionality-contract-test") == 0) {
        int result = d2_functionality_contract_test();
        d2_font_shutdown();
        return result;
    }

    /* The producer renders at 60 Hz. USB consumes only the newest completed
     * full frame at 30 Hz, keeping two 480x272 displays comfortably below the
     * shared high-speed bus limit without building a stale-frame queue. */
    struct ctlra_create_opts_t opts = {
        .screen_redraw_target_fps = 30
    };

    struct ctlra_t *ctlra = ctlra_create(&opts);

    if (!ctlra) {
        fprintf(stderr, "ctlra_create failed\n");
        return 1;
    }

    if (midi_init() < 0) {
        fprintf(stderr, "MIDI initialization failed\n");
        ctlra_exit(ctlra);
        return 1;
    }

    printf("Starting D2 bridge...\n");

    int n = ctlra_probe(
        ctlra,
        accept_device,
        NULL);

    printf("Connected D2 devices: %d\n", n);

    printf("TOTAL D2 devices: %d\n", n);

    if (d2_track_loaders_start() != 0) {
        fprintf(stderr, "D2 track loader creation failed\n");
        midi_shutdown();
        ctlra_exit(ctlra);
        d2_browse_db_shutdown();
        d2_font_shutdown();
        return 1;
    }

    if (pthread_create(&d2_render_thread, NULL,
                       d2_render_thread_main, NULL) != 0) {
        fprintf(stderr, "D2 render thread creation failed\n");
        d2_track_loaders_shutdown();
        midi_shutdown();
        ctlra_exit(ctlra);
        d2_browse_db_shutdown();
        d2_font_shutdown();
        return 1;
    }
    d2_render_thread_started = 1;

    while (running) {
        /* Input polling has priority over rendering. event_callback() takes
         * the state lock only after libctlra has drained the USB interrupt
         * endpoint, so framebuffer work can no longer starve HID controls. */
        ctlra_idle_iter(ctlra);

        pthread_mutex_lock(&d2_state_mutex);
        midi_input_poll();
        pthread_mutex_unlock(&d2_state_mutex);
        d2_capture_write_requested_frames();
        /* ctlra_idle_iter() also invokes feedback_callback(), which flushes
         * the complete LED report for both D2 units.  Running this at 2 ms
         * produces roughly 1000 HID writes/second and starves the interrupt
         * IN endpoint.  10 ms keeps controls at 100 Hz while the independent
         * render producer and 30 Hz complete-frame USB path stay unchanged. */
        usleep(10000);
    }

    printf("Stopping D2 bridge...\n");

    if (d2_render_thread_started) {
        pthread_join(d2_render_thread, NULL);
        d2_render_thread_started = 0;
    }

    d2_track_loaders_shutdown();

    midi_shutdown();
    ctlra_exit(ctlra);
    d2_browse_db_shutdown();
    d2_font_shutdown();

    return 0;
}
