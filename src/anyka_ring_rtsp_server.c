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
} server_config_t;

typedef struct {
    int fd;
    uint16_t rtp_seq;
    uint32_t rtp_ssrc;
    uint32_t rtp_ts_base;
    uint64_t media_ts_base_ms;
    char session_id[32];
    int verbose;
} client_state_t;

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

static int compare_entries(const void *a, const void *b)
{
    const ring_entry_t *ea = (const ring_entry_t *)a;
    const ring_entry_t *eb = (const ring_entry_t *)b;
    if (ea->seq < eb->seq) {
        return -1;
    }
    if (ea->seq > eb->seq) {
        return 1;
    }
    if (ea->ts_ms < eb->ts_ms) {
        return -1;
    }
    if (ea->ts_ms > eb->ts_ms) {
        return 1;
    }
    return 0;
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

        tmp.ts_ms = 0;
        memcpy(&tmp.ts_ms, entry + 0, sizeof(uint64_t));
        memcpy(&tmp.logical_offset, entry + 8, sizeof(uint32_t));
        memcpy(&tmp.total_len, entry + 12, sizeof(uint32_t));
        memcpy(&tmp.type, entry + 16, sizeof(uint32_t));
        memcpy(&tmp.seq, entry + 20, sizeof(uint32_t));

        if (tmp.type != 129u) {
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
            if (entries[i].seq == entries[out - 1].seq) {
                entries[out - 1] = entries[i];
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

static int frame_key_score(const uint8_t *payload, size_t len)
{
    size_t off = 0;
    int has_vps = 0;
    int has_sps = 0;
    int has_pps = 0;
    int has_idr = 0;

    while (off < len) {
        size_t sc_size = 0;
        size_t start = find_start_code(payload, len, off, &sc_size);
        size_t next_sc;
        uint8_t nal_type;

        if (start >= len || start + sc_size + 2 > len) {
            break;
        }
        next_sc = find_start_code(payload, len, start + sc_size, &sc_size);
        nal_type = (uint8_t)((payload[start + sc_size] >> 1) & 0x3f);
        if (nal_type == 32u) {
            has_vps = 1;
        } else if (nal_type == 33u) {
            has_sps = 1;
        } else if (nal_type == 34u) {
            has_pps = 1;
        } else if (nal_type == 19u || nal_type == 20u) {
            has_idr = 1;
        }
        off = (next_sc < len) ? next_sc : len;
    }

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

static ssize_t find_anchor_index(const ring_snapshot_t *snap, const ring_entry_t *entries, size_t count, int *score_out)
{
    ssize_t i;
    ssize_t best_full = -1;
    ssize_t best_fallback = -1;
    int best_fallback_score = 0;

    for (i = (ssize_t)count - 1; i >= 0; --i) {
        size_t frame_len = 0;
        uint8_t *frame = extract_frame_bytes(snap, &entries[i], &frame_len);
        int score;
        if (frame == NULL || frame_len <= FRAME_HEADER_SIZE) {
            free(frame);
            continue;
        }
        score = frame_key_score(frame + FRAME_HEADER_SIZE, frame_len - FRAME_HEADER_SIZE);
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
    return (count > 0) ? 0 : -1;
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
    header[2] = (uint8_t)((payload_len >> 8) & 0xff);
    header[3] = (uint8_t)(payload_len & 0xff);
    if (send_all(fd, header, sizeof(header)) != 0) {
        return -1;
    }
    return send_all(fd, payload, payload_len);
}

static int send_rtp_packet(client_state_t *client, const uint8_t *payload, size_t payload_len, uint32_t rtp_ts, int marker)
{
    uint8_t packet[12 + 3 + RTP_MAX_PAYLOAD];
    size_t total_len = 12 + payload_len;

    if (payload_len > sizeof(packet) - 12) {
        errno = EMSGSIZE;
        return -1;
    }

    packet[0] = 0x80;
    packet[1] = (uint8_t)(96 | (marker ? 0x80 : 0x00));
    packet[2] = (uint8_t)((client->rtp_seq >> 8) & 0xff);
    packet[3] = (uint8_t)(client->rtp_seq & 0xff);
    packet[4] = (uint8_t)((rtp_ts >> 24) & 0xff);
    packet[5] = (uint8_t)((rtp_ts >> 16) & 0xff);
    packet[6] = (uint8_t)((rtp_ts >> 8) & 0xff);
    packet[7] = (uint8_t)(rtp_ts & 0xff);
    packet[8] = (uint8_t)((client->rtp_ssrc >> 24) & 0xff);
    packet[9] = (uint8_t)((client->rtp_ssrc >> 16) & 0xff);
    packet[10] = (uint8_t)((client->rtp_ssrc >> 8) & 0xff);
    packet[11] = (uint8_t)(client->rtp_ssrc & 0xff);
    memcpy(packet + 12, payload, payload_len);
    client->rtp_seq = (uint16_t)(client->rtp_seq + 1u);
    return send_interleaved(client->fd, 0, packet, total_len);
}

static int send_hevc_nal(client_state_t *client, const uint8_t *nal, size_t nal_len, uint32_t rtp_ts, int is_last_nal)
{
    if (nal_len <= RTP_MAX_PAYLOAD) {
        return send_rtp_packet(client, nal, nal_len, rtp_ts, is_last_nal);
    }

    if (nal_len < 3) {
        errno = EINVAL;
        return -1;
    }

    {
        uint8_t nal_type = (uint8_t)((nal[0] >> 1) & 0x3f);
        uint8_t fu_indicator[2];
        size_t pos = 2;
        int first = 1;

        fu_indicator[0] = (uint8_t)((nal[0] & 0x81) | (49u << 1));
        fu_indicator[1] = nal[1];

        while (pos < nal_len) {
            uint8_t payload[3 + RTP_MAX_PAYLOAD];
            size_t frag = nal_len - pos;
            int last = 0;

            if (frag > RTP_MAX_PAYLOAD - 3) {
                frag = RTP_MAX_PAYLOAD - 3;
            } else {
                last = 1;
            }

            payload[0] = fu_indicator[0];
            payload[1] = fu_indicator[1];
            payload[2] = (uint8_t)((first ? 0x80 : 0x00) | (last ? 0x40 : 0x00) | nal_type);
            memcpy(payload + 3, nal + pos, frag);
            if (send_rtp_packet(client, payload, frag + 3, rtp_ts, last && is_last_nal) != 0) {
                return -1;
            }

            first = 0;
            pos += frag;
        }
    }

    return 0;
}

static int send_hevc_frame(client_state_t *client, const uint8_t *payload, size_t payload_len, uint32_t rtp_ts)
{
    size_t off = 0;
    while (off < payload_len) {
        size_t sc_size = 0;
        size_t start = find_start_code(payload, payload_len, off, &sc_size);
        size_t next_sc;
        size_t nal_start;
        size_t nal_len;
        int is_last = 0;

        if (start >= payload_len) {
            break;
        }
        nal_start = start + sc_size;
        next_sc = find_start_code(payload, payload_len, nal_start, &sc_size);
        if (next_sc >= payload_len) {
            next_sc = payload_len;
            is_last = 1;
        }
        while (next_sc > nal_start && payload[next_sc - 1] == 0) {
            next_sc--;
        }
        nal_len = next_sc - nal_start;
        if (nal_len > 0) {
            if (send_hevc_nal(client, payload + nal_start, nal_len, rtp_ts, is_last) != 0) {
                return -1;
            }
        }
        off = (next_sc < payload_len) ? next_sc : payload_len;
    }
    return 0;
}

static uint32_t compute_rtp_ts(client_state_t *client, uint64_t ts_ms, uint64_t offset_ms)
{
    if (client->media_ts_base_ms == 0) {
        client->media_ts_base_ms = ts_ms;
    }
    return client->rtp_ts_base + (uint32_t)(((ts_ms - client->media_ts_base_ms) + offset_ms) * 90u);
}

static int send_entry(client_state_t *client, const ring_snapshot_t *snap, const ring_entry_t *entry, uint64_t offset_ms)
{
    size_t frame_len = 0;
    uint8_t *frame = extract_frame_bytes(snap, entry, &frame_len);
    int rc;

    if (frame == NULL) {
        return -1;
    }
    if (frame_len <= FRAME_HEADER_SIZE) {
        free(frame);
        errno = EINVAL;
        return -1;
    }

    rc = send_hevc_frame(client, frame + FRAME_HEADER_SIZE, frame_len - FRAME_HEADER_SIZE, compute_rtp_ts(client, entry->ts_ms, offset_ms));
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
        if (send_entry(client, snap, &entries[i], offset_ms) != 0) {
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

static int reply_describe(int fd, const char *cseq, const char *url)
{
    char headers[256];
    char body[512];
    snprintf(body,
             sizeof(body),
             "v=0\r\n"
             "o=- 0 0 IN IP4 0.0.0.0\r\n"
             "s=Anyka Main Stream\r\n"
             "t=0 0\r\n"
             "a=control:*\r\n"
             "m=video 0 RTP/AVP 96\r\n"
             "c=IN IP4 0.0.0.0\r\n"
             "a=rtpmap:96 H265/90000\r\n"
             "a=control:%s/trackID=0\r\n",
             url);
    snprintf(headers,
             sizeof(headers),
             "CSeq: %s\r\n"
             "Content-Type: application/sdp\r\n",
             cseq);
    return send_rtsp_response(fd, 200, "OK", headers, body);
}

static int reply_setup(int fd, const char *cseq, const char *session_id, const char *transport)
{
    char headers[512];
    const char *interleaved = strstr(transport, "interleaved=");
    const char *selected = "RTP/AVP/TCP;unicast;interleaved=0-1";

    if (strstr(transport, "RTP/AVP/TCP") == NULL) {
        snprintf(headers, sizeof(headers), "CSeq: %s\r\n", cseq);
        return send_rtsp_response(fd, 461, "Unsupported Transport", headers, "");
    }

    if (interleaved != NULL) {
        static char transport_buf[128];
        const char *end = strpbrk(interleaved, ";\r\n");
        size_t len = end ? (size_t)(end - transport) : strlen(transport);
        if (len >= sizeof(transport_buf)) {
            len = sizeof(transport_buf) - 1u;
        }
        memcpy(transport_buf, transport, len);
        transport_buf[len] = '\0';
        selected = transport_buf;
    }

    snprintf(headers,
             sizeof(headers),
             "CSeq: %s\r\n"
             "Session: %s\r\n"
             "Transport: %s\r\n",
             cseq,
             session_id,
             selected);
    return send_rtsp_response(fd, 200, "OK", headers, "");
}

static int reply_play(int fd, const char *cseq, const char *session_id, const char *url, uint16_t seq, uint32_t rtptime)
{
    char headers[512];
    snprintf(headers,
             sizeof(headers),
             "CSeq: %s\r\n"
             "Session: %s\r\n"
             "RTP-Info: url=%s/trackID=0;seq=%u;rtptime=%" PRIu32 "\r\n",
             cseq,
             session_id,
             url,
             (unsigned)seq,
             rtptime);
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
    size_t count;
    ssize_t anchor;
    size_t start_idx;
    uint64_t last_sent_seq;
    uint64_t loop_offset_ms = 0;
    uint64_t loop_span_ms = 1000;
    int anchor_score = 0;
    int attempts;

    memset(&snap, 0, sizeof(snap));
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

        anchor = find_anchor_index(&snap, entries, count, &anchor_score);
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
    last_sent_seq = entries[count - 1].seq;
    client->media_ts_base_ms = entries[start_idx].ts_ms;
    if (entries[count - 1].ts_ms > entries[start_idx].ts_ms) {
        loop_span_ms = entries[count - 1].ts_ms - entries[start_idx].ts_ms + 67u;
    }

    log_line(cfg,
             "Streaming %s from seq=%" PRIu32 " to seq=%" PRIu32 " (%zu frames, anchor=%zu)",
             cfg->ring_path,
             entries[start_idx].seq,
             entries[count - 1].seq,
             count - start_idx,
             start_idx);
    log_line(cfg, "Anchor quality score=%d after %d attempt(s)", anchor_score, attempts + 1);

    if (send_range(client, &snap, entries, start_idx, count, 0, 1) != 0) {
        free_snapshot(&snap);
        return -1;
    }
    free_snapshot(&snap);

    while (cfg->static_replay || cfg->loop_forever) {
        if (cfg->static_replay) {
            if (read_file_fully(cfg->ring_path, &snap) != 0) {
                return -1;
            }
            count = collect_entries(&snap, entries, MAX_TABLE_ENTRIES);
            if (count == 0) {
                free_snapshot(&snap);
                return -1;
            }
            anchor = find_anchor_index(&snap, entries, count, &anchor_score);
            if (anchor < 0) {
                free_snapshot(&snap);
                return -1;
            }
            start_idx = (size_t)anchor;
            loop_offset_ms += loop_span_ms;
            if (send_range(client, &snap, entries, start_idx, count, loop_offset_ms, 1) != 0) {
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
        for (size_t i = 0; i < count; ++i) {
            if (entries[i].seq > last_sent_seq) {
                if (send_entry(client, &snap, &entries[i], 0) != 0) {
                    free_snapshot(&snap);
                    return -1;
                }
                last_sent_seq = entries[i].seq;
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
            "Usage: %s [--ring PATH] [--port N] [--verbose] [--static-replay] [--loop-forever]\n",
            argv0);
}

int main(int argc, char **argv)
{
    server_config_t cfg;
    int server_fd;

    memset(&cfg, 0, sizeof(cfg));
    cfg.ring_path = DEFAULT_RING_PATH;
    cfg.port = DEFAULT_PORT;

    for (int i = 1; i < argc; ++i) {
        if (strcmp(argv[i], "--ring") == 0 && i + 1 < argc) {
            cfg.ring_path = argv[++i];
        } else if (strcmp(argv[i], "--port") == 0 && i + 1 < argc) {
            cfg.port = atoi(argv[++i]);
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

    log_line(&cfg, "Listening on RTSP port %d using ring %s", cfg.port, cfg.ring_path);

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
        client.rtp_seq = (uint16_t)(random_u32() & 0xffffu);
        client.rtp_ssrc = random_u32();
        client.rtp_ts_base = random_u32();
        client.verbose = cfg.verbose;
        snprintf(client.session_id, sizeof(client.session_id), "%08x", random_u32());

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
                if (reply_describe(client_fd, cseq, url) != 0) {
                    break;
                }
            } else if (strcmp(method, "SETUP") == 0) {
                if (header_value(req, "Transport", transport, sizeof(transport)) != 0) {
                    strcpy(transport, "RTP/AVP/TCP;unicast;interleaved=0-1");
                }
                if (reply_setup(client_fd, cseq, client.session_id, transport) != 0) {
                    break;
                }
            } else if (strcmp(method, "PLAY") == 0) {
                if (reply_play(client_fd, cseq, client.session_id, url, client.rtp_seq, client.rtp_ts_base) != 0) {
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

        close(client_fd);
        log_line(&cfg, "Client disconnected");
    }

    close(server_fd);
    return 0;
}
