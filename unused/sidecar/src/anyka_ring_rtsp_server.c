#define _GNU_SOURCE
#define _POSIX_C_SOURCE 200809L

#include <arpa/inet.h>
#include <ctype.h>
#include <errno.h>
#include <inttypes.h>
#include <netinet/in.h>
#include <signal.h>
#include <stdarg.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <time.h>
#include <unistd.h>

#define HEADER_SIZE 0x6000u
#define ENTRY_START 0x4E8u
#define ENTRY_SIZE 0x18u
#define FRAME_HEADER_SIZE 36u
#define RTP_MAX_PAYLOAD 1200u
#define DEFAULT_RING_PATH "/tmp/VideoMainStream0"
#define DEFAULT_PORT 8554
#define MAX_TABLE_ENTRIES ((HEADER_SIZE - ENTRY_START) / ENTRY_SIZE)
#define VIDEO_TYPE 129u
#define AUDIO_TYPE 130u
#define VIDEO_TRACK_ID 0
#define AUDIO_TRACK_ID 1
#define VIDEO_PAYLOAD_TYPE 96u
#define AUDIO_PAYLOAD_TYPE 97u
#define VIDEO_CLOCK_RATE 90000u
#define VIDEO_PREFIX_SCAN_LIMIT 256u
#define AUDIO_PREFIX_SCAN_LIMIT 80u
#define LIVE_STABILITY_DELAY_MS 250u

typedef struct {
    uint8_t *data;
    size_t size;
    size_t payload_size;
} ring_snapshot_t;

typedef struct {
    uint64_t ts_ms;
    uint32_t logical_offset;
    uint32_t total_len;
    uint32_t type;
    uint32_t seq;
} ring_entry_t;

typedef struct {
    const char *ring_path;
    int port;
    int verbose;
    int static_replay;
    int loop_forever;
    size_t video_structured_skip;
} server_config_t;

typedef struct {
    int present;
    unsigned object_type;
    unsigned sample_rate;
    unsigned sample_index;
    unsigned channels;
    char config_hex[16];
} aac_config_t;

typedef struct {
    uint8_t *data;
    size_t len;
} hevc_nal_t;

typedef struct {
    size_t end_offset;
    uint64_t ts_ms;
} hevc_pending_segment_t;

typedef struct {
    uint8_t *pending;
    size_t pending_len;
    size_t pending_cap;
    hevc_pending_segment_t *segments;
    size_t segment_count;
    size_t segment_cap;
    hevc_nal_t *prefix_nals;
    size_t prefix_count;
    size_t prefix_cap;
    hevc_nal_t *current_au_nals;
    size_t current_au_count;
    size_t current_au_cap;
    uint64_t current_au_ts_ms;
    int current_au_active;
    int synced;
    int have_vps;
    int have_sps;
    int have_pps;
} hevc_stream_t;

typedef struct {
    uint16_t rtp_seq;
    uint32_t rtp_ssrc;
    uint32_t rtp_ts_base;
    uint64_t media_ts_base_ms;
    uint8_t rtp_channel;
    uint8_t rtcp_channel;
    int setup_done;
} rtp_track_state_t;

typedef struct {
    int fd;
    rtp_track_state_t video;
    rtp_track_state_t audio;
    hevc_stream_t hevc;
    aac_config_t audio_cfg;
    char session_id[32];
    int verbose;
} client_state_t;

static const unsigned g_adts_sample_rates[16] = {
    96000u, 88200u, 64000u, 48000u,
    44100u, 32000u, 24000u, 22050u,
    16000u, 12000u, 11025u, 8000u,
    7350u, 0u, 0u, 0u
};

static void log_line(const server_config_t *cfg, const char *fmt, ...)
{
    va_list ap;
    va_start(ap, fmt);
    if (cfg == NULL || cfg->verbose) {
        vfprintf(stderr, fmt, ap);
        fputc('\n', stderr);
    }
    va_end(ap);
}

static void sleep_ms(unsigned ms)
{
    struct timespec ts;
    ts.tv_sec = (time_t)(ms / 1000u);
    ts.tv_nsec = (long)((ms % 1000u) * 1000000u);
    while (nanosleep(&ts, &ts) != 0 && errno == EINTR) {
    }
}

static int read_file_fully(const char *path, ring_snapshot_t *snap)
{
    FILE *fp = fopen(path, "rb");
    long size_long;

    memset(snap, 0, sizeof(*snap));
    if (fp == NULL) {
        return -1;
    }
    if (fseek(fp, 0, SEEK_END) != 0) {
        fclose(fp);
        return -1;
    }
    size_long = ftell(fp);
    if (size_long < 0) {
        fclose(fp);
        return -1;
    }
    if (fseek(fp, 0, SEEK_SET) != 0) {
        fclose(fp);
        return -1;
    }
    snap->size = (size_t)size_long;
    if (snap->size <= HEADER_SIZE) {
        fclose(fp);
        errno = EINVAL;
        return -1;
    }
    snap->data = (uint8_t *)malloc(snap->size);
    if (snap->data == NULL) {
        fclose(fp);
        return -1;
    }
    if (fread(snap->data, 1, snap->size, fp) != snap->size) {
        free(snap->data);
        snap->data = NULL;
        fclose(fp);
        return -1;
    }
    fclose(fp);
    snap->payload_size = snap->size - HEADER_SIZE;
    return 0;
}

static void free_snapshot(ring_snapshot_t *snap)
{
    free(snap->data);
    memset(snap, 0, sizeof(*snap));
}

static int compare_entry_order(const ring_entry_t *ea, const ring_entry_t *eb)
{
    if (ea->ts_ms < eb->ts_ms) {
        return -1;
    }
    if (ea->ts_ms > eb->ts_ms) {
        return 1;
    }
    if (ea->seq < eb->seq) {
        return -1;
    }
    if (ea->seq > eb->seq) {
        return 1;
    }
    if (ea->logical_offset < eb->logical_offset) {
        return -1;
    }
    if (ea->logical_offset > eb->logical_offset) {
        return 1;
    }
    if (ea->type < eb->type) {
        return -1;
    }
    if (ea->type > eb->type) {
        return 1;
    }
    if (ea->total_len < eb->total_len) {
        return -1;
    }
    if (ea->total_len > eb->total_len) {
        return 1;
    }
    return 0;
}

static int compare_entries(const void *a, const void *b)
{
    return compare_entry_order((const ring_entry_t *)a, (const ring_entry_t *)b);
}

static int entries_are_identical(const ring_entry_t *ea, const ring_entry_t *eb)
{
    return ea->ts_ms == eb->ts_ms &&
           ea->logical_offset == eb->logical_offset &&
           ea->total_len == eb->total_len &&
           ea->type == eb->type &&
           ea->seq == eb->seq;
}

static int entry_is_newer(const ring_entry_t *entry, const ring_entry_t *cursor)
{
    return compare_entry_order(entry, cursor) > 0;
}

static size_t stable_entry_count(const ring_entry_t *entries, size_t count, uint64_t delay_ms)
{
    uint64_t newest_ts;
    uint64_t cutoff;
    size_t stable = count;

    (void)entries;

    if (count == 0 || delay_ms == 0) {
        return count;
    }

    newest_ts = entries[count - 1].ts_ms;
    if (newest_ts <= delay_ms) {
        return 0;
    }

    cutoff = newest_ts - delay_ms;
    while (stable > 0 && entries[stable - 1].ts_ms > cutoff) {
        stable--;
    }

    return stable;
}

static size_t collect_entries(const ring_snapshot_t *snap, ring_entry_t *entries, size_t max_entries)
{
    size_t count = 0;
    size_t limit = (HEADER_SIZE - ENTRY_START) / ENTRY_SIZE;
    size_t i;

    if (limit > max_entries) {
        limit = max_entries;
    }

    for (i = 0; i < limit; ++i) {
        size_t off = ENTRY_START + i * ENTRY_SIZE;
        const uint8_t *entry = snap->data + off;
        ring_entry_t tmp;

        memset(&tmp, 0, sizeof(tmp));
        memcpy(&tmp.ts_ms, entry + 0, sizeof(uint64_t));
        memcpy(&tmp.logical_offset, entry + 8, sizeof(uint32_t));
        memcpy(&tmp.total_len, entry + 12, sizeof(uint32_t));
        memcpy(&tmp.type, entry + 16, sizeof(uint32_t));
        memcpy(&tmp.seq, entry + 20, sizeof(uint32_t));

        if (tmp.type != VIDEO_TYPE && tmp.type != AUDIO_TYPE) {
            continue;
        }
        if (tmp.total_len <= FRAME_HEADER_SIZE || tmp.total_len > snap->payload_size) {
            continue;
        }
        if (tmp.ts_ms == 0 || tmp.seq == 0) {
            continue;
        }
        entries[count++] = tmp;
    }

    qsort(entries, count, sizeof(entries[0]), compare_entries);

    if (count > 1) {
        size_t out = 1;
        for (i = 1; i < count; ++i) {
            if (entries_are_identical(&entries[i], &entries[out - 1])) {
                continue;
            }
            entries[out++] = entries[i];
        }
        count = out;
    }

    return count;
}

static uint8_t *extract_frame_bytes(const ring_snapshot_t *snap, const ring_entry_t *entry, size_t *frame_len_out)
{
    size_t frame_len = entry->total_len;
    uint8_t *buf = (uint8_t *)malloc(frame_len);
    size_t payload_off = entry->logical_offset % snap->payload_size;
    size_t first_chunk;

    if (buf == NULL) {
        return NULL;
    }

    first_chunk = frame_len;
    if (payload_off + frame_len > snap->payload_size) {
        first_chunk = snap->payload_size - payload_off;
    }

    memcpy(buf, snap->data + HEADER_SIZE + payload_off, first_chunk);
    if (first_chunk < frame_len) {
        memcpy(buf + first_chunk, snap->data + HEADER_SIZE, frame_len - first_chunk);
    }

    *frame_len_out = frame_len;
    return buf;
}

static size_t find_start_code(const uint8_t *buf, size_t len, size_t offset, size_t *sc_size)
{
    size_t i;
    for (i = offset; i + 3 < len; ++i) {
        if (buf[i] == 0 && buf[i + 1] == 0) {
            if (buf[i + 2] == 1) {
                *sc_size = 3;
                return i;
            }
            if (i + 4 < len && buf[i + 2] == 0 && buf[i + 3] == 1) {
                *sc_size = 4;
                return i;
            }
        }
    }
    return len;
}

static uint32_t read_le32(const uint8_t *buf)
{
    uint32_t v = 0;
    memcpy(&v, buf, sizeof(v));
    return v;
}

static int video_frame_has_chunk_header(const uint8_t *frame, size_t frame_len)
{
    if (frame_len <= FRAME_HEADER_SIZE) {
        return 0;
    }

    return read_le32(frame + 4) == 413u &&
           read_le32(frame + 8) == (uint32_t)(frame_len - FRAME_HEADER_SIZE) &&
           read_le32(frame + 12) == 0u &&
           read_le32(frame + 16) == 0u;
}

static size_t video_payload_offset(const uint8_t *frame, size_t frame_len, size_t structured_skip)
{
    /*
     * The 36-byte region at the start of some type=129 entries is not uniform.
     * The first 20 bytes behave like a structured per-chunk prefix, while bytes
     * 20..35 appear to belong to the bytestream carried over from the previous
     * chunk. If we drop all 36 bytes we risk truncating the tail of the prior
     * NAL; if there is no structured prefix at all we must keep the full chunk.
     */
    size_t skip = structured_skip;

    if (!video_frame_has_chunk_header(frame, frame_len)) {
        return 0u;
    }
    if (skip > FRAME_HEADER_SIZE) {
        skip = FRAME_HEADER_SIZE;
    }
    if (skip >= frame_len) {
        return 0u;
    }
    return skip;
}

static int hevc_header_valid(const uint8_t *nal, size_t nal_len, uint8_t *nal_type_out)
{
    uint8_t nal_type;
    uint8_t tid;
    uint8_t layer_id;

    if (nal_len < 2) {
        return 0;
    }

    nal_type = (uint8_t)((nal[0] >> 1) & 0x3fu);
    layer_id = (uint8_t)(((nal[0] & 0x01u) << 5) | ((nal[1] >> 3) & 0x1fu));
    tid = (uint8_t)(nal[1] & 0x07u);

    if (tid == 0 || layer_id != 0 || nal_type > 50u) {
        return 0;
    }

    if (nal_type_out != NULL) {
        *nal_type_out = nal_type;
    }
    return 1;
}

static int find_valid_hevc_start(const uint8_t *payload,
                                 size_t payload_len,
                                 size_t search_offset,
                                 size_t max_prefix,
                                 size_t *start_out,
                                 size_t *sc_size_out)
{
    size_t off = search_offset;

    while (off < payload_len) {
        size_t sc_size = 0;
        size_t start = find_start_code(payload, payload_len, off, &sc_size);

        if (start >= payload_len || start > max_prefix) {
            break;
        }
        if (start + sc_size + 2 <= payload_len &&
            hevc_header_valid(payload + start + sc_size, payload_len - start - sc_size, NULL)) {
            *start_out = start;
            *sc_size_out = sc_size;
            return 0;
        }
        off = start + sc_size;
    }

    errno = ENODATA;
    return -1;
}

static int parse_adts_header(const uint8_t *buf,
                             size_t len,
                             aac_config_t *cfg_out,
                             size_t *header_len_out,
                             size_t *frame_len_out)
{
    unsigned protection_absent;
    unsigned object_type;
    unsigned sample_index;
    unsigned sample_rate;
    unsigned channels;
    size_t header_len;
    size_t frame_len;
    uint8_t asc0;
    uint8_t asc1;

    if (len < 7) {
        return -1;
    }
    if (buf[0] != 0xffu || (buf[1] & 0xf0u) != 0xf0u) {
        return -1;
    }

    protection_absent = buf[1] & 0x01u;
    object_type = ((unsigned)(buf[2] >> 6) & 0x03u) + 1u;
    sample_index = ((unsigned)buf[2] >> 2) & 0x0fu;
    sample_rate = (sample_index < 16u) ? g_adts_sample_rates[sample_index] : 0u;
    channels = (((unsigned)buf[2] & 0x01u) << 2) | (((unsigned)buf[3] >> 6) & 0x03u);
    header_len = protection_absent ? 7u : 9u;
    frame_len = (((size_t)buf[3] & 0x03u) << 11) |
                ((size_t)buf[4] << 3) |
                (((size_t)buf[5] >> 5) & 0x07u);

    if (sample_rate == 0u || channels == 0u || frame_len < header_len || frame_len > len) {
        return -1;
    }

    asc0 = (uint8_t)((object_type << 3) | (sample_index >> 1));
    asc1 = (uint8_t)(((sample_index & 0x01u) << 7) | ((channels & 0x0fu) << 3));

    if (cfg_out != NULL) {
        memset(cfg_out, 0, sizeof(*cfg_out));
        cfg_out->present = 1;
        cfg_out->object_type = object_type;
        cfg_out->sample_rate = sample_rate;
        cfg_out->sample_index = sample_index;
        cfg_out->channels = channels;
        snprintf(cfg_out->config_hex, sizeof(cfg_out->config_hex), "%02x%02x", asc0, asc1);
    }
    if (header_len_out != NULL) {
        *header_len_out = header_len;
    }
    if (frame_len_out != NULL) {
        *frame_len_out = frame_len;
    }
    return 0;
}

static int find_valid_adts_frame(const uint8_t *payload,
                                 size_t payload_len,
                                 size_t max_prefix,
                                 size_t *start_out,
                                 size_t *header_len_out,
                                 size_t *frame_len_out,
                                 aac_config_t *cfg_out)
{
    size_t i;

    for (i = 0; i + 7 <= payload_len && i <= max_prefix; ++i) {
        if (parse_adts_header(payload + i, payload_len - i, cfg_out, header_len_out, frame_len_out) == 0) {
            if (start_out != NULL) {
                *start_out = i;
            }
            return 0;
        }
    }

    errno = ENODATA;
    return -1;
}

static int hevc_flags_to_score(int has_vps, int has_sps, int has_pps, int has_idr)
{
    if (has_vps && has_sps && has_pps && has_idr) {
        return 4;
    }
    if (has_sps && has_pps && has_idr) {
        return 3;
    }
    if (has_idr) {
        return 2;
    }
    if (has_vps || has_sps || has_pps) {
        return 1;
    }
    return 0;
}

static int hevc_sequence_score_from_start(const uint8_t *payload, size_t len, size_t start)
{
    size_t off;
    int has_vps = 0;
    int has_sps = 0;
    int has_pps = 0;
    int has_idr = 0;

    off = start;
    while (off < len) {
        size_t cur_start = 0;
        size_t cur_sc_size = 0;
        size_t next_start = len;
        size_t next_sc_size = 0;
        size_t nal_start;
        uint8_t nal_type;

        if (find_valid_hevc_start(payload, len, off, len, &cur_start, &cur_sc_size) != 0) {
            break;
        }

        nal_start = cur_start + cur_sc_size;
        if (!hevc_header_valid(payload + nal_start, len - nal_start, &nal_type)) {
            off = cur_start + cur_sc_size;
            continue;
        }

        if (nal_type == 32u) {
            has_vps = 1;
        } else if (nal_type == 33u) {
            has_sps = 1;
        } else if (nal_type == 34u) {
            has_pps = 1;
        } else if (nal_type == 19u || nal_type == 20u) {
            has_idr = 1;
        }

        if (find_valid_hevc_start(payload, len, nal_start, len, &next_start, &next_sc_size) != 0) {
            break;
        }
        if (next_start <= cur_start) {
            break;
        }
        off = next_start;
        (void)next_sc_size;
    }

    return hevc_flags_to_score(has_vps, has_sps, has_pps, has_idr);
}

static int frame_key_score(const uint8_t *payload, size_t len)
{
    size_t off = 0;
    int best = 0;

    while (off < len) {
        size_t start = 0;
        size_t sc_size = 0;
        int score;

        if (find_valid_hevc_start(payload, len, off, VIDEO_PREFIX_SCAN_LIMIT, &start, &sc_size) != 0) {
            break;
        }
        score = hevc_sequence_score_from_start(payload, len, start);
        if (score > best) {
            best = score;
        }
        off = start + sc_size;
    }

    return best;
}

static ssize_t find_anchor_index(const ring_snapshot_t *snap,
                                 const ring_entry_t *entries,
                                 size_t count,
                                 int *score_out,
                                 size_t structured_skip)
{
    ssize_t i;
    ssize_t best_full = -1;
    ssize_t best_fallback = -1;
    int best_fallback_score = 0;

    for (i = (ssize_t)count - 1; i >= 0; --i) {
        size_t frame_len = 0;
        size_t payload_off = 0;
        uint8_t *frame;
        int score;

        if (entries[i].type != VIDEO_TYPE) {
            continue;
        }

        frame = extract_frame_bytes(snap, &entries[i], &frame_len);
        if (frame == NULL || frame_len <= FRAME_HEADER_SIZE) {
            free(frame);
            continue;
        }

        payload_off = video_payload_offset(frame, frame_len, structured_skip);
        if (payload_off == 0) {
            free(frame);
            continue;
        }

        score = frame_key_score(frame + payload_off, frame_len - payload_off);
        free(frame);

        if (score >= 4) {
            best_full = i;
            break;
        }
        if (score > 0 && score >= best_fallback_score) {
            best_fallback = i;
            best_fallback_score = score;
        }
    }

    if (best_full >= 0) {
        if (score_out != NULL) {
            *score_out = 4;
        }
        return best_full;
    }
    if (best_fallback >= 0) {
        if (score_out != NULL) {
            *score_out = best_fallback_score;
        }
        return best_fallback;
    }
    if (score_out != NULL) {
        *score_out = 0;
    }
    return -1;
}

static int detect_audio_config_from_snapshot(const ring_snapshot_t *snap, aac_config_t *cfg_out)
{
    ring_entry_t entries[MAX_TABLE_ENTRIES];
    size_t count = collect_entries(snap, entries, MAX_TABLE_ENTRIES);
    ssize_t i;

    memset(cfg_out, 0, sizeof(*cfg_out));

    for (i = (ssize_t)count - 1; i >= 0; --i) {
        size_t frame_len = 0;
        uint8_t *frame;
        size_t start = 0;
        size_t header_len = 0;
        size_t adts_len = 0;
        aac_config_t tmp_cfg;

        if (entries[i].type != AUDIO_TYPE) {
            continue;
        }

        frame = extract_frame_bytes(snap, &entries[i], &frame_len);
        if (frame == NULL || frame_len <= FRAME_HEADER_SIZE) {
            free(frame);
            continue;
        }

        if (find_valid_adts_frame(frame + FRAME_HEADER_SIZE,
                                  frame_len - FRAME_HEADER_SIZE,
                                  AUDIO_PREFIX_SCAN_LIMIT,
                                  &start,
                                  &header_len,
                                  &adts_len,
                                  &tmp_cfg) == 0) {
            free(frame);
            *cfg_out = tmp_cfg;
            return 0;
        }

        free(frame);
    }

    errno = ENODATA;
    return -1;
}

static int detect_audio_config(const server_config_t *cfg, aac_config_t *cfg_out)
{
    ring_snapshot_t snap;
    int rc;

    memset(&snap, 0, sizeof(snap));
    rc = read_file_fully(cfg->ring_path, &snap);
    if (rc != 0) {
        return -1;
    }

    rc = detect_audio_config_from_snapshot(&snap, cfg_out);
    free_snapshot(&snap);
    return rc;
}

static int send_all(int fd, const uint8_t *buf, size_t len)
{
    while (len > 0) {
        ssize_t sent = send(fd, buf, len, 0);
        if (sent < 0) {
            if (errno == EINTR) {
                continue;
            }
            return -1;
        }
        if (sent == 0) {
            errno = EPIPE;
            return -1;
        }
        buf += (size_t)sent;
        len -= (size_t)sent;
    }
    return 0;
}

static int send_interleaved(int fd, uint8_t channel, const uint8_t *payload, size_t payload_len)
{
    uint8_t header[4];
    header[0] = '$';
    header[1] = channel;
    header[2] = (uint8_t)((payload_len >> 8) & 0xffu);
    header[3] = (uint8_t)(payload_len & 0xffu);
    if (send_all(fd, header, sizeof(header)) != 0) {
        return -1;
    }
    return send_all(fd, payload, payload_len);
}

static uint32_t compute_rtp_ts(rtp_track_state_t *track, uint64_t ts_ms, uint64_t offset_ms, uint32_t clock_rate)
{
    uint64_t delta_ms;

    if (track->media_ts_base_ms == 0) {
        track->media_ts_base_ms = ts_ms;
    }

    delta_ms = (ts_ms - track->media_ts_base_ms) + offset_ms;
    return track->rtp_ts_base + (uint32_t)((delta_ms * (uint64_t)clock_rate) / 1000u);
}

static int send_rtp_packet(client_state_t *client,
                           rtp_track_state_t *track,
                           uint8_t payload_type,
                           const uint8_t *payload,
                           size_t payload_len,
                           uint32_t rtp_ts,
                           int marker)
{
    uint8_t packet[12 + 4 + RTP_MAX_PAYLOAD];
    size_t total_len = 12u + payload_len;

    if (payload_len > sizeof(packet) - 12u) {
        errno = EMSGSIZE;
        return -1;
    }

    packet[0] = 0x80u;
    packet[1] = (uint8_t)(payload_type | (marker ? 0x80u : 0x00u));
    packet[2] = (uint8_t)((track->rtp_seq >> 8) & 0xffu);
    packet[3] = (uint8_t)(track->rtp_seq & 0xffu);
    packet[4] = (uint8_t)((rtp_ts >> 24) & 0xffu);
    packet[5] = (uint8_t)((rtp_ts >> 16) & 0xffu);
    packet[6] = (uint8_t)((rtp_ts >> 8) & 0xffu);
    packet[7] = (uint8_t)(rtp_ts & 0xffu);
    packet[8] = (uint8_t)((track->rtp_ssrc >> 24) & 0xffu);
    packet[9] = (uint8_t)((track->rtp_ssrc >> 16) & 0xffu);
    packet[10] = (uint8_t)((track->rtp_ssrc >> 8) & 0xffu);
    packet[11] = (uint8_t)(track->rtp_ssrc & 0xffu);
    memcpy(packet + 12, payload, payload_len);

    track->rtp_seq = (uint16_t)(track->rtp_seq + 1u);
    return send_interleaved(client->fd, track->rtp_channel, packet, total_len);
}

static int send_hevc_nal(client_state_t *client, const uint8_t *nal, size_t nal_len, uint32_t rtp_ts, int is_last_nal)
{
    uint8_t nal_type;

    if (!hevc_header_valid(nal, nal_len, &nal_type)) {
        errno = ENODATA;
        return 1;
    }

    if (nal_len <= RTP_MAX_PAYLOAD) {
        if (send_rtp_packet(client, &client->video, VIDEO_PAYLOAD_TYPE, nal, nal_len, rtp_ts, is_last_nal) != 0) {
            return -1;
        }
        return 0;
    }

    {
        uint8_t fu_indicator[2];
        size_t pos = 2;
        int first = 1;

        fu_indicator[0] = (uint8_t)((nal[0] & 0x81u) | (49u << 1));
        fu_indicator[1] = nal[1];

        while (pos < nal_len) {
            uint8_t payload[3 + RTP_MAX_PAYLOAD];
            size_t frag = nal_len - pos;
            int last = 0;

            if (frag > RTP_MAX_PAYLOAD - 3u) {
                frag = RTP_MAX_PAYLOAD - 3u;
            } else {
                last = 1;
            }

            payload[0] = fu_indicator[0];
            payload[1] = fu_indicator[1];
            payload[2] = (uint8_t)((first ? 0x80u : 0x00u) | (last ? 0x40u : 0x00u) | nal_type);
            memcpy(payload + 3, nal + pos, frag);

            if (send_rtp_packet(client,
                                &client->video,
                                VIDEO_PAYLOAD_TYPE,
                                payload,
                                frag + 3u,
                                rtp_ts,
                                last && is_last_nal) != 0) {
                return -1;
            }

            first = 0;
            pos += frag;
        }
    }

    return 0;
}

static void clear_hevc_nal(hevc_nal_t *nal)
{
    if (nal == NULL) {
        return;
    }
    free(nal->data);
    nal->data = NULL;
    nal->len = 0;
}

static void clear_hevc_nal_list(hevc_nal_t *list, size_t count)
{
    size_t i;

    for (i = 0; i < count; ++i) {
        clear_hevc_nal(&list[i]);
    }
}

static void reset_hevc_stream(hevc_stream_t *stream)
{
    if (stream == NULL) {
        return;
    }
    free(stream->pending);
    stream->pending = NULL;
    stream->pending_len = 0;
    stream->pending_cap = 0;

    free(stream->segments);
    stream->segments = NULL;
    stream->segment_count = 0;
    stream->segment_cap = 0;

    clear_hevc_nal_list(stream->prefix_nals, stream->prefix_count);
    free(stream->prefix_nals);
    stream->prefix_nals = NULL;
    stream->prefix_count = 0;
    stream->prefix_cap = 0;

    clear_hevc_nal_list(stream->current_au_nals, stream->current_au_count);
    free(stream->current_au_nals);
    stream->current_au_nals = NULL;
    stream->current_au_count = 0;
    stream->current_au_cap = 0;
    stream->current_au_ts_ms = 0;
    stream->current_au_active = 0;
    stream->synced = 0;
    stream->have_vps = 0;
    stream->have_sps = 0;
    stream->have_pps = 0;
}

static int ensure_hevc_nal_capacity(hevc_nal_t **list, size_t *cap, size_t need)
{
    hevc_nal_t *tmp;
    size_t new_cap = *cap ? *cap : 8u;

    while (new_cap < need) {
        new_cap *= 2u;
    }

    tmp = (hevc_nal_t *)realloc(*list, new_cap * sizeof(*tmp));
    if (tmp == NULL) {
        return -1;
    }

    *list = tmp;
    *cap = new_cap;
    return 0;
}

static int ensure_hevc_segment_capacity(hevc_stream_t *stream, size_t need)
{
    hevc_pending_segment_t *tmp;
    size_t new_cap = stream->segment_cap ? stream->segment_cap : 8u;

    while (new_cap < need) {
        new_cap *= 2u;
    }

    tmp = (hevc_pending_segment_t *)realloc(stream->segments, new_cap * sizeof(*tmp));
    if (tmp == NULL) {
        return -1;
    }

    stream->segments = tmp;
    stream->segment_cap = new_cap;
    return 0;
}

static int ensure_hevc_pending_capacity(hevc_stream_t *stream, size_t need)
{
    uint8_t *tmp;
    size_t new_cap = stream->pending_cap ? stream->pending_cap : 4096u;

    while (new_cap < need) {
        new_cap *= 2u;
    }

    tmp = (uint8_t *)realloc(stream->pending, new_cap);
    if (tmp == NULL) {
        return -1;
    }

    stream->pending = tmp;
    stream->pending_cap = new_cap;
    return 0;
}

static int push_hevc_nal_copy(hevc_nal_t **list,
                              size_t *count,
                              size_t *cap,
                              const uint8_t *nal,
                              size_t nal_len)
{
    hevc_nal_t *slot;

    if (ensure_hevc_nal_capacity(list, cap, *count + 1u) != 0) {
        return -1;
    }

    slot = &(*list)[*count];
    memset(slot, 0, sizeof(*slot));
    slot->data = (uint8_t *)malloc(nal_len);
    if (slot->data == NULL) {
        return -1;
    }
    memcpy(slot->data, nal, nal_len);
    slot->len = nal_len;
    (*count)++;
    return 0;
}

static void clear_bootstrap_prefix(hevc_stream_t *stream)
{
    clear_hevc_nal_list(stream->prefix_nals, stream->prefix_count);
    stream->prefix_count = 0;
    stream->have_vps = 0;
    stream->have_sps = 0;
    stream->have_pps = 0;
}

static int store_bootstrap_prefix_nal(hevc_stream_t *stream,
                                      const uint8_t *nal,
                                      size_t nal_len,
                                      uint8_t nal_type)
{
    size_t i;

    for (i = 0; i < stream->prefix_count; ++i) {
        uint8_t existing_type = 0;
        uint8_t *copy;

        if (!hevc_header_valid(stream->prefix_nals[i].data, stream->prefix_nals[i].len, &existing_type) ||
            existing_type != nal_type) {
            continue;
        }

        copy = (uint8_t *)malloc(nal_len);
        if (copy == NULL) {
            return -1;
        }
        memcpy(copy, nal, nal_len);
        free(stream->prefix_nals[i].data);
        stream->prefix_nals[i].data = copy;
        stream->prefix_nals[i].len = nal_len;
        return 0;
    }

    return push_hevc_nal_copy(&stream->prefix_nals, &stream->prefix_count, &stream->prefix_cap, nal, nal_len);
}

static int append_pending_bytes(hevc_stream_t *stream, const uint8_t *payload, size_t payload_len, uint64_t ts_ms)
{
    if (payload_len == 0) {
        return 0;
    }

    if (ensure_hevc_pending_capacity(stream, stream->pending_len + payload_len) != 0) {
        return -1;
    }

    memcpy(stream->pending + stream->pending_len, payload, payload_len);
    stream->pending_len += payload_len;

    if (stream->segment_count > 0 && stream->segments[stream->segment_count - 1].ts_ms == ts_ms) {
        stream->segments[stream->segment_count - 1].end_offset = stream->pending_len;
        return 0;
    }

    if (ensure_hevc_segment_capacity(stream, stream->segment_count + 1u) != 0) {
        return -1;
    }

    stream->segments[stream->segment_count].end_offset = stream->pending_len;
    stream->segments[stream->segment_count].ts_ms = ts_ms;
    stream->segment_count++;
    return 0;
}

static void discard_pending_bytes(hevc_stream_t *stream, size_t prefix_len)
{
    size_t in = 0;
    size_t out = 0;

    if (prefix_len == 0 || stream->pending_len == 0) {
        return;
    }
    if (prefix_len >= stream->pending_len) {
        stream->pending_len = 0;
        stream->segment_count = 0;
        return;
    }

    memmove(stream->pending, stream->pending + prefix_len, stream->pending_len - prefix_len);
    stream->pending_len -= prefix_len;

    for (in = 0; in < stream->segment_count; ++in) {
        if (stream->segments[in].end_offset <= prefix_len) {
            continue;
        }
        stream->segments[in].end_offset -= prefix_len;
        stream->segments[out++] = stream->segments[in];
    }
    stream->segment_count = out;
}

static uint64_t pending_ts_for_offset(const hevc_stream_t *stream, size_t offset)
{
    size_t i;

    for (i = 0; i < stream->segment_count; ++i) {
        if (offset < stream->segments[i].end_offset) {
            return stream->segments[i].ts_ms;
        }
    }
    if (stream->segment_count > 0) {
        return stream->segments[stream->segment_count - 1].ts_ms;
    }
    return 0;
}

static int hevc_nal_is_vcl(uint8_t nal_type)
{
    return nal_type <= 31u;
}

static int hevc_nal_is_prefix(uint8_t nal_type)
{
    return nal_type == 32u || nal_type == 33u || nal_type == 34u || nal_type == 35u || nal_type == 39u;
}

static int hevc_nal_is_idr(uint8_t nal_type)
{
    return nal_type == 19u || nal_type == 20u;
}

static int hevc_slice_first_flag(const uint8_t *nal, size_t nal_len, int *first_flag_out)
{
    uint8_t nal_type = 0;

    if (!hevc_header_valid(nal, nal_len, &nal_type) || !hevc_nal_is_vcl(nal_type) || nal_len < 3) {
        return -1;
    }

    *first_flag_out = (nal[2] & 0x80u) != 0;
    return 0;
}

static int move_prefix_to_current_au(hevc_stream_t *stream)
{
    size_t i;

    if (stream->prefix_count == 0) {
        return 0;
    }
    if (ensure_hevc_nal_capacity(&stream->current_au_nals,
                                 &stream->current_au_cap,
                                 stream->current_au_count + stream->prefix_count) != 0) {
        return -1;
    }

    for (i = 0; i < stream->prefix_count; ++i) {
        stream->current_au_nals[stream->current_au_count++] = stream->prefix_nals[i];
        memset(&stream->prefix_nals[i], 0, sizeof(stream->prefix_nals[i]));
    }
    stream->prefix_count = 0;
    return 0;
}

static int flush_current_au(client_state_t *client, uint64_t offset_ms)
{
    uint32_t rtp_ts;
    size_t i;

    if (!client->hevc.current_au_active || client->hevc.current_au_count == 0) {
        client->hevc.current_au_active = 0;
        return 0;
    }

    rtp_ts = compute_rtp_ts(&client->video, client->hevc.current_au_ts_ms, offset_ms, VIDEO_CLOCK_RATE);
    for (i = 0; i < client->hevc.current_au_count; ++i) {
        if (send_hevc_nal(client,
                          client->hevc.current_au_nals[i].data,
                          client->hevc.current_au_nals[i].len,
                          rtp_ts,
                          i + 1u == client->hevc.current_au_count) != 0) {
            return -1;
        }
    }

    clear_hevc_nal_list(client->hevc.current_au_nals, client->hevc.current_au_count);
    client->hevc.current_au_count = 0;
    client->hevc.current_au_ts_ms = 0;
    client->hevc.current_au_active = 0;
    return 0;
}

static int route_complete_hevc_nal(client_state_t *client,
                                   const uint8_t *nal,
                                   size_t nal_len,
                                   uint64_t ts_ms,
                                   uint64_t offset_ms)
{
    uint8_t nal_type = 0;
    int first_slice = 0;

    if (!hevc_header_valid(nal, nal_len, &nal_type)) {
        return 0;
    }

    if (!client->hevc.synced) {
        if (hevc_nal_is_prefix(nal_type)) {
            if (nal_type == 32u) {
                clear_bootstrap_prefix(&client->hevc);
            }
            if (nal_type == 32u || nal_type == 33u || nal_type == 34u) {
                if (store_bootstrap_prefix_nal(&client->hevc, nal, nal_len, nal_type) != 0) {
                    return -1;
                }
                if (nal_type == 32u) {
                    client->hevc.have_vps = 1;
                } else if (nal_type == 33u) {
                    client->hevc.have_sps = 1;
                } else if (nal_type == 34u) {
                    client->hevc.have_pps = 1;
                }
            }
            return 0;
        }

        if (!hevc_nal_is_vcl(nal_type) || hevc_slice_first_flag(nal, nal_len, &first_slice) != 0) {
            return 0;
        }
        if (!hevc_nal_is_idr(nal_type) ||
            !first_slice ||
            !client->hevc.have_vps ||
            !client->hevc.have_sps ||
            !client->hevc.have_pps) {
            return 0;
        }

        client->hevc.synced = 1;
        client->hevc.current_au_active = 1;
        client->hevc.current_au_ts_ms = ts_ms;
        if (move_prefix_to_current_au(&client->hevc) != 0) {
            return -1;
        }
        return push_hevc_nal_copy(&client->hevc.current_au_nals,
                                  &client->hevc.current_au_count,
                                  &client->hevc.current_au_cap,
                                  nal,
                                  nal_len);
    }

    if (hevc_nal_is_vcl(nal_type)) {
        if (hevc_slice_first_flag(nal, nal_len, &first_slice) != 0) {
            return 0;
        }

        if (first_slice) {
            if (flush_current_au(client, offset_ms) != 0) {
                return -1;
            }
            client->hevc.current_au_active = 1;
            client->hevc.current_au_ts_ms = ts_ms;
            if (move_prefix_to_current_au(&client->hevc) != 0) {
                return -1;
            }
        } else if (!client->hevc.current_au_active) {
            client->hevc.current_au_active = 1;
            client->hevc.current_au_ts_ms = ts_ms;
            if (move_prefix_to_current_au(&client->hevc) != 0) {
                return -1;
            }
        }

        return push_hevc_nal_copy(&client->hevc.current_au_nals,
                                  &client->hevc.current_au_count,
                                  &client->hevc.current_au_cap,
                                  nal,
                                  nal_len);
    }

    if (hevc_nal_is_prefix(nal_type) || !client->hevc.current_au_active) {
        return push_hevc_nal_copy(&client->hevc.prefix_nals,
                                  &client->hevc.prefix_count,
                                  &client->hevc.prefix_cap,
                                  nal,
                                  nal_len);
    }

    return push_hevc_nal_copy(&client->hevc.current_au_nals,
                              &client->hevc.current_au_count,
                              &client->hevc.current_au_cap,
                              nal,
                              nal_len);
}

static int feed_hevc_chunk(client_state_t *client,
                           const uint8_t *payload,
                           size_t payload_len,
                           uint64_t ts_ms,
                           uint64_t offset_ms)
{
    if (append_pending_bytes(&client->hevc, payload, payload_len, ts_ms) != 0) {
        return -1;
    }

    while (client->hevc.pending_len > 0) {
        size_t cur_start = 0;
        size_t cur_sc_size = 0;
        size_t next_start = 0;
        size_t next_sc_size = 0;
        size_t nal_end;
        uint64_t nal_ts_ms;

        if (find_valid_hevc_start(client->hevc.pending,
                                  client->hevc.pending_len,
                                  0,
                                  client->hevc.pending_len,
                                  &cur_start,
                                  &cur_sc_size) != 0) {
            break;
        }

        if (cur_start > 0) {
            discard_pending_bytes(&client->hevc, cur_start);
            continue;
        }

        if (find_valid_hevc_start(client->hevc.pending,
                                  client->hevc.pending_len,
                                  cur_sc_size,
                                  client->hevc.pending_len,
                                  &next_start,
                                  &next_sc_size) != 0 ||
            next_start <= cur_start) {
            break;
        }

        nal_end = next_start;
        while (nal_end > cur_sc_size && client->hevc.pending[nal_end - 1] == 0) {
            nal_end--;
        }

        nal_ts_ms = pending_ts_for_offset(&client->hevc, 0);
        if (nal_end > cur_sc_size) {
            if (route_complete_hevc_nal(client,
                                        client->hevc.pending + cur_sc_size,
                                        nal_end - cur_sc_size,
                                        nal_ts_ms,
                                        offset_ms) != 0) {
                return -1;
            }
        }

        discard_pending_bytes(&client->hevc, next_start);
        (void)next_sc_size;
    }

    return 0;
}

static int send_aac_frame(client_state_t *client, const uint8_t *payload, size_t payload_len, uint32_t rtp_ts)
{
    size_t start = 0;
    size_t header_len = 0;
    size_t adts_len = 0;
    aac_config_t parsed_cfg;
    const uint8_t *raw;
    size_t raw_len;
    uint8_t packet[4 + RTP_MAX_PAYLOAD];

    if (find_valid_adts_frame(payload,
                              payload_len,
                              AUDIO_PREFIX_SCAN_LIMIT,
                              &start,
                              &header_len,
                              &adts_len,
                              &parsed_cfg) != 0) {
        return 1;
    }

    if (!client->audio_cfg.present) {
        client->audio_cfg = parsed_cfg;
    }

    raw = payload + start + header_len;
    raw_len = adts_len - header_len;
    if (raw_len > RTP_MAX_PAYLOAD - 4u) {
        errno = EMSGSIZE;
        return -1;
    }

    packet[0] = 0x00u;
    packet[1] = 0x10u;
    packet[2] = (uint8_t)((raw_len >> 5) & 0xffu);
    packet[3] = (uint8_t)((raw_len & 0x1fu) << 3);
    memcpy(packet + 4, raw, raw_len);

    if (send_rtp_packet(client, &client->audio, AUDIO_PAYLOAD_TYPE, packet, raw_len + 4u, rtp_ts, 1) != 0) {
        return -1;
    }
    return 0;
}

static int send_entry(client_state_t *client,
                      const server_config_t *cfg,
                      const ring_snapshot_t *snap,
                      const ring_entry_t *entry,
                      uint64_t offset_ms)
{
    size_t frame_len = 0;
    size_t payload_off = 0;
    uint8_t *frame = extract_frame_bytes(snap, entry, &frame_len);
    int rc = 0;

    if (frame == NULL) {
        return -1;
    }
    if (frame_len <= FRAME_HEADER_SIZE) {
        free(frame);
        return 0;
    }

    if (entry->type == VIDEO_TYPE) {
        if (!client->video.setup_done) {
            free(frame);
            return 0;
        }
        payload_off = video_payload_offset(frame, frame_len, cfg->video_structured_skip);
        rc = feed_hevc_chunk(client,
                             frame + payload_off,
                             frame_len - payload_off,
                             entry->ts_ms,
                             offset_ms);
    } else if (entry->type == AUDIO_TYPE) {
        if (!client->audio.setup_done || !client->audio_cfg.present) {
            free(frame);
            return 0;
        }
        rc = send_aac_frame(client,
                            frame + FRAME_HEADER_SIZE,
                            frame_len - FRAME_HEADER_SIZE,
                            compute_rtp_ts(&client->audio,
                                           entry->ts_ms,
                                           offset_ms,
                                           client->audio_cfg.sample_rate));
        if (rc > 0) {
            log_line(cfg, "Skipping malformed AAC entry ts=%" PRIu64 " seq=%" PRIu32, entry->ts_ms, entry->seq);
            rc = 0;
        }
    }

    free(frame);
    return rc;
}

static void pace_frames(uint64_t *last_ts, uint64_t current_ts)
{
    if (*last_ts != 0 && current_ts > *last_ts) {
        uint64_t delta = current_ts - *last_ts;
        if (delta > 250u) {
            delta = 250u;
        }
        sleep_ms((unsigned)delta);
    }
    *last_ts = current_ts;
}

static int send_range(client_state_t *client,
                      const server_config_t *cfg,
                      const ring_snapshot_t *snap,
                      const ring_entry_t *entries,
                      size_t start_idx,
                      size_t end_idx,
                      uint64_t offset_ms,
                      int paced)
{
    uint64_t last_ts = 0;
    size_t i;

    for (i = start_idx; i < end_idx; ++i) {
        if (paced) {
            pace_frames(&last_ts, entries[i].ts_ms);
        }
        if (send_entry(client, cfg, snap, &entries[i], offset_ms) != 0) {
            return -1;
        }
    }

    return 0;
}

static int recv_request(int fd, char *buf, size_t cap)
{
    size_t used = 0;
    while (used + 1 < cap) {
        ssize_t got = recv(fd, buf + used, cap - used - 1, 0);
        if (got < 0) {
            if (errno == EINTR) {
                continue;
            }
            return -1;
        }
        if (got == 0) {
            errno = EPIPE;
            return -1;
        }
        used += (size_t)got;
        buf[used] = '\0';
        if (strstr(buf, "\r\n\r\n") != NULL || strstr(buf, "\n\n") != NULL) {
            return 0;
        }
    }
    errno = EMSGSIZE;
    return -1;
}

static int header_value(const char *req, const char *name, char *out, size_t out_cap)
{
    const char *p = req;
    size_t name_len = strlen(name);

    while ((p = strcasestr(p, name)) != NULL) {
        const char *line_end;
        if ((p == req || p[-1] == '\n') && strncasecmp(p, name, name_len) == 0 && p[name_len] == ':') {
            p += name_len + 1;
            while (*p == ' ' || *p == '\t') {
                ++p;
            }
            line_end = strpbrk(p, "\r\n");
            if (line_end == NULL) {
                line_end = p + strlen(p);
            }
            if ((size_t)(line_end - p) >= out_cap) {
                return -1;
            }
            memcpy(out, p, (size_t)(line_end - p));
            out[line_end - p] = '\0';
            return 0;
        }
        p += name_len;
    }
    return -1;
}

static int request_method(const char *req, char *method, size_t method_cap, char *url, size_t url_cap)
{
    const char *line_end = strpbrk(req, "\r\n");
    char line[512];

    if (line_end == NULL || (size_t)(line_end - req) >= sizeof(line)) {
        return -1;
    }
    memcpy(line, req, (size_t)(line_end - req));
    line[line_end - req] = '\0';
    if (sscanf(line, "%31s %255s", method, url) != 2) {
        return -1;
    }
    method[method_cap - 1] = '\0';
    url[url_cap - 1] = '\0';
    return 0;
}

static int send_rtsp_response(int fd, int code, const char *status, const char *headers, const char *body)
{
    char resp[4096];
    int len = snprintf(resp,
                       sizeof(resp),
                       "RTSP/1.0 %d %s\r\n"
                       "%s"
                       "Content-Length: %zu\r\n"
                       "\r\n"
                       "%s",
                       code,
                       status,
                       headers ? headers : "",
                       body ? strlen(body) : 0u,
                       body ? body : "");
    if (len < 0 || (size_t)len >= sizeof(resp)) {
        errno = EMSGSIZE;
        return -1;
    }
    return send_all(fd, (const uint8_t *)resp, (size_t)len);
}

static int reply_options(int fd, const char *cseq)
{
    char headers[256];
    snprintf(headers,
             sizeof(headers),
             "CSeq: %s\r\n"
             "Public: OPTIONS, DESCRIBE, SETUP, PLAY, TEARDOWN\r\n",
             cseq);
    return send_rtsp_response(fd, 200, "OK", headers, "");
}

static void strip_track_suffix(const char *url, char *base_url, size_t base_url_cap)
{
    const char *track = strstr(url, "/trackID=");
    size_t len = track ? (size_t)(track - url) : strlen(url);

    if (len >= base_url_cap) {
        len = base_url_cap - 1u;
    }
    memcpy(base_url, url, len);
    base_url[len] = '\0';
}

static int reply_describe(int fd, const char *cseq, const char *url, const aac_config_t *audio_cfg)
{
    char headers[256];
    char body[1024];
    char base_url[256];
    int len;

    strip_track_suffix(url, base_url, sizeof(base_url));

    len = snprintf(body,
                   sizeof(body),
                   "v=0\r\n"
                   "o=- 0 0 IN IP4 0.0.0.0\r\n"
                   "s=Anyka Main Stream\r\n"
                   "t=0 0\r\n"
                   "a=control:*\r\n"
                   "m=video 0 RTP/AVP 96\r\n"
                   "c=IN IP4 0.0.0.0\r\n"
                   "a=rtpmap:96 H265/90000\r\n"
                   "a=control:trackID=0\r\n");
    if (len < 0 || (size_t)len >= sizeof(body)) {
        errno = EMSGSIZE;
        return -1;
    }

    if (audio_cfg != NULL && audio_cfg->present) {
        int extra = snprintf(body + len,
                             sizeof(body) - (size_t)len,
                             "m=audio 0 RTP/AVP 97\r\n"
                             "c=IN IP4 0.0.0.0\r\n"
                             "a=rtpmap:97 MPEG4-GENERIC/%u/%u\r\n"
                             "a=fmtp:97 streamtype=5;profile-level-id=1;mode=AAC-hbr;config=%s;SizeLength=13;IndexLength=3;IndexDeltaLength=3\r\n"
                             "a=control:trackID=1\r\n",
                             audio_cfg->sample_rate,
                             audio_cfg->channels,
                             audio_cfg->config_hex);
        if (extra < 0 || (size_t)extra >= sizeof(body) - (size_t)len) {
            errno = EMSGSIZE;
            return -1;
        }
        len += extra;
    }

    snprintf(headers,
             sizeof(headers),
             "CSeq: %s\r\n"
             "Content-Type: application/sdp\r\n",
             cseq);
    return send_rtsp_response(fd, 200, "OK", headers, body);
}

static int parse_track_id_from_url(const char *url)
{
    if (strstr(url, "trackID=1") != NULL) {
        return AUDIO_TRACK_ID;
    }
    return VIDEO_TRACK_ID;
}

static int parse_interleaved_channels(const char *transport, uint8_t *rtp_channel, uint8_t *rtcp_channel)
{
    const char *p = strstr(transport, "interleaved=");
    unsigned a = 0;
    unsigned b = 0;

    if (p == NULL) {
        return -1;
    }

    p += strlen("interleaved=");
    if (sscanf(p, "%u-%u", &a, &b) != 2 || a > 255u || b > 255u) {
        return -1;
    }

    *rtp_channel = (uint8_t)a;
    *rtcp_channel = (uint8_t)b;
    return 0;
}

static int reply_setup(int fd,
                       const char *cseq,
                       const char *session_id,
                       uint8_t rtp_channel,
                       uint8_t rtcp_channel)
{
    char headers[512];

    snprintf(headers,
             sizeof(headers),
             "CSeq: %s\r\n"
             "Session: %s\r\n"
             "Transport: RTP/AVP/TCP;unicast;interleaved=%u-%u\r\n",
             cseq,
             session_id,
             (unsigned)rtp_channel,
             (unsigned)rtcp_channel);
    return send_rtsp_response(fd, 200, "OK", headers, "");
}

static int reply_play(int fd, const char *cseq, const char *session_id, const char *url, const client_state_t *client)
{
    char headers[1024];
    char base_url[256];
    int len;

    strip_track_suffix(url, base_url, sizeof(base_url));

    len = snprintf(headers,
                   sizeof(headers),
                   "CSeq: %s\r\n"
                   "Session: %s\r\n"
                   "RTP-Info: url=%s/trackID=0;seq=%u;rtptime=%" PRIu32,
                   cseq,
                   session_id,
                   base_url,
                   (unsigned)client->video.rtp_seq,
                   client->video.rtp_ts_base);
    if (len < 0 || (size_t)len >= sizeof(headers)) {
        errno = EMSGSIZE;
        return -1;
    }

    if (client->audio.setup_done && client->audio_cfg.present) {
        int extra = snprintf(headers + len,
                             sizeof(headers) - (size_t)len,
                             ",url=%s/trackID=1;seq=%u;rtptime=%" PRIu32,
                             base_url,
                             (unsigned)client->audio.rtp_seq,
                             client->audio.rtp_ts_base);
        if (extra < 0 || (size_t)extra >= sizeof(headers) - (size_t)len) {
            errno = EMSGSIZE;
            return -1;
        }
        len += extra;
    }

    if ((size_t)len + 4u >= sizeof(headers)) {
        errno = EMSGSIZE;
        return -1;
    }
    memcpy(headers + len, "\r\n", 3u);

    return send_rtsp_response(fd, 200, "OK", headers, "");
}

static int reply_teardown(int fd, const char *cseq, const char *session_id)
{
    char headers[256];
    snprintf(headers, sizeof(headers), "CSeq: %s\r\nSession: %s\r\n", cseq, session_id);
    return send_rtsp_response(fd, 200, "OK", headers, "");
}

static int url_is_main_ch(const char *url)
{
    return strstr(url, "main_ch") != NULL;
}

static int stream_main(client_state_t *client, const server_config_t *cfg)
{
    ring_entry_t entries[MAX_TABLE_ENTRIES];
    ring_snapshot_t snap;
    size_t count = 0;
    size_t stable_count = 0;
    ssize_t anchor = -1;
    size_t start_idx = 0;
    ring_entry_t last_cursor;
    uint64_t loop_offset_ms = 0;
    uint64_t loop_span_ms = 1000;
    size_t video_entries = 0;
    size_t audio_entries = 0;
    int anchor_score = 0;
    int attempts;

    memset(&snap, 0, sizeof(snap));
    memset(&last_cursor, 0, sizeof(last_cursor));

    for (attempts = 0; attempts < (cfg->static_replay ? 1 : 150); ++attempts) {
        if (snap.data != NULL) {
            free_snapshot(&snap);
        }
        if (read_file_fully(cfg->ring_path, &snap) != 0) {
            return -1;
        }

        count = collect_entries(&snap, entries, MAX_TABLE_ENTRIES);
        if (count == 0) {
            if (!cfg->static_replay) {
                sleep_ms(100);
                continue;
            }
            free_snapshot(&snap);
            errno = ENODATA;
            return -1;
        }

        if (!client->audio_cfg.present) {
            (void)detect_audio_config_from_snapshot(&snap, &client->audio_cfg);
        }

        stable_count = stable_entry_count(entries, count, cfg->static_replay ? 0u : LIVE_STABILITY_DELAY_MS);
        if (stable_count == 0) {
            if (!cfg->static_replay) {
                sleep_ms(50);
                continue;
            }
            free_snapshot(&snap);
            errno = ENODATA;
            return -1;
        }

        anchor = find_anchor_index(&snap, entries, stable_count, &anchor_score, cfg->video_structured_skip);
        if (anchor >= 0 && anchor_score > 0) {
            break;
        }
        if (!cfg->static_replay) {
            sleep_ms(100);
        }
    }

    if (count == 0 || anchor < 0) {
        free_snapshot(&snap);
        errno = ENODATA;
        return -1;
    }
    if (anchor_score == 0) {
        log_line(cfg, "No usable anchor found after %d attempt(s)", attempts + 1);
        free_snapshot(&snap);
        errno = ENODATA;
        return -1;
    }

    start_idx = (size_t)anchor;
    last_cursor = entries[stable_count - 1];
    if (entries[stable_count - 1].ts_ms > entries[start_idx].ts_ms) {
        loop_span_ms = entries[stable_count - 1].ts_ms - entries[start_idx].ts_ms + 67u;
    }

    for (size_t i = start_idx; i < stable_count; ++i) {
        if (entries[i].type == VIDEO_TYPE) {
            video_entries++;
        } else if (entries[i].type == AUDIO_TYPE) {
            audio_entries++;
        }
    }

    log_line(cfg,
             "Streaming %s from ts=%" PRIu64 " to ts=%" PRIu64 " (%zu stable/%zu total entries: %zu video, %zu audio, anchor=%zu, live_delay_ms=%u)",
             cfg->ring_path,
             entries[start_idx].ts_ms,
             entries[stable_count - 1].ts_ms,
             stable_count - start_idx,
             count - start_idx,
             video_entries,
             audio_entries,
             start_idx,
             (unsigned)(cfg->static_replay ? 0u : LIVE_STABILITY_DELAY_MS));
    log_line(cfg, "Anchor quality score=%d after %d attempt(s)", anchor_score, attempts + 1);
    if (client->audio_cfg.present) {
        log_line(cfg,
                 "Audio config: AAC object=%u sample_rate=%u channels=%u config=%s",
                 client->audio_cfg.object_type,
                 client->audio_cfg.sample_rate,
                 client->audio_cfg.channels,
                 client->audio_cfg.config_hex);
    }

    if (send_range(client, cfg, &snap, entries, start_idx, stable_count, 0, 1) != 0) {
        free_snapshot(&snap);
        return -1;
    }
    free_snapshot(&snap);

    while (cfg->static_replay || cfg->loop_forever) {
        if (cfg->static_replay) {
            reset_hevc_stream(&client->hevc);
            if (read_file_fully(cfg->ring_path, &snap) != 0) {
                return -1;
            }
            count = collect_entries(&snap, entries, MAX_TABLE_ENTRIES);
            if (count == 0) {
                free_snapshot(&snap);
                return -1;
            }
            stable_count = stable_entry_count(entries, count, 0u);
            if (!client->audio_cfg.present) {
                (void)detect_audio_config_from_snapshot(&snap, &client->audio_cfg);
            }
            anchor = find_anchor_index(&snap, entries, stable_count, &anchor_score, cfg->video_structured_skip);
            if (anchor < 0) {
                free_snapshot(&snap);
                return -1;
            }
            start_idx = (size_t)anchor;
            loop_offset_ms += loop_span_ms;
            if (send_range(client, cfg, &snap, entries, start_idx, stable_count, loop_offset_ms, 1) != 0) {
                free_snapshot(&snap);
                return -1;
            }
            free_snapshot(&snap);
            continue;
        }

        sleep_ms(50);
        if (read_file_fully(cfg->ring_path, &snap) != 0) {
            return -1;
        }
        count = collect_entries(&snap, entries, MAX_TABLE_ENTRIES);
        if (count == 0) {
            free_snapshot(&snap);
            continue;
        }
        stable_count = stable_entry_count(entries, count, LIVE_STABILITY_DELAY_MS);
        if (stable_count == 0) {
            free_snapshot(&snap);
            continue;
        }
        if (!client->audio_cfg.present) {
            (void)detect_audio_config_from_snapshot(&snap, &client->audio_cfg);
        }

        for (size_t i = 0; i < stable_count; ++i) {
            if (entry_is_newer(&entries[i], &last_cursor)) {
                if (send_entry(client, cfg, &snap, &entries[i], 0) != 0) {
                    free_snapshot(&snap);
                    return -1;
                }
                last_cursor = entries[i];
            }
        }

        free_snapshot(&snap);
    }

    return 0;
}

static int listen_socket(int port)
{
    int fd = socket(AF_INET, SOCK_STREAM, 0);
    int yes = 1;
    struct sockaddr_in addr;

    if (fd < 0) {
        return -1;
    }
    setsockopt(fd, SOL_SOCKET, SO_REUSEADDR, &yes, sizeof(yes));

    memset(&addr, 0, sizeof(addr));
    addr.sin_family = AF_INET;
    addr.sin_port = htons((uint16_t)port);
    addr.sin_addr.s_addr = htonl(INADDR_ANY);

    if (bind(fd, (struct sockaddr *)&addr, sizeof(addr)) != 0) {
        close(fd);
        return -1;
    }
    if (listen(fd, 4) != 0) {
        close(fd);
        return -1;
    }
    return fd;
}

static uint32_t random_u32(void)
{
    uint32_t v = (uint32_t)rand();
    v ^= (uint32_t)rand() << 16;
    return v;
}

static void usage(const char *argv0)
{
    fprintf(stderr,
            "Usage: %s [--ring PATH] [--port N] [--video-structured-skip N] [--verbose] [--static-replay] [--loop-forever]\n",
            argv0);
}

int main(int argc, char **argv)
{
    server_config_t cfg;
    int server_fd;

    memset(&cfg, 0, sizeof(cfg));
    cfg.ring_path = DEFAULT_RING_PATH;
    cfg.port = DEFAULT_PORT;
    cfg.video_structured_skip = FRAME_HEADER_SIZE;

    for (int i = 1; i < argc; ++i) {
        if (strcmp(argv[i], "--ring") == 0 && i + 1 < argc) {
            cfg.ring_path = argv[++i];
        } else if (strcmp(argv[i], "--port") == 0 && i + 1 < argc) {
            cfg.port = atoi(argv[++i]);
        } else if (strcmp(argv[i], "--video-structured-skip") == 0 && i + 1 < argc) {
            unsigned long v = strtoul(argv[++i], NULL, 10);
            if (v > FRAME_HEADER_SIZE) {
                fprintf(stderr, "--video-structured-skip must be between 0 and %u\n", (unsigned)FRAME_HEADER_SIZE);
                return 2;
            }
            cfg.video_structured_skip = (size_t)v;
        } else if (strcmp(argv[i], "--verbose") == 0) {
            cfg.verbose = 1;
        } else if (strcmp(argv[i], "--static-replay") == 0) {
            cfg.static_replay = 1;
        } else if (strcmp(argv[i], "--loop-forever") == 0) {
            cfg.loop_forever = 1;
        } else {
            usage(argv[0]);
            return 2;
        }
    }

    signal(SIGPIPE, SIG_IGN);
    srand((unsigned)time(NULL));

    server_fd = listen_socket(cfg.port);
    if (server_fd < 0) {
        perror("listen_socket");
        return 1;
    }

    log_line(&cfg,
             "Listening on RTSP port %d using ring %s (video_structured_skip=%u)",
             cfg.port,
             cfg.ring_path,
             (unsigned)cfg.video_structured_skip);

    while (1) {
        int client_fd;
        struct sockaddr_in peer;
        socklen_t peer_len = sizeof(peer);
        client_state_t client;
        char req[4096];
        char cseq[64];
        char method[32];
        char url[256];
        char transport[256];

        client_fd = accept(server_fd, (struct sockaddr *)&peer, &peer_len);
        if (client_fd < 0) {
            if (errno == EINTR) {
                continue;
            }
            perror("accept");
            break;
        }

        memset(&client, 0, sizeof(client));
        client.fd = client_fd;
        client.video.rtp_seq = (uint16_t)(random_u32() & 0xffffu);
        client.video.rtp_ssrc = random_u32();
        client.video.rtp_ts_base = random_u32();
        client.video.rtp_channel = 0u;
        client.video.rtcp_channel = 1u;
        client.audio.rtp_seq = (uint16_t)(random_u32() & 0xffffu);
        client.audio.rtp_ssrc = random_u32();
        client.audio.rtp_ts_base = random_u32();
        client.audio.rtp_channel = 2u;
        client.audio.rtcp_channel = 3u;
        client.verbose = cfg.verbose;
        snprintf(client.session_id, sizeof(client.session_id), "%08x", random_u32());

        (void)detect_audio_config(&cfg, &client.audio_cfg);

        log_line(&cfg, "Client connected from %s", inet_ntoa(peer.sin_addr));

        for (;;) {
            memset(req, 0, sizeof(req));
            memset(cseq, 0, sizeof(cseq));
            memset(method, 0, sizeof(method));
            memset(url, 0, sizeof(url));
            memset(transport, 0, sizeof(transport));

            if (recv_request(client_fd, req, sizeof(req)) != 0) {
                break;
            }
            if (request_method(req, method, sizeof(method), url, sizeof(url)) != 0) {
                break;
            }
            if (header_value(req, "CSeq", cseq, sizeof(cseq)) != 0) {
                strcpy(cseq, "1");
            }

            log_line(&cfg, "RTSP %s %s", method, url);

            if (!url_is_main_ch(url) && strcmp(method, "OPTIONS") != 0) {
                char headers[128];
                snprintf(headers, sizeof(headers), "CSeq: %s\r\n", cseq);
                send_rtsp_response(client_fd, 404, "Not Found", headers, "");
                continue;
            }

            if (strcmp(method, "OPTIONS") == 0) {
                if (reply_options(client_fd, cseq) != 0) {
                    break;
                }
            } else if (strcmp(method, "DESCRIBE") == 0) {
                if (reply_describe(client_fd, cseq, url, &client.audio_cfg) != 0) {
                    break;
                }
            } else if (strcmp(method, "SETUP") == 0) {
                int track_id = parse_track_id_from_url(url);
                uint8_t req_rtp_channel = 0;
                uint8_t req_rtcp_channel = 0;

                if (header_value(req, "Transport", transport, sizeof(transport)) != 0) {
                    strcpy(transport, "RTP/AVP/TCP;unicast;interleaved=0-1");
                }
                if (strstr(transport, "RTP/AVP/TCP") == NULL) {
                    char headers[128];
                    snprintf(headers, sizeof(headers), "CSeq: %s\r\n", cseq);
                    send_rtsp_response(client_fd, 461, "Unsupported Transport", headers, "");
                    continue;
                }
                if (parse_interleaved_channels(transport, &req_rtp_channel, &req_rtcp_channel) != 0) {
                    if (track_id == AUDIO_TRACK_ID) {
                        req_rtp_channel = 2u;
                        req_rtcp_channel = 3u;
                    } else {
                        req_rtp_channel = 0u;
                        req_rtcp_channel = 1u;
                    }
                }

                if (track_id == AUDIO_TRACK_ID) {
                    if (!client.audio_cfg.present) {
                        char headers[128];
                        snprintf(headers, sizeof(headers), "CSeq: %s\r\n", cseq);
                        send_rtsp_response(client_fd, 404, "Not Found", headers, "");
                        continue;
                    }
                    client.audio.rtp_channel = req_rtp_channel;
                    client.audio.rtcp_channel = req_rtcp_channel;
                    client.audio.setup_done = 1;
                    if (reply_setup(client_fd,
                                    cseq,
                                    client.session_id,
                                    client.audio.rtp_channel,
                                    client.audio.rtcp_channel) != 0) {
                        break;
                    }
                } else {
                    client.video.rtp_channel = req_rtp_channel;
                    client.video.rtcp_channel = req_rtcp_channel;
                    client.video.setup_done = 1;
                    if (reply_setup(client_fd,
                                    cseq,
                                    client.session_id,
                                    client.video.rtp_channel,
                                    client.video.rtcp_channel) != 0) {
                        break;
                    }
                }
            } else if (strcmp(method, "PLAY") == 0) {
                if (!client.video.setup_done) {
                    client.video.setup_done = 1;
                }
                if (reply_play(client_fd, cseq, client.session_id, url, &client) != 0) {
                    break;
                }
                if (stream_main(&client, &cfg) != 0) {
                    log_line(&cfg, "stream_main ended: %s", strerror(errno));
                }
                break;
            } else if (strcmp(method, "TEARDOWN") == 0) {
                reply_teardown(client_fd, cseq, client.session_id);
                break;
            } else {
                char headers[128];
                snprintf(headers, sizeof(headers), "CSeq: %s\r\n", cseq);
                if (send_rtsp_response(client_fd, 501, "Not Implemented", headers, "") != 0) {
                    break;
                }
            }
        }

        reset_hevc_stream(&client.hevc);
        close(client_fd);
        log_line(&cfg, "Client disconnected");
    }

    close(server_fd);
    return 0;
}
