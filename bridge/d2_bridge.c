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
#define D2_VERBOSE 0
#define D2_EVENT_LOG(...) \
    do { if (D2_VERBOSE) fprintf(stdout, __VA_ARGS__); } while (0)
#define D2_EVENT_FLUSH() \
    do { if (D2_VERBOSE) fflush(stdout); } while (0)

static volatile int running = 1;
static snd_seq_t *midi_seq = NULL;
static int midi_port = -1;
static FT_Library d2_ft_library;
static FT_Face d2_ft_face;
static int d2_ft_ready = 0;

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
static struct d2_cached_glyph d2_font_cache[D2_FONT_SIZES][D2_FONT_CACHE_GLYPHS];

struct d2_screen_state {
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
    int phase_valid;
    float beat_prev_position;
    float beat_next_position;
    int beatgrid_valid;
    float beatgrid_first_position;
    float beatgrid_interval;
    int beatgrid_ready;
    float beatmap_position[D2_MAX_BEATS];
    int beatmap_count;
    int beatmap_ready;
    int zoom_level;
    int time_mode;
    float loop_size;
    int quantize;
    int keylock;
    int visual_key;
    int fx_touch_mask;
    float fx_parameter[4];
    int fx_enabled[4];
    int stem_count;
    float stem_volume[4];
    int stem_muted[4];
    int browse_focus; /* 0 = track list, 1 = library tree */
    float hotcue_position[8];
    int playing;
    int metadata_pending;
    struct timespec position_updated_at;
    char title[80];
    char artist[80];
    char musical_key[16];
};

static struct d2_screen_state d2_screen_state[3] = {
    {0},
    {.bpm = 128.0f, .position = 0.38f, .rate = 1.0f, .zoom_level = 2,
     .loop_size = 4.0f,
     .stem_volume = {1,1,1,1}, .title = "DECK 1", .artist = "MIXXX",
     .hotcue_position = {-1,-1,-1,-1,-1,-1,-1,-1}},
    {.bpm = 128.0f, .position = 0.38f, .rate = 1.0f, .zoom_level = 2,
     .loop_size = 4.0f,
     .stem_volume = {1,1,1,1}, .title = "DECK 2", .artist = "MIXXX",
     .hotcue_position = {-1,-1,-1,-1,-1,-1,-1,-1}},
};
static float d2_metadata_duration[3] = {0.0f, 0.0f, 0.0f};
/* FX touch is a transient overlay.  This timestamp is a safety net for
 * controllers that omit the note-off/release packet. */
static uint64_t d2_fx_touch_updated_us[3] = {0, 0, 0};

struct d2_led_state {
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
    {.active_channel = 1, .mode = 1, .fx_unit = 1,
     .performance_on = {1, 1, 1, 1}},
    {.active_channel = 2, .mode = 1, .fx_unit = 2,
     .performance_on = {1, 1, 1, 1}},
};

static uint64_t d2_monotonic_us(void)
{
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (uint64_t)ts.tv_sec * 1000000ULL +
           (uint64_t)ts.tv_nsec / 1000ULL;
}
/* Real amplitude summaries, generated from the loaded audio file. */
static uint8_t d2_waveform[3][D2_WAVEFORM_POINTS];
static uint8_t d2_waveform_low[3][D2_WAVEFORM_POINTS];
static uint8_t d2_waveform_mid[3][D2_WAVEFORM_POINTS];
static uint8_t d2_waveform_high[3][D2_WAVEFORM_POINTS];
static int d2_waveform_ready[3] = {0, 0, 0};
/* Pre-rasterized row-major RGB565 waveform layers. Each loaded deck owns a
 * 8192-pixel strip; a playing frame copies a 426-pixel window per row. */
static uint8_t d2_wave_strip[3][D2_WAVEFORM_HEIGHT]
                            [D2_WAVEFORM_POINTS * 2];
static int d2_wave_strip_ready[3] = {0, 0, 0};
static uint8_t d2_cover_art[3][48 * 48 * 2];
static int d2_cover_art_ready[3] = {0, 0, 0};
/* Two native RGB565 framebuffers per physical display. The compositor never
 * draws into a buffer that is being copied to libctlra's USB transfer frame. */
static uint8_t d2_render_buffer[3][2][WIDTH * HEIGHT * 2];
static unsigned d2_render_buffer_index[3] = {0, 0, 0};
static int d2_render_buffer_ready[3] = {0, 0, 0};
static uint64_t d2_render_generation[3] = {0, 0, 0};
static uint64_t d2_usb_generation[3] = {0, 0, 0};
static pthread_mutex_t d2_frame_mutex = PTHREAD_MUTEX_INITIALIZER;
static pthread_mutex_t d2_state_mutex = PTHREAD_MUTEX_INITIALIZER;
static pthread_t d2_render_thread;
static int d2_render_thread_started = 0;
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

static void d2_browse_mark_dirty(void)
{
    ++d2_browse_generation;
    if (d2_browse_generation == 0)
        d2_browse_generation = 1;
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
static const char *d2_pad_labels[8] = {
    "1", "2", "3", "4", "5", "6", "7", "8",
};

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
    if (deck < 1 || deck > 2 || !location || !location[0])
        return;
    d2_cover_art_ready[deck] = 0;
    memset(d2_cover_art[deck], 0, sizeof(d2_cover_art[deck]));
    if (!d2_shell_quote(quoted_location, sizeof(quoted_location), location))
        return;
    snprintf(command, sizeof(command),
             "ffmpeg -nostdin -v error -i %s -an "
             "-vf scale=48:48:force_original_aspect_ratio=decrease," 
             "pad=48:48:(ow-iw)/2:(oh-ih)/2 -frames:v 1 "
             "-f rawvideo -pix_fmt rgb565be - 2>/dev/null",
             quoted_location);
    pipe = popen(command, "r");
    if (!pipe)
        return;
    size_t bytes = fread(d2_cover_art[deck], 1,
                         sizeof(d2_cover_art[deck]), pipe);
    int status = pclose(pipe);
    if (status == 0 && bytes == sizeof(d2_cover_art[deck]))
        d2_cover_art_ready[deck] = 1;
}

/* Read Mixxx's own high-resolution, frequency-separated waveform cache.
 * The helper writes low/mid/high values as 2048 RGB triplets. */
static int d2_load_mixxx_waveform(int deck, int analysis_id)
{
    uint8_t raw[D2_WAVEFORM_POINTS][3];
    char command[256];
    FILE *pipe;

    if (deck < 1 || deck > 2 || analysis_id <= 0)
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
    char mode[8] = "";
    FILE *pipe;
    int first_frame = -1;
    double sample_rate = 0.0;
    double bpm = 0.0;

    if (deck < 1 || deck > 2 || track_id <= 0 || duration <= 1.0f)
        return;
    d2_screen_state[deck].beatgrid_ready = 0;
    d2_screen_state[deck].beatmap_ready = 0;
    d2_screen_state[deck].beatmap_count = 0;
    snprintf(command, sizeof(command),
             "/home/pi/openAV-Ctlra/build/d2_beatgrid_extract %d", track_id);
    pipe = popen(command, "r");
    if (!pipe)
        return;
    if (fscanf(pipe, "%7s", mode) != 1) {
        pclose(pipe);
        return;
    }
    if (strcmp(mode, "GRID") == 0 &&
        fscanf(pipe, "%d %lf %lf", &first_frame, &sample_rate, &bpm) == 3 &&
        first_frame >= 0 && sample_rate > 0.0 && bpm >= 20.0 && bpm <= 400.0) {
        float first_position = (float)(first_frame / (sample_rate * duration));
        float interval = 60.0f / ((float)bpm * duration);
        if (first_position >= -0.25f && first_position <= 1.0f &&
            interval > 0.0f && interval <= 1.0f) {
            d2_screen_state[deck].beatgrid_first_position = first_position;
            d2_screen_state[deck].beatgrid_interval = interval;
            d2_screen_state[deck].beatgrid_ready = 1;
            printf("D2 BEATGRID: deck=%d first=%.6f interval=%.8f bpm=%.3f\n",
                   deck, first_position, interval, bpm);
        }
    } else if (strcmp(mode, "MAP") == 0) {
        int source_count = 0;
        if (fscanf(pipe, "%d %lf", &source_count, &sample_rate) == 2 &&
            source_count > 0 && sample_rate > 0.0) {
            int stored = 0;
            for (int i = 0; i < source_count; ++i) {
                int frame = -1;
                if (fscanf(pipe, "%d", &frame) != 1)
                    break;
                if (frame >= 0 && stored < D2_MAX_BEATS)
                    d2_screen_state[deck].beatmap_position[stored++] =
                        (float)(frame / (sample_rate * duration));
            }
            d2_screen_state[deck].beatmap_count = stored;
            d2_screen_state[deck].beatmap_ready = stored > 1;
            printf("D2 BEATMAP: deck=%d beats=%d source=%d\n",
                   deck, stored, source_count);
        }
    }
    int exit_status = pclose(pipe);
    if (exit_status != 0) {
        d2_screen_state[deck].beatgrid_ready = 0;
        d2_screen_state[deck].beatmap_ready = 0;
        d2_screen_state[deck].beatmap_count = 0;
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

    if (deck < 1 || deck > 2 || !location || !*location || duration <= 0.0f)
        return;
    d2_waveform_ready[deck] = 0;
    memset(d2_waveform[deck], 0, sizeof(d2_waveform[deck]));
    /* A failed/unfinished Mixxx analysis must not inherit the previous
     * track's filtered bands.  Stale low/mid/high arrays were responsible
     * for artificial repeated shapes when the raw fallback was used. */
    memset(d2_waveform_low[deck], 0, sizeof(d2_waveform_low[deck]));
    memset(d2_waveform_mid[deck], 0, sizeof(d2_waveform_mid[deck]));
    memset(d2_waveform_high[deck], 0, sizeof(d2_waveform_high[deck]));
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

/*
 * Controller scripts expose numeric deck controls but not track strings.
 * The live position and remaining time yield the loaded track duration,
 * which uniquely identifies the correct library record for each deck.
 */
static void d2_load_duration_metadata(int deck, float duration)
{
    static const char sql[] =
        "SELECT l.artist, l.title, l.key, l.duration, tl.location, "
        "(SELECT id FROM track_analysis "
        " WHERE track_id = l.id AND type = '1' ORDER BY id DESC LIMIT 1) "
        ", l.id "
        "FROM library l "
        "JOIN track_locations tl ON tl.id = l.location "
        "WHERE l.mixxx_deleted = 0 "
        "ORDER BY ABS(l.duration - ?) ASC LIMIT 1";
    sqlite3 *db = NULL;
    sqlite3_stmt *stmt = NULL;

    if (sqlite3_open_v2("/home/pi/.mixxx/mixxxdb.sqlite", &db,
                        SQLITE_OPEN_READONLY, NULL) != SQLITE_OK) {
        fprintf(stderr, "D2 metadata: cannot open Mixxx database\\n");
        goto done;
    }
    if (sqlite3_prepare_v2(db, sql, -1, &stmt, NULL) != SQLITE_OK) {
        fprintf(stderr, "D2 metadata: query preparation failed: %s\\n",
                sqlite3_errmsg(db));
        goto done;
    }
    sqlite3_bind_double(stmt, 1, duration);
    if (sqlite3_step(stmt) == SQLITE_ROW) {
        const unsigned char *artist = sqlite3_column_text(stmt, 0);
        const unsigned char *title = sqlite3_column_text(stmt, 1);
        const unsigned char *musical_key = sqlite3_column_text(stmt, 2);
        float matched_duration = (float)sqlite3_column_double(stmt, 3);
        const unsigned char *location = sqlite3_column_text(stmt, 4);
        int waveform_analysis_id = sqlite3_column_int(stmt, 5);
        int track_id = sqlite3_column_int(stmt, 6);
        if (fabsf(matched_duration - duration) > 0.25f) {
            printf("D2 metadata: no duration match deck=%d duration=%.3f\\n",
                   deck, duration);
            fflush(stdout);
            goto done;
        }
        snprintf(d2_screen_state[deck].artist,
                 sizeof(d2_screen_state[deck].artist), "%s",
                 artist ? (const char *)artist : "");
        snprintf(d2_screen_state[deck].title,
                 sizeof(d2_screen_state[deck].title), "%s",
                 title ? (const char *)title : "DECK");
        snprintf(d2_screen_state[deck].musical_key,
                 sizeof(d2_screen_state[deck].musical_key), "%s",
                 musical_key ? (const char *)musical_key : "--");
        d2_metadata_duration[deck] = matched_duration;
        if (!d2_load_mixxx_waveform(deck, waveform_analysis_id)) {
            d2_load_real_waveform(deck,
                                  location ? (const char *)location : "",
                                  matched_duration);
        }
        if (d2_waveform_ready[deck])
            d2_build_wave_strip(deck);
        d2_load_cover_art(deck, location ? (const char *)location : "");
        d2_load_mixxx_beatgrid(deck, track_id, matched_duration);
        printf("D2 METADATA: deck=%d duration=%.3f artist=%s title=%s\\n",
               deck, matched_duration, d2_screen_state[deck].artist,
               d2_screen_state[deck].title);
        fflush(stdout);
    }

done:
    if (stmt)
        sqlite3_finalize(stmt);
    if (db)
        sqlite3_close(db);
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
    if (d2_browse_entries[row].track_id == track_id &&
        d2_browse_entries[row].title[0] != '\0' &&
        strcmp(d2_browse_entries[row].title, "LOADING...") != 0)
        return;

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
    } else if (strcmp(key, "LOAD") == 0) {
        /* A track-load event is the only normal path that may replace deck
         * metadata and waveform. Do not infer identity repeatedly from the
         * moving play position while a track is running. */
        float duration = strtof(value, NULL);
        if (duration > 1.0f) {
            d2_screen_state[deck].duration = duration;
            for (int cue = 0; cue < 8; ++cue)
                d2_screen_state[deck].hotcue_position[cue] = -1.0f;
            d2_screen_state[deck].beatgrid_ready = 0;
            d2_screen_state[deck].beatmap_ready = 0;
            d2_screen_state[deck].beatmap_count = 0;
            d2_metadata_duration[deck] = 0.0f;
            snprintf(d2_screen_state[deck].title,
                     sizeof(d2_screen_state[deck].title), "DECK %d", deck);
            snprintf(d2_screen_state[deck].artist,
                     sizeof(d2_screen_state[deck].artist), "MIXXX");
            d2_load_duration_metadata(deck, duration);
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
        if (playing && !d2_screen_state[deck].playing)
            d2_screen_state[deck].metadata_pending = 2;
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
        int parsed = sscanf(value, "%f,%f,%d,%d", &master_phase,
                            &active_phase, &master_step, &active_step);
        if (parsed >= 2 &&
            isfinite(master_phase) && isfinite(active_phase)) {
            master_phase -= floorf(master_phase);
            active_phase -= floorf(active_phase);
            if (master_phase < 0.0f) master_phase += 1.0f;
            if (active_phase < 0.0f) active_phase += 1.0f;
            d2_screen_state[deck].phase_master = master_phase;
            d2_screen_state[deck].phase_active = active_phase;
            if (parsed == 4) {
                d2_screen_state[deck].phase_master_step =
                    ((master_step % 4) + 4) % 4;
                d2_screen_state[deck].phase_active_step =
                    ((active_step % 4) + 4) % 4;
            }
            d2_screen_state[deck].phase_valid = 1;
        }
    } else if (strcmp(key, "LEDPACK") == 0) {
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
                if (parsed >= 1 && parsed <= 4)
                    d2_led_state[deck].fx_unit = parsed;
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
    } else if (strcmp(key, "LEDDECK") == 0) {
        int channel = atoi(value);
        if (channel >= 1 && channel <= 4)
            d2_led_state[deck].active_channel = channel;
    } else if (strcmp(key, "LEDMODE") == 0) {
        int mode = atoi(value);
        if (mode >= 1 && mode <= 5)
            d2_led_state[deck].mode = mode;
    } else if (strcmp(key, "LEDFXSEL") == 0) {
        int unit = atoi(value);
        if (unit >= 1 && unit <= 4)
            d2_led_state[deck].fx_unit = unit;
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
    } else if (strcmp(key, "FXTOUCH") == 0) {
        d2_screen_state[deck].fx_touch_mask = atoi(value) & 0x0f;
        d2_fx_touch_updated_us[deck] = d2_monotonic_us();
    } else if (strncmp(key, "FXEN", 4) == 0) {
        int slot = atoi(key + 4) - 1;
        if (slot >= 0 && slot < 4)
            d2_screen_state[deck].fx_enabled[slot] = atoi(value) != 0;
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
    } else if (strncmp(key, "CUE", 3) == 0) {
        int cue = atoi(key + 3) - 1;
        float cue_position = strtof(value, NULL);
        if (cue >= 0 && cue < 8 && cue_position >= -1.0f && cue_position <= 1.0f)
            d2_screen_state[deck].hotcue_position[cue] = cue_position;
    } else if (strcmp(key, "REMAIN") == 0) {
        d2_screen_state[deck].remaining = strtof(value, NULL);
        if (d2_screen_state[deck].remaining < 0.0f)
            d2_screen_state[deck].remaining = 0.0f;
        /* Mixxx now provides the exact track duration. Keep the old formula
         * only as a compatibility fallback; rounded POS/REMAIN packets made
         * it drift enough to repeatedly discard valid metadata. */
        float duration = d2_screen_state[deck].duration;
        if (duration <= 0.0f && d2_screen_state[deck].remaining > 0.0f &&
            d2_screen_state[deck].position < 0.999f)
            duration = d2_screen_state[deck].remaining /
                       (1.0f - d2_screen_state[deck].position);
        /* Compatibility fallback for old controller scripts that don't emit
         * LOAD. With the current script this executes only before init. */
        if (duration > 0.0f &&
            d2_screen_state[deck].metadata_pending == 0 &&
            strncmp(d2_screen_state[deck].title, "DECK", 4) == 0)
            d2_screen_state[deck].metadata_pending = 2;
        if (d2_screen_state[deck].metadata_pending > 0 &&
            --d2_screen_state[deck].metadata_pending == 0)
            d2_load_duration_metadata(deck, duration);
    } else if (strcmp(key, "TITLE") == 0) {
        snprintf(d2_screen_state[deck].title,
                sizeof(d2_screen_state[deck].title), "%s", value);
    } else if (strcmp(key, "ARTIST") == 0) {
        snprintf(d2_screen_state[deck].artist,
                sizeof(d2_screen_state[deck].artist), "%s", value);
    } else if (strncmp(key, "BROWSE", 6) == 0 &&
               key[6] >= '0' && key[6] <= '8' && key[7] == '\0') {
        d2_load_browse_metadata(key[6] - '0', atoi(value));
    } else if (strcmp(key, "VIEW") == 0) {
        d2_browse_mark_dirty();
        if (strcmp(value, "DECK") == 0)
            d2_screen_view[deck] = D2_VIEW_DECK;
        else if (strcmp(value, "BROWSE") == 0)
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
    if (deck < 1 || deck > 2 || !d2_waveform_ready[deck])
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

static int d2_text_pixel(const char *text, int x, int y,
        int origin_x, int origin_y, int scale)
{
    int local_x = x - origin_x;
    int local_y = y - origin_y;
    if (local_x < 0 || local_y < 0)
        return 0;

    int character = local_x / (6 * scale);
    int column = (local_x / scale) % 6;
    int row = local_y / scale;
    if (row >= 7 || column >= 5 ||
            character >= (int)strlen(text) || text[character] == '\0')
        return 0;

    unsigned char glyph = (unsigned char)text[character];
    if (glyph >= 'a' && glyph <= 'z')
        glyph = (unsigned char)(glyph - 'a' + 'A');

    return (d2_font[glyph][column] & (1u << row)) != 0;
}

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
    if (!state || !state->phase_valid)
        return;
    d2_fill_rect(pixels, 156, 22, 168, 25, 0, 0, 0);
    d2_draw_text(pixels, player == 1 ? "A" : "B", 145, 22, 1,
                 244, 150, 18);
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

static unsigned d2_browse_hash(const char *text)
{
    unsigned hash = 2166136261u;
    if (!text)
        return hash;
    for (const unsigned char *p = (const unsigned char *)text; *p; ++p)
        hash = (hash ^ *p) * 16777619u;
    return hash;
}

static void d2_draw_browse_art(uint8_t *pixels, int x, int y,
                               const struct d2_browse_entry *entry,
                               int selected)
{
    unsigned hash = d2_browse_hash(entry ? entry->title : "");
    int r = 48 + (int)(hash & 0x3f);
    int g = 38 + (int)((hash >> 7) & 0x4f);
    int b = 52 + (int)((hash >> 15) & 0x5f);
    d2_fill_rect(pixels, x, y, 23, 23, selected ? 255 : 32,
                 selected ? 170 : 38, selected ? 45 : 48);
    d2_fill_rect(pixels, x + 2, y + 2, 19, 19, r, g, b);
    d2_fill_rect(pixels, x + 3, y + 3, 17, 5, r / 2, g / 2, b / 2);
    d2_fill_rect(pixels, x + 3, y + 16, 17, 4, 8, 10, 14);
    char number[3] = {entry && entry->track_id ?
                      (char)('1' + (entry->track_id % 8)) : '-', '\0', '\0'};
    d2_draw_text(pixels, number, x + 8, y + 6, 1, 246, 248, 250);
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

    snprintf(context, sizeof(context), "%.40s",
             d2_library_browse.context[0] ?
             d2_library_browse.context : "ALL TRACKS");

    d2_fill_rect(pixels, 0, 0, WIDTH, HEIGHT, 3, 5, 7);
    d2_fill_rect(pixels, 0, 0, WIDTH, 26, 25, 29, 34);
    d2_fill_rect(pixels, 0, 25, WIDTH, 1, 0, 112, 138);
    d2_draw_text(pixels, "BROWSER >", 5, 5, 1, 188, 198, 208);
    d2_draw_text(pixels, sidebar_open ? "LIBRARY >" : "PLAYLIST >",
                 54, 5, 1, 205, 213, 220);
    d2_draw_text(pixels, context, 106, 5, 1, 76, 218, 235);
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
        d2_draw_text(pixels, "TURN: SELECT", 180, 256, 1, 190, 198, 207);
        d2_draw_text(pixels, player == 1 ? "PRESS: LOAD A" : "PRESS: LOAD B",
                     398, 256, 1, accent_r, accent_g, accent_b);
    }
}

static void d2_render_deck_fast(uint8_t *pixels, int player,
                                const struct d2_screen_state *state,
                                float position, const char *title,
                                int accent_r, int accent_g, int accent_b)
{
    char bpm[16], time_text[20], tempo[16], loop_text[8], header_title[44];
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

    snprintf(bpm, sizeof(bpm), "%.2f", state->bpm);
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
    } else {
        for (int local_x = 0; local_x < wave_width; ++local_x) {
            float source = strip_center +
                           (local_x - wave_width / 2) * source_step;
            if (source < 0.0f) source = 0.0f;
            if (source > D2_WAVEFORM_POINTS - 1) source = D2_WAVEFORM_POINTS - 1;
            int index = (int)source;
            int amplitude = d2_waveform_ready[player] ? d2_waveform[player][index] : 10;
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
            int x = 240 + (int)(((beat_position - position) *
                                (D2_WAVEFORM_POINTS - 1)) / source_step + 0.5f);
            if (x < wave_left || x >= wave_left + wave_width)
                continue;
            int is_bar = (beat % 4) == 0;
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
            int x = 240 + (int)(((beat_position - position) *
                                (D2_WAVEFORM_POINTS - 1)) / source_step + 0.5f);
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
    if (fx_touch_mask && d2_fx_touch_updated_us[player] != 0 &&
        d2_monotonic_us() - d2_fx_touch_updated_us[player] > 1500000ULL) {
        fx_touch_mask = 0;
        d2_fx_touch_updated_us[player] = 0;
    }
    if (fx_touch_mask) {
        static const char *fx_labels[4] = {"MIX", "FX 1", "FX 2", "FX 3"};
        d2_fill_rect(pixels, 7, 74, 466, 78, 8, 11, 15);
        for (int slot = 0; slot < 4; ++slot) {
            int x = 13 + slot * 115;
            int active = state->fx_enabled[slot] ||
                         (fx_touch_mask & (1 << slot));
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
    for (int x = 0; x < 470; ++x) {
        int index = x * D2_WAVEFORM_POINTS / 470;
        int height = d2_waveform_ready[player] ? d2_waveform[player][index] / 4 : 4;
        int r = 45, g = 175, b = 238;
        if (d2_waveform_ready[player] == 2) {
            r = 15 + d2_waveform_low[player][index] * 220 / 255;
            g = 35 + d2_waveform_mid[player][index] * 180 / 255;
            b = 70 + d2_waveform_high[player][index] * 185 / 255;
        }
        if (height > 10) height = 10;
        d2_fill_rect(pixels, 5 + x, 255 - height, 1, height * 2 + 1, r, g, b);
    }
    int overview = 5 + (int)(470.0f * position);
    d2_fill_rect(pixels, overview - 1, 240, 3, 31, 250, 250, 250);

    static const int cue_colors[8][3] = {
        {255, 238, 0}, {0, 144, 198}, {0, 244, 70}, {0, 144, 198},
        {0, 144, 198}, {0, 244, 70}, {0, 244, 70}, {0, 244, 70},
    };
    for (int cue = 0; cue < 8; ++cue) {
        float cue_position = state->hotcue_position[cue];
        if (cue_position < 0.0f || cue_position > 1.0f)
            continue;
        int cue_x = 5 + (int)(cue_position * 470.0f);
        d2_fill_rect(pixels, cue_x - 4, 236, 9, 8,
                     cue_colors[cue][0], cue_colors[cue][1], cue_colors[cue][2]);
        char cue_label[2] = {(char)('1' + cue), '\0'};
        d2_draw_text(pixels, cue_label, cue_x - 2, 235, 1, 0, 0, 0);
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
}

/* Render producer: always overwrite the non-published RGB565 buffer.  USB
 * never sees a partially drawn frame because publication is a single index
 * swap under d2_frame_mutex after the complete 480x272 image is ready. */
static void *d2_render_thread_main(void *userdata)
{
    (void)userdata;
    struct timespec deadline;
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
                snprintf(deck_title, sizeof(deck_title), "%s", state->title);
                char *mix_suffix = strpbrk(deck_title, "([{");
                if (mix_suffix)
                    *mix_suffix = '\0';
                size_t title_len = strlen(deck_title);
                while (title_len > 0 && deck_title[title_len - 1] == ' ')
                    deck_title[--title_len] = '\0';

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
    struct d2_screen_state *state =
        (player >= 1 && player <= 2) ? &d2_screen_state[player] : NULL;
    /* This value is deliberately evaluated once per rendered frame. */
    float displayed_position = d2_effective_position(state);
    char deck_title[80] = {0};
    if (state) {
        snprintf(deck_title, sizeof(deck_title), "%s", state->title);
        char *mix_suffix = strpbrk(deck_title, "([{");
        if (mix_suffix)
            *mix_suffix = '\0';
        size_t title_len = strlen(deck_title);
        while (title_len > 0 && deck_title[title_len - 1] == ' ')
            deck_title[--title_len] = '\0';
    }
    int progress_end = 20 + (int)(440.0f * displayed_position);
    char bpm_text[16];
    int bpm_tenths = (int)((state ? state->bpm : 0.0f) * 10.0f + 0.5f);
    snprintf(bpm_text, sizeof(bpm_text), "%d.%d", bpm_tenths / 10, bpm_tenths % 10);
    char remain_text[16];
    int remain_seconds = (int)((state ? state->remaining : 0.0f) + 0.5f);
    snprintf(remain_text, sizeof(remain_text), "%02d:%02d",
            remain_seconds / 60, remain_seconds % 60);
    enum d2_screen_view view =
        (player >= 1 && player <= 2) ? d2_screen_view[player] : D2_VIEW_DECK;
    const char *performance_heading =
        view == D2_VIEW_HOTCUE ? "HOTCUE" :
        view == D2_VIEW_LOOP ? "LOOP" :
        view == D2_VIEW_FREEZE ? "FREEZE" :
        view == D2_VIEW_BEATJUMP ? "BEATJUMP" : "SAMPLER";
    const char *performance_line_1 =
        view == D2_VIEW_BEATJUMP ? "-1  +1  -4  +4" :
        "1/4  1/2  1  2";
    const char *performance_line_2 =
        view == D2_VIEW_BEATJUMP ? "-8  +8  -16  +16" :
        "4  8  16  32";
    const char *performance_line_3 =
        view == D2_VIEW_BEATJUMP ? "PAD 1-8: BEAT JUMP" :
        view == D2_VIEW_FREEZE ? "HOLD PAD: BEAT ROLL" :
        "PAD 1-8: LOOP SIZE";

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
            d2_usb_generation[player] = d2_render_generation[player];
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
        if (d2_load_library_browse_state())
            d2_browse_mark_dirty();
        if (d2_browse_frame_valid[player] &&
            d2_browse_rendered_generation[player] == d2_browse_generation)
            return 0;
        unsigned back = d2_render_buffer_index[player] ^ 1u;
        uint8_t *back_buffer = d2_render_buffer[player][back];
        d2_render_browse_fast(back_buffer, player, state);
        memcpy(pixel_data, back_buffer, WIDTH * HEIGHT * 2);
        d2_render_buffer_index[player] = back;
        d2_browse_rendered_generation[player] = d2_browse_generation;
        d2_browse_frame_valid[player] = 1;
        return 1;
    }

    for (int y = 0; y < HEIGHT; y++) {
        for (int x = 0; x < WIDTH; x++) {

            int r = 8;
            int g = 8;
            int b = 10;

            /* Browse stays open after encoder touch and returns to Deck on Load. */
            if (player >= 1 && player <= 2 &&
                d2_screen_view[player] == D2_VIEW_BROWSE) {
                r = 12;
                g = 14;
                b = 18;

                if (y < 42) {
                    r = 21;
                    g = 23;
                    b = 29;
                }
                if (y >= 39 && y < 42) {
                    r = accent_r;
                    g = accent_g;
                    b = accent_b;
                }
                int browse_row = (y >= 48 && y < 228) ? (y - 48) / 36 : -1;
                if (x < 140 && y >= 42 && y < 240) {
                    r = 15;
                    g = 18;
                    b = 23;
                }
                if (x >= 140 && x < 478 && browse_row >= 0) {
                    int selected = (!state || !state->browse_focus) && browse_row == 2;
                    r = selected ? 32 : 18;
                    g = selected ? 38 : 20;
                    b = selected ? 48 : 26;
                }
                if (x == 0 || x == WIDTH - 1 || y == 0 || y == HEIGHT - 1 ||
                    x == 139 ||
                    (x >= 140 && x < 478 && y >= 48 && y <= 228 &&
                     ((y - 48) % 36 == 0 || y == 227))) {
                    r = accent_r;
                    g = accent_g;
                    b = accent_b;
                }
                if (x >= 7 && x < 133 && y >= 76 && y < 99 &&
                    state && state->browse_focus) {
                    r = accent_r / 3;
                    g = accent_g / 3;
                    b = accent_b / 3;
                }
                if (d2_text_pixel("BROWSE", x, y, 18, 9, 3) ||
                    d2_text_pixel("LIBRARY", x, y, 8, 48, 1) ||
                    d2_text_pixel("TRACKS", x, y, 18, 82, 1) ||
                    d2_text_pixel("PLAYLISTS", x, y, 18, 112, 1) ||
                    d2_text_pixel("CRATES", x, y, 18, 142, 1) ||
                    d2_text_pixel("FILES", x, y, 18, 172, 1) ||
                    d2_text_pixel("TITLE / ARTIST", x, y, 148, 48, 1) ||
                    d2_text_pixel("TURN SELECT  PRESS LOAD", x, y, 146, 246, 1)) {
                    r = 235;
                    g = 235;
                    b = 242;
                }
                for (int row = 0; row < 5; row++) {
                    if (x >= 148 && x < 472 &&
                        d2_text_pixel(d2_browse_entries[row].title, x, y,
                                      148, 59 + row * 36, 1)) {
                        int selected = (!state || !state->browse_focus) && row == 2;
                        r = selected ? 255 : 190;
                        g = selected ? 210 : 200;
                        b = selected ? 80 : 215;
                    }
                }
                if (browse_row == 2 && x >= 142 && x < 148 &&
                    d2_text_pixel(">", x, y, 142, 126, 1)) {
                    r = 255;
                    g = 210;
                    b = 80;
                }
                rgb565(&pixel_data[(y * WIDTH + x) * 2], r, g, b);
                continue;
            }

            if (view == D2_VIEW_HOTCUE || view == D2_VIEW_LOOP ||
                view == D2_VIEW_SAMPLER || view == D2_VIEW_FREEZE ||
                view == D2_VIEW_BEATJUMP) {
                r = 12;
                g = 14;
                b = 18;
                if (y < 62) {
                    r = 21;
                    g = 23;
                    b = 29;
                }
                if (y >= 59 && y < 62) {
                    r = accent_r;
                    g = accent_g;
                    b = accent_b;
                }
                if (view == D2_VIEW_HOTCUE || view == D2_VIEW_SAMPLER) {
                    int grid_x = x - 20;
                    int grid_y = y - 76;
                    if (grid_x >= 0 && grid_x < 440 &&
                        grid_y >= 0 && grid_y < 128) {
                        int column = grid_x / 110;
                        int row = grid_y / 64;
                        int cell_x = grid_x % 110;
                        int cell_y = grid_y % 64;
                        if (column < 4 && row < 2 &&
                            cell_x >= 4 && cell_x < 102 &&
                            cell_y >= 4 && cell_y < 56) {
                            int pad = row * 4 + column;
                            uint32_t pad_rgb = d2_led_state[player].pad_rgb[pad];
                            r = (int)((pad_rgb >> 16) & 0xff);
                            g = (int)((pad_rgb >> 8) & 0xff);
                            b = (int)(pad_rgb & 0xff);
                            if (r + g + b < 18) r = g = b = 10;
                            if (d2_text_pixel(d2_pad_labels[pad], x, y,
                                              65 + column * 110,
                                              94 + row * 64, 3)) {
                                r = 250;
                                g = 250;
                                b = 255;
                            }
                        }
                    }
                } else {
                    if (x >= 30 && x < 450 && y >= 84 && y < 120) {
                        r = 55;
                        g = 90;
                        b = 180;
                    }
                    if (x >= 30 && x < 450 && y >= 132 && y < 168) {
                        r = 80;
                        g = 130;
                        b = 205;
                    }
                    if (x >= 30 && x < 450 && y >= 180 && y < 216) {
                        r = 45;
                        g = 50;
                        b = 62;
                    }
                    if (d2_text_pixel(performance_line_1, x, y, 52, 95, 2) ||
                        d2_text_pixel(performance_line_2, x, y, 52, 143, 2) ||
                        d2_text_pixel(performance_line_3, x, y, 52, 191, 2)) {
                        r = 250;
                        g = 250;
                        b = 255;
                    }
                }
                if (x == 0 || x == WIDTH - 1 || y == 0 || y == HEIGHT - 1) {
                    r = accent_r;
                    g = accent_g;
                    b = accent_b;
                }
                if (d2_text_pixel(performance_heading, x, y, 18, 10, 3)) {
                    r = 240;
                    g = 240;
                    b = 245;
                }
                if (state && d2_text_pixel(deck_title, x, y, 18, 42, 1)) {
                    r = 180;
                    g = 190;
                    b = 205;
                }
                if (d2_text_pixel("DECK TO EXIT", x, y, 20, 240, 1)) {
                    r = 150;
                    g = 155;
                    b = 165;
                }
                rgb565(&pixel_data[(y * WIDTH + x) * 2], r, g, b);
                continue;
            }

            /*
             * Track-deck layout inspired by the native Kontrol D2 display:
             * title header, beat-grid waveform, transport readouts and a
             * compact stripe waveform. Browse and performance views above
             * remain separate screen modes.
             */
            if (view == D2_VIEW_DECK) {
                unsigned wave_seed = 2166136261u;
                const char *seed_text = state ? state->title : "MIXXX";
                for (const char *p = seed_text; *p; ++p)
                    wave_seed = (wave_seed ^ (unsigned char)*p) * 16777619u;
                int waveform_center = 91;
                char player_text[16];
                char track_text[16];
                snprintf(player_text, sizeof(player_text), "PLAYER %d", player);
                snprintf(track_text, sizeof(track_text), "TRACK %02d", player);

                r = 6;
                g = 10;
                b = 15;

                /* Top title band and player identity chip. */
                if (y < 29) {
                    r = 17;
                    g = 29;
                    b = 43;
                }
                if (x >= 444 && y >= 5 && y < 22) {
                    r = accent_r;
                    g = accent_g;
                    b = accent_b;
                }
                if (y == 28) {
                    r = 110;
                    g = 160;
                    b = 200;
                }

                /* Loop-mode area and large scrolling waveform panel. */
                if (x < 50 && y >= 34 && y < 128) {
                    r = 19;
                    g = 33;
                    b = 47;
                }
                if (x >= 52 && x < 478 && y >= 38 && y < 132) {
                    r = 7;
                    g = 23;
                    b = 38;
                    int beat_x = x - 52;
                    if (beat_x % 43 == 0 || beat_x % 43 == 1) {
                        r = 42;
                        g = 87;
                        b = 117;
                    }
                    if (beat_x % 172 == 0 || beat_x % 172 == 1) {
                        r = 105;
                        g = 170;
                        b = 205;
                    }

                    int amplitude;
                    unsigned n0 = 0;
                    int waveform_index = 0;
                    if (d2_waveform_ready[player]) {
                        /* A dense, fractional viewport follows the current
                         * playback sample.  Linear interpolation removes the
                         * 1-pixel bar pattern from the old renderer. */
                        float source = displayed_position *
                                       (D2_WAVEFORM_POINTS - 1) +
                                       (beat_x - 213) * 2.15f;
                        if (source < 0.0f) source = 0.0f;
                        if (source > D2_WAVEFORM_POINTS - 1)
                            source = D2_WAVEFORM_POINTS - 1;
                        waveform_index = (int)source;
                        int next_index = waveform_index + 1;
                        if (next_index >= D2_WAVEFORM_POINTS)
                            next_index = D2_WAVEFORM_POINTS - 1;
                        float fraction = source - waveform_index;
                        amplitude = (int)(d2_waveform[player][waveform_index] *
                                          (1.0f - fraction) +
                                          d2_waveform[player][next_index] * fraction);
                        n0 = wave_seed + (unsigned)(waveform_index * 747796405u);
                        n0 ^= n0 >> 15;
                    } else {
                        /* Fallback only while a newly loaded file is still
                         * being decoded into its real waveform summary. */
                        int sample_x = beat_x + (int)(displayed_position * 960.0f);
                        int cell = sample_x / 8;
                        int fraction = sample_x & 7;
                        n0 = wave_seed + (unsigned)(cell * 747796405u);
                        unsigned n1 = wave_seed +
                                      (unsigned)((cell + 1) * 747796405u);
                        n0 ^= n0 >> 15;
                        n1 ^= n1 >> 15;
                        int macro = cell % 31;
                        if (macro < 0) macro += 31;
                        int energy = macro < 16 ? macro : 31 - macro;
                        int a0 = 8 + (int)(n0 % 24u) + energy;
                        int a1 = 8 + (int)(n1 % 24u) + energy;
                        amplitude = a0 + ((a1 - a0) * fraction) / 8;
                    }
                    int distance = y - waveform_center;
                    if (distance < 0) distance = -distance;
                    if (distance <= amplitude) {
                        if (d2_waveform_ready[player] == 2) {
                            /* Mixxx's low/mid/high analysis bands provide
                             * the same track-specific colour variation as
                             * the main Mixxx waveform. */
                            int low = d2_waveform_low[player][waveform_index];
                            int mid = d2_waveform_mid[player][waveform_index];
                            int high = d2_waveform_high[player][waveform_index];
                            r = 15 + low * 220 / 255 + mid * 25 / 255;
                            g = 35 + low * 120 / 255 + mid * 125 / 255;
                            b = 70 + mid * 80 / 255 + high * 165 / 255;
                            if (r > 255) r = 255;
                            if (g > 255) g = 255;
                            if (b > 255) b = 255;
                        } else {
                            r = 20 + (int)(n0 % 18u);
                            g = 125 + (int)(n0 % 62u);
                            b = 218 + (int)(n0 % 30u);
                        }
                    }
                }
                /* Fixed red playhead like the native D2 display. */
                if (x >= 238 && x <= 241 && y >= 32 && y < 136) {
                    r = 248;
                    g = 82;
                    b = 76;
                }
                if (y == waveform_center && x >= 52 && x < 478 &&
                    x != 238 && x != 239 && x != 240 && x != 241) {
                    r = 45;
                    g = 90;
                    b = 112;
                }

                /* Lower transport-information section. */
                if (y >= 137 && y < 191) {
                    r = 10;
                    g = 15;
                    b = 21;
                }
                if (y == 136 || y == 191) {
                    r = 47;
                    g = 93;
                    b = 126;
                }

                /* Overview/stripe waveform and timeline. */
                if (x >= 18 && x < 462 && y >= 201 && y < 224) {
                    r = 8;
                    g = 26;
                    b = 38;
                    int stripe_x = x - 18;
                    int stripe_height;
                    if (d2_waveform_ready[player]) {
                        int waveform_index = stripe_x * D2_WAVEFORM_POINTS / 444;
                        stripe_height = d2_waveform[player][waveform_index] / 4;
                    } else {
                        int stripe_cell = stripe_x / 6;
                        unsigned n = wave_seed +
                                     (unsigned)(stripe_cell * 747796405u);
                        n ^= n >> 15;
                        stripe_height = 3 + (int)(n % 12u);
                    }
                    int stripe_distance = y - 212;
                    if (stripe_distance < 0) stripe_distance = -stripe_distance;
                    if (stripe_distance <= stripe_height) {
                        r = 45;
                        g = 175;
                        b = 238;
                    }
                }
                /* A thin playhead preserves the true overview waveform;
                 * never fill the already-played area with a solid block. */
                int overview_playhead = 18 + (int)(444.0f *
                    displayed_position);
                if (x >= overview_playhead - 1 && x <= overview_playhead + 1 &&
                    y >= 198 && y < 229) {
                    r = 250;
                    g = 224;
                    b = 90;
                }
                if (y >= 226 && y < 229 && x >= 18 && x < 462) {
                    r = 27;
                    g = 53;
                    b = 70;
                }
                if (x >= 18 && x < 18 + (int)(444.0f *
                    displayed_position) && y >= 226 && y < 229) {
                    r = accent_r;
                    g = accent_g;
                    b = accent_b;
                }

                /* Footer controls. */
                if (y >= 236) {
                    r = 13;
                    g = 20;
                    b = 28;
                }
                if (x == 0 || x == WIDTH - 1 || y == 0 || y == HEIGHT - 1) {
                    r = accent_r;
                    g = accent_g;
                    b = accent_b;
                }

                if (state && d2_text_pixel(deck_title, x, y, 18, 7, 2)) {
                    r = 238; g = 244; b = 248;
                }
                if (d2_text_pixel(player_text, x, y, 448, 9, 1)) {
                    r = 8; g = 12; b = 18;
                }
                if (d2_text_pixel("LOOP", x, y, 8, 42, 1) ||
                    d2_text_pixel("MODE", x, y, 8, 53, 1) ||
                    d2_text_pixel("1 BEAT", x, y, 58, 47, 1) ||
                    d2_text_pixel("4 BEAT", x, y, 302, 47, 1)) {
                    r = 218; g = 231; b = 240;
                }
                if (d2_text_pixel(player_text, x, y, 12, 143, 1) ||
                    d2_text_pixel(track_text, x, y, 64, 143, 1) ||
                    d2_text_pixel("REMAIN", x, y, 146, 143, 1) ||
                    d2_text_pixel("TEMPO", x, y, 312, 143, 1) ||
                    d2_text_pixel("BPM", x, y, 421, 143, 1)) {
                    r = 139; g = 180; b = 205;
                }
                if (d2_text_pixel(player == 1 ? "1" : "2", x, y, 22, 157, 3) ||
                    d2_text_pixel(player == 1 ? "01" : "02", x, y, 73, 157, 3) ||
                    d2_text_pixel(remain_text, x, y, 143, 157, 3) ||
                    d2_text_pixel("+00.00", x, y, 307, 158, 2) ||
                    d2_text_pixel(bpm_text, x, y, 417, 157, 3)) {
                    r = 240; g = 247; b = 250;
                }
                if (d2_text_pixel(state && state->playing ? "PLAYING" : "PAUSED",
                                  x, y, 18, 180, 1) ||
                    d2_text_pixel("NEEDLE COUNT DOWN", x, y, 165, 231, 1) ||
                    d2_text_pixel("QUANTIZE", x, y, 7, 244, 1) ||
                    d2_text_pixel("1/2", x, y, 17, 256, 2) ||
                    d2_text_pixel("LOOP", x, y, 410, 244, 1) ||
                    d2_text_pixel("1/32", x, y, 403, 256, 2)) {
                    r = state && state->playing ? 85 : 170;
                    g = state && state->playing ? 235 : 185;
                    b = state && state->playing ? 135 : 195;
                }
                rgb565(&pixel_data[(y * WIDTH + x) * 2], r, g, b);
                continue;
            }

            /*
             * Header
             */
            if (y < 76) {
                r = 18;
                g = 18;
                b = 22;
            }

            /*
             * Accent line under header
             */
            if (y >= 73 && y < 76) {
                r = accent_r;
                g = accent_g;
                b = accent_b;
            }

            /*
             * Main waveform area
             */
            if (x >= 18 && x < 462 &&
                y >= 78 && y < 172) {

                r = 15;
                g = 15;
                b = 18;
            }

            /*
             * Fake waveform.
             * Deterministic bars, just for UI testing.
             */
            if (x >= 22 && x < 458 &&
                y >= 82 && y < 168) {

                int center = 125;

                /*
                 * Create a repeating waveform shape.
                 */
                int phase = (x * 7) % 64;
                int height = 8 + ((phase < 32) ? phase : 64 - phase);

                if (y >= center - height &&
                    y <= center + height) {

                    r = accent_r;
                    g = accent_g;
                    b = accent_b;
                }
            }

            /*
             * Waveform center line
             */
            if (y == 125) {
                r = 80;
                g = 80;
                b = 85;
            }

            /*
             * Progress bar background
             */
            if (x >= 20 && x < 460 &&
                y >= 188 && y < 196) {

                r = 35;
                g = 35;
                b = 40;
            }

            /* Progress is fed by D2|deck|POS|0.0..1.0 SysEx. */
            if (x >= 20 && x < progress_end &&
                y >= 188 && y < 196) {

                r = accent_r;
                g = accent_g;
                b = accent_b;
            }

            /*
             * Play indicator area
             */
            if (x >= 185 && x < 295 &&
                y >= 213 && y < 258) {

                r = 18;
                g = 18;
                b = 22;
            }

            /*
             * PLAY triangle
             */
            int px = x - 215;
            int py = y - 221;

            if (px >= 0 && px < 30 &&
                py >= 0 && py < 30) {

                int triangle_width =
                    2 * py;

                if (px >= 5 &&
                    px <= triangle_width &&
                    py < 15) {

                    if (state && state->playing) {
                        r = 80;
                        g = 255;
                        b = 100;
                    } else {
                        r = 70;
                        g = 70;
                        b = 75;
                    }
                }
            }

            /*
             * Cue indicator
             */
            if ((x >= 315 && x < 335) &&
                (y >= 225 && y < 245)) {

                r = 255;
                g = 190;
                b = 30;
            }

            /*
             * Sync indicator
             */
            if ((x >= 355 && x < 375) &&
                (y >= 225 && y < 245)) {

                r = 80;
                g = 180;
                b = 255;
            }

            /*
             * Outer border
             */
            if (x == 0 || x == WIDTH - 1 ||
                y == 0 || y == HEIGHT - 1) {

                r = accent_r;
                g = accent_g;
                b = accent_b;
            }

            /* Large live BPM readout in the header. */
            if (d2_text_pixel("BPM", x, y, 18, 15, 2)) {
                r = 145;
                g = 150;
                b = 160;
            }
            if (d2_text_pixel(bpm_text, x, y, 70, 7, 4)) {
                r = 240;
                g = 240;
                b = 245;
            }

            /* Large, stable main title; detailed mix/version stays in Browse. */
            if (state && x >= 18 && x < 460 &&
                d2_text_pixel(deck_title, x, y, 18, 45, 2)) {
                r = 235;
                g = 235;
                b = 240;
            }
            if (state && d2_text_pixel(state->artist, x, y, 20, 63, 1)) {
                r = 150;
                g = 155;
                b = 165;
            }

            if (d2_text_pixel("REMAIN", x, y, 20, 216, 1)) {
                r = 145;
                g = 150;
                b = 160;
            }
            if (d2_text_pixel(remain_text, x, y, 20, 229, 3)) {
                r = 240;
                g = 240;
                b = 245;
            }

            /* A clear, unambiguous state label beneath the transport triangle. */
            if (d2_text_pixel(state && state->playing ? "PLAYING" : "PAUSED",
                              x, y, 198, 244, 2)) {
                if (state && state->playing) {
                    r = 80;
                    g = 255;
                    b = 100;
                } else {
                    r = 105;
                    g = 105;
                    b = 112;
                }
            }

            rgb565(
                &pixel_data[(y * WIDTH + x) * 2],
                r, g, b);
        }
    }

    return 1;
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
             * Press is mapped to MIDI Note 62 so Mixxx can
             * use it as LoadSelectedTrack for the corresponding deck.
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

                if (player >= 1 && player <= 2 && e->button.pressed) {
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
                }

                midi_note(
                    midi_channel,
                    62,
                    e->button.pressed ? 127 : 0);

                D2_EVENT_LOG(
                    "BROWSE PRESS PLAYER %d CH %d NOTE 62 %s\\n",
                    player,
                    midi_channel + 1,
                    e->button.pressed ? "ON" : "OFF");

            } else if (e->button.id == 27) {

                /* Back changes Library focus between the tree and track list.
                 * It deliberately keeps both the D2 and touchscreen in Browse. */
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

                /*
                 * D2 Browse encoder (id=4):
                 * suppress only extremely-close duplicate events.
                 *
                 * Measurements showed duplicate events as close as
                 * 0.3 ms for one physical detent.
                 * Use 30 ms: one physical detent can produce several reports
                 * spaced farther apart than the old 5 ms filter.
                 */
                if (e->encoder.id == 4) {
                    static int browse_last_delta[2] = {0, 0};
                    static uint64_t browse_last_us[2] = {0, 0};

                    int pidx = player - 1;

                    struct timespec ts;
                    clock_gettime(CLOCK_MONOTONIC, &ts);

                    uint64_t now_us =
                        ((uint64_t)ts.tv_sec * 1000000ULL) +
                        ((uint64_t)ts.tv_nsec / 1000ULL);

                    D2_EVENT_LOG(
                        "BROWSE DEBUG PLAYER %d delta=%d last_delta=%d now_us=%llu last_us=%llu diff=%llu\\n",
                        player,
                        delta,
                        browse_last_delta[pidx],
                        (unsigned long long)now_us,
                        (unsigned long long)browse_last_us[pidx],
                        (unsigned long long)(now_us - browse_last_us[pidx]));

                    if (browse_last_delta[pidx] == delta &&
                        (now_us - browse_last_us[pidx]) < 30000ULL) {

                        D2_EVENT_LOG(
                            "BROWSE 30MS FILTER PLAYER %d delta=%d suppressed (%lluus)\\n",
                            player,
                            delta,
                            (unsigned long long)(now_us - browse_last_us[pidx]));

                        browse_last_us[pidx] = now_us;
                        continue;
                    }

                    browse_last_delta[pidx] = delta;
                    browse_last_us[pidx] = now_us;
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

                midi_cc(
                    midi_channel,
                    cc,
                    value);

                D2_EVENT_LOG(
                    "MIDI PLAYER %d CH %d ENCODER id=%u CC=%d VALUE=%d delta=%d\n",
                    player,
                    midi_channel + 1,
                    e->encoder.id,
                    cc,
                    value,
                    delta);
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

    for (uint32_t i = 0; i < NI_KONTROL_D2_LED_COUNT; i++)
        ctlra_dev_light_set(dev, i, off);

    for (int pad = 0; pad < 8; ++pad) {
        uint32_t rgb = led->pad_rgb[pad] & 0x00ffffffU;
        ctlra_dev_light_set(dev, NI_KONTROL_D2_LED_PAD_1 + pad,
                            rgb ? (0x7f000000U | rgb) : off);
    }

    ctlra_dev_light_set(dev, NI_KONTROL_D2_LED_FX_SELECT, bright);
    for (int fx = 0; fx < 4; ++fx)
        ctlra_dev_light_set(dev, NI_KONTROL_D2_LED_FX_1 + fx,
                            led->fx_enabled[fx] ? bright : dim);

    ctlra_dev_light_set(dev, NI_KONTROL_D2_LED_SCREEN_LEFT_1, bright);
    ctlra_dev_light_set(dev, NI_KONTROL_D2_LED_SCREEN_LEFT_2,
                        screen->keylock ? bright : dim);
    ctlra_dev_light_set(dev, NI_KONTROL_D2_LED_SCREEN_LEFT_3, dim);
    ctlra_dev_light_set(dev, NI_KONTROL_D2_LED_SCREEN_LEFT_4,
                        screen->time_mode ? bright : dim);
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
    ctlra_dev_light_set(dev, NI_KONTROL_D2_LED_EDIT, dim);
    for (int strip = 0; strip < 4; ++strip)
        ctlra_dev_light_set(dev, NI_KONTROL_D2_LED_ON_1 + strip,
                            led->performance_on[strip] ? bright : dim);

    ctlra_dev_light_set(dev, NI_KONTROL_D2_LED_HOTCUE,
                        led->mode == 1 ? blue_active : dim);
    ctlra_dev_light_set(dev, NI_KONTROL_D2_LED_LOOP,
                        led->mode == 2 ? blue_active : dim);
    ctlra_dev_light_set(dev, NI_KONTROL_D2_LED_FREEZE,
                        led->mode == 3 || led->mode == 5 ? blue_active : dim);
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
    ctlra_dev_light_flush(dev, 1);
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

        printf("D2 PLAYER MAP: dev=%p -> Player %d\n",
               (void *)dev,
               d2_map[d2_map_count].player);
        fflush(stdout);

        d2_map_count++;
    }

    ctlra_dev_set_event_func(dev, event_callback);
    ctlra_dev_set_feedback_func(dev, feedback_callback);
    ctlra_dev_set_screen_feedback_func(dev, screen_callback);

    return 1;
}


static int d2_render_test_image(const char *path)
{
    uint8_t *pixels = calloc((size_t)WIDTH * HEIGHT * 2, 1);
    if (!pixels)
        return 1;
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
        .phase_master_step = 0, .phase_active_step = 1, .phase_valid = 1,
        .visual_key = 20, .loop_size = 16.0f, .quantize = 1, .keylock = 1,
        .beatgrid_first_position = 0.001f,
        .beatgrid_interval = 60.0f / (128.0f * 420.0f),
        .beatgrid_ready = 1, .playing = 1,
        .title = "HELEMAAL NAAR DE KLOTE (ALVARO REMIX)",
        .artist = "NEXUS D2 PLAYER", .musical_key = "4A",
        .hotcue_position = {0.03f, 0.18f, 0.42f, 0.58f, 0.76f, -1, -1, -1},
    };
    d2_led_state[1].loop = 1;
    d2_led_state[1].sync = 1;
    d2_render_deck_fast(pixels, 1, &state, state.position, state.title,
                        0, 150, 210);
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

static int d2_render_browse_test_image(const char *path, int browse_focus)
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
    }
    struct d2_screen_state state = {.browse_focus = browse_focus};
    d2_render_browse_fast(pixels, 1, &state);
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

int main(int argc, char **argv)
{
    signal(SIGINT, stop_handler);
    signal(SIGTERM, stop_handler);
    d2_font_init();
    if (!d2_ft_ready)
        fprintf(stderr, "D2 font: FreeType unavailable; using bitmap fallback\n");
    if (argc == 3 && strcmp(argv[1], "--render-test") == 0) {
        int result = d2_render_test_image(argv[2]);
        d2_font_shutdown();
        return result;
    }
    if (argc == 3 && strcmp(argv[1], "--render-browse-test") == 0) {
        int result = d2_render_browse_test_image(argv[2], 0);
        d2_font_shutdown();
        return result;
    }
    if (argc == 3 && strcmp(argv[1], "--render-browse-sidebar-test") == 0) {
        int result = d2_render_browse_test_image(argv[2], 1);
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

    if (pthread_create(&d2_render_thread, NULL,
                       d2_render_thread_main, NULL) != 0) {
        fprintf(stderr, "D2 render thread creation failed\n");
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

    midi_shutdown();
    ctlra_exit(ctlra);
    d2_browse_db_shutdown();
    d2_font_shutdown();

    return 0;
}
