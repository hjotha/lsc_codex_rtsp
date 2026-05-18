#define _POSIX_C_SOURCE 200809L

#include <arpa/inet.h>
#include <errno.h>
#include <fcntl.h>
#include <netinet/in.h>
#include <pthread.h>
#include <signal.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <sys/time.h>
#include <time.h>
#include <unistd.h>

#define MAX_RATE 16000
#define MIN_RATE 8000
#define MAX_FRAME_SAMPLES 320
#define MAX_PRE_SAMPLES 4096
#define MAX_UTT_SAMPLES 32768
#define MAX_FEATURE_FRAMES 220
#define MAX_TEMPLATES 8
#define READ_SAMPLES 2048
#define FEATURE_SCALE 1024U
#define INF_COST 0x3fffffffU
#define AK_STREAM_HEADER_OFFSET 0x490
#define AK_STREAM_DESC_BASE 0x4d0
#define AK_STREAM_PAYLOAD_BASE 0x6000
#define AK_STREAM_PAYLOAD_SIZE 0x8000
#define AK_STREAM_DESC_SIZE 24
#define AK_STREAM_DESC_COUNT 896
#define AK_STREAM_MAX_FRAME_BYTES 2048
#define AK_STREAM_MAX_BACKLOG 32
#define AK_STREAM_LAG_ENTRIES 1
#define AK_STREAM_POLL_MS 20

typedef struct {
    uint16_t energy;
    uint16_t zcr;
} feature_t;

typedef struct {
    char path[256];
    feature_t features[MAX_FEATURE_FRAMES];
    int feature_count;
} template_t;

typedef struct {
    bool use_stdin;
    bool use_ak_stream;
    bool no_net;
    bool profile;
    bool verbose;
    bool once;
    bool eval_max_len;
    int sample_rate;
    int threshold;
    int band;
    int vad_ratio;
    int min_energy;
    int pre_ms;
    int min_utterance_ms;
    int max_utterance_ms;
    int start_frames;
    int end_frames;
    int ak_source_rate;
    char host[64];
    int port;
    char path[128];
    char ak_stream_path[128];
    char template_paths[MAX_TEMPLATES][256];
    int template_count;
} config_t;

typedef struct {
    char wake_word[16];
    int score;
    int threshold;
    int confidence_milli;
    long timestamp;
} wake_event_t;

typedef struct {
    bool enabled;
    bool stop;
    bool pending;
    bool started;
    pthread_t thread;
    pthread_mutex_t mutex;
    pthread_cond_t cond;
    wake_event_t event;
    char host[64];
    int port;
    char path[128];
} net_worker_t;

typedef struct {
    const config_t *cfg;
    template_t templates[MAX_TEMPLATES];
    int template_count;
    int frame_len;
    int max_utt_samples;
    int min_utt_samples;
    int pre_capacity;
    int pre_mask;
    int pre_write;
    int pre_count;
    int16_t pre_ring[MAX_PRE_SAMPLES];
    int16_t frame[MAX_FRAME_SAMPLES];
    int frame_fill;
    int16_t utterance[MAX_UTT_SAMPLES];
    int utterance_count;
    bool active;
    int speech_run;
    int quiet_run;
    uint32_t noise_floor;
    uint64_t frames_seen;
    uint64_t utterances_seen;
    uint64_t accepted_seen;
    uint64_t frame_us;
    uint64_t eval_us;
    uint64_t dtw_us;
} detector_t;

typedef struct {
    uint32_t stamp;
    uint32_t unk;
    uint32_t payload_offset;
    uint32_t length;
    uint32_t type;
    uint32_t seq;
} ak_stream_entry_t;

static volatile sig_atomic_t g_stop = 0;
static uint32_t g_dtw_prev[MAX_FEATURE_FRAMES + 1];
static uint32_t g_dtw_cur[MAX_FEATURE_FRAMES + 1];
static uint32_t g_energy_tmp[MAX_FEATURE_FRAMES];
static uint16_t g_zcr_tmp[MAX_FEATURE_FRAMES];
static int16_t g_template_samples[MAX_UTT_SAMPLES];
static net_worker_t g_net;

static void on_signal(int sig)
{
    (void)sig;
    g_stop = 1;
}

static uint64_t now_us(void)
{
    struct timespec ts;
    if (clock_gettime(CLOCK_MONOTONIC, &ts) == 0) {
        return (uint64_t)ts.tv_sec * 1000000ULL + (uint64_t)ts.tv_nsec / 1000ULL;
    }
    return 0;
}

static void sleep_ms(int ms)
{
    struct timespec ts;
    ts.tv_sec = ms / 1000;
    ts.tv_nsec = (long)(ms % 1000) * 1000000L;
    while (nanosleep(&ts, &ts) != 0 && errno == EINTR && !g_stop) {
    }
}

static void usage(FILE *out, const char *argv0)
{
    fprintf(out,
            "usage: %s --stdin --template templates/jarvis_01.raw [options]\n"
            "\n"
            "Tiny fixed-point-ish Jarvis wake-word prototype for weak ARMv5 cameras.\n"
            "Input is signed 16-bit little-endian mono PCM.\n"
            "\n"
            "options:\n"
            "  --stdin                  read raw PCM from stdin (default)\n"
            "  --ak-stream PATH         read Anyka /tmp/AudioStream shared PCMA ring\n"
            "  --ak-source-rate N       Anyka ring source rate, 8000 or 16000 (default: 16000)\n"
            "  --template PATH          raw Jarvis template, may be repeated\n"
            "  --rate N                 sample rate, 8000 or 16000 (default: 8000)\n"
            "  --threshold N            DTW accept threshold; lower is stricter (default: 90000)\n"
            "  --band N                 DTW Sakoe-Chiba band in frames (default: 15)\n"
            "  --vad-ratio N            speech threshold = noise_floor * N (default: 5)\n"
            "  --min-energy N           absolute minimum frame energy (default: 20)\n"
            "  --host IP                wake endpoint host (default: 192.168.1.70)\n"
            "  --port N                 wake endpoint port (default: 18070)\n"
            "  --path PATH              wake endpoint path (default: /v1/wake)\n"
            "  --no-net                 do not POST; only log accepted detections\n"
            "  --profile                print timing summary at exit\n"
            "  --verbose                log rejected utterances too\n"
            "  --once                   exit after first accepted wake\n"
            "  --eval-max-len           also evaluate utterances that hit max length\n"
            "  -h, --help               show this help\n",
            argv0);
}

static int parse_int(const char *text, int *out)
{
    char *end = NULL;
    long value;

    errno = 0;
    value = strtol(text, &end, 0);
    if (errno != 0 || end == text || *end != '\0' || value < -2147483647L || value > 2147483647L) {
        return -1;
    }
    *out = (int)value;
    return 0;
}

static int copy_string(char *dst, size_t dst_size, const char *src)
{
    size_t len = strlen(src);
    if (len >= dst_size) {
        return -1;
    }
    memcpy(dst, src, len + 1);
    return 0;
}

static void default_config(config_t *cfg)
{
    memset(cfg, 0, sizeof(*cfg));
    cfg->use_stdin = true;
    cfg->sample_rate = 8000;
    cfg->threshold = 90000;
    cfg->band = 15;
    cfg->vad_ratio = 5;
    cfg->min_energy = 20;
    cfg->pre_ms = 200;
    cfg->min_utterance_ms = 250;
    cfg->max_utterance_ms = 1400;
    cfg->start_frames = 3;
    cfg->end_frames = 12;
    cfg->ak_source_rate = 16000;
    copy_string(cfg->host, sizeof(cfg->host), "192.168.1.70");
    cfg->port = 18070;
    copy_string(cfg->path, sizeof(cfg->path), "/v1/wake");
}

static int parse_args(config_t *cfg, int argc, char **argv)
{
    int i;

    for (i = 1; i < argc; i++) {
        const char *arg = argv[i];
        if (strcmp(arg, "-h") == 0 || strcmp(arg, "--help") == 0) {
            usage(stdout, argv[0]);
            exit(0);
        } else if (strcmp(arg, "--stdin") == 0) {
            cfg->use_stdin = true;
            cfg->use_ak_stream = false;
        } else if (strcmp(arg, "--ak-stream") == 0 && i + 1 < argc) {
            cfg->use_stdin = false;
            cfg->use_ak_stream = true;
            if (copy_string(cfg->ak_stream_path, sizeof(cfg->ak_stream_path), argv[++i]) != 0) {
                fprintf(stderr, "ak stream path too long\n");
                return -1;
            }
        } else if (strcmp(arg, "--no-net") == 0) {
            cfg->no_net = true;
        } else if (strcmp(arg, "--profile") == 0) {
            cfg->profile = true;
        } else if (strcmp(arg, "--verbose") == 0) {
            cfg->verbose = true;
        } else if (strcmp(arg, "--once") == 0) {
            cfg->once = true;
        } else if (strcmp(arg, "--eval-max-len") == 0) {
            cfg->eval_max_len = true;
        } else if (strcmp(arg, "--template") == 0 && i + 1 < argc) {
            if (cfg->template_count >= MAX_TEMPLATES) {
                fprintf(stderr, "too many templates; max=%d\n", MAX_TEMPLATES);
                return -1;
            }
            if (copy_string(cfg->template_paths[cfg->template_count],
                            sizeof(cfg->template_paths[cfg->template_count]),
                            argv[++i]) != 0) {
                fprintf(stderr, "template path too long\n");
                return -1;
            }
            cfg->template_count++;
        } else if (strcmp(arg, "--rate") == 0 && i + 1 < argc) {
            if (parse_int(argv[++i], &cfg->sample_rate) != 0) {
                fprintf(stderr, "invalid --rate\n");
                return -1;
            }
        } else if (strcmp(arg, "--ak-source-rate") == 0 && i + 1 < argc) {
            if (parse_int(argv[++i], &cfg->ak_source_rate) != 0) {
                fprintf(stderr, "invalid --ak-source-rate\n");
                return -1;
            }
        } else if (strcmp(arg, "--threshold") == 0 && i + 1 < argc) {
            if (parse_int(argv[++i], &cfg->threshold) != 0 || cfg->threshold <= 0) {
                fprintf(stderr, "invalid --threshold\n");
                return -1;
            }
        } else if (strcmp(arg, "--band") == 0 && i + 1 < argc) {
            if (parse_int(argv[++i], &cfg->band) != 0 || cfg->band < 1) {
                fprintf(stderr, "invalid --band\n");
                return -1;
            }
        } else if (strcmp(arg, "--vad-ratio") == 0 && i + 1 < argc) {
            if (parse_int(argv[++i], &cfg->vad_ratio) != 0 || cfg->vad_ratio < 1) {
                fprintf(stderr, "invalid --vad-ratio\n");
                return -1;
            }
        } else if (strcmp(arg, "--min-energy") == 0 && i + 1 < argc) {
            if (parse_int(argv[++i], &cfg->min_energy) != 0 || cfg->min_energy < 0) {
                fprintf(stderr, "invalid --min-energy\n");
                return -1;
            }
        } else if (strcmp(arg, "--host") == 0 && i + 1 < argc) {
            if (copy_string(cfg->host, sizeof(cfg->host), argv[++i]) != 0) {
                fprintf(stderr, "host too long\n");
                return -1;
            }
        } else if (strcmp(arg, "--port") == 0 && i + 1 < argc) {
            if (parse_int(argv[++i], &cfg->port) != 0 || cfg->port <= 0 || cfg->port > 65535) {
                fprintf(stderr, "invalid --port\n");
                return -1;
            }
        } else if (strcmp(arg, "--path") == 0 && i + 1 < argc) {
            if (copy_string(cfg->path, sizeof(cfg->path), argv[++i]) != 0 || cfg->path[0] != '/') {
                fprintf(stderr, "invalid --path\n");
                return -1;
            }
        } else {
            fprintf(stderr, "unknown or incomplete option: %s\n", arg);
            return -1;
        }
    }

    if (cfg->sample_rate != 8000 && cfg->sample_rate != 16000) {
        fprintf(stderr, "--rate must be 8000 or 16000\n");
        return -1;
    }
    if (cfg->ak_source_rate != 8000 && cfg->ak_source_rate != 16000) {
        fprintf(stderr, "--ak-source-rate must be 8000 or 16000\n");
        return -1;
    }
    if (cfg->use_ak_stream &&
        !(cfg->ak_source_rate == cfg->sample_rate ||
          (cfg->ak_source_rate == 16000 && cfg->sample_rate == 8000))) {
        fprintf(stderr, "unsupported ak stream conversion: source=%d detector=%d\n",
                cfg->ak_source_rate, cfg->sample_rate);
        return -1;
    }
    if (cfg->template_count == 0) {
        fprintf(stderr, "at least one --template is required\n");
        return -1;
    }
    return 0;
}

static void frame_metrics(const int16_t *samples, int n, uint32_t *energy, uint16_t *zcr)
{
    uint32_t sum = 0;
    int zc = 0;
    int prev_positive = samples[0] >= 0;
    int i;

    for (i = 0; i < n; i++) {
        int32_t x = (int32_t)samples[i] >> 4;
        int positive = samples[i] >= 0;
        sum += (uint32_t)(x * x);
        if (i > 0 && positive != prev_positive) {
            zc++;
        }
        prev_positive = positive;
    }

    *energy = sum / (uint32_t)n;
    *zcr = (uint16_t)((zc * (int)FEATURE_SCALE) / n);
}

static int extract_features(const int16_t *samples, int sample_count, int frame_len,
                            feature_t *out, int *out_count)
{
    int raw_frames = sample_count / frame_len;
    uint32_t max_energy = 0;
    uint32_t trim_threshold;
    int start = 0;
    int end;
    int i;
    int n = 0;

    if (raw_frames > MAX_FEATURE_FRAMES) {
        raw_frames = MAX_FEATURE_FRAMES;
    }
    if (raw_frames < 3) {
        return -1;
    }

    for (i = 0; i < raw_frames; i++) {
        frame_metrics(samples + i * frame_len, frame_len, &g_energy_tmp[i], &g_zcr_tmp[i]);
        if (g_energy_tmp[i] > max_energy) {
            max_energy = g_energy_tmp[i];
        }
    }
    if (max_energy == 0) {
        return -1;
    }

    trim_threshold = max_energy / 25U + 1U;
    end = raw_frames - 1;
    while (start < raw_frames && g_energy_tmp[start] < trim_threshold) {
        start++;
    }
    while (end > start && g_energy_tmp[end] < trim_threshold) {
        end--;
    }
    if (end - start + 1 < 3) {
        return -1;
    }

    for (i = start; i <= end && n < MAX_FEATURE_FRAMES; i++) {
        uint32_t norm_energy = (uint32_t)(((uint64_t)g_energy_tmp[i] * FEATURE_SCALE) / max_energy);
        if (norm_energy > 65535U) {
            norm_energy = 65535U;
        }
        out[n].energy = (uint16_t)norm_energy;
        out[n].zcr = g_zcr_tmp[i];
        n++;
    }
    *out_count = n;
    return 0;
}

static int load_template(const char *path, int frame_len, template_t *templ)
{
    FILE *fp;
    size_t got;
    int sample_count;

    fp = fopen(path, "rb");
    if (fp == NULL) {
        fprintf(stderr, "failed to open template %s: %s\n", path, strerror(errno));
        return -1;
    }
    got = fread(g_template_samples, sizeof(g_template_samples[0]), MAX_UTT_SAMPLES, fp);
    if (ferror(fp)) {
        fprintf(stderr, "failed to read template %s\n", path);
        fclose(fp);
        return -1;
    }
    fclose(fp);

    sample_count = (int)got;
    if (sample_count < frame_len * 3) {
        fprintf(stderr, "template too short: %s\n", path);
        return -1;
    }
    memset(templ, 0, sizeof(*templ));
    if (copy_string(templ->path, sizeof(templ->path), path) != 0) {
        return -1;
    }
    if (extract_features(g_template_samples, sample_count, frame_len,
                         templ->features, &templ->feature_count) != 0) {
        fprintf(stderr, "template has no usable speech features: %s\n", path);
        return -1;
    }
    fprintf(stderr, "loaded template path=%s samples=%d features=%d\n",
            path, sample_count, templ->feature_count);
    return 0;
}

static uint32_t feature_distance(feature_t a, feature_t b)
{
    int de = (int)a.energy - (int)b.energy;
    int dz = (int)a.zcr - (int)b.zcr;
    if (de < 0) {
        de = -de;
    }
    if (dz < 0) {
        dz = -dz;
    }
    return (uint32_t)(de * de + 3 * dz * dz);
}

static uint32_t min3(uint32_t a, uint32_t b, uint32_t c)
{
    uint32_t m = a < b ? a : b;
    return m < c ? m : c;
}

static uint32_t dtw_score(const feature_t *a, int an, const feature_t *b, int bn,
                          int band, uint32_t normalized_reject_threshold)
{
    uint32_t *prev = g_dtw_prev;
    uint32_t *cur = g_dtw_cur;
    uint32_t reject_total = normalized_reject_threshold * (uint32_t)(an > bn ? an : bn);
    int i;
    int j;

    if (an <= 0 || bn <= 0) {
        return INF_COST;
    }
    if (band < abs(an - bn)) {
        band = abs(an - bn);
    }
    if (band > MAX_FEATURE_FRAMES) {
        band = MAX_FEATURE_FRAMES;
    }

    for (j = 0; j <= bn; j++) {
        prev[j] = INF_COST;
    }
    prev[0] = 0;

    for (i = 1; i <= an; i++) {
        int start = i - band;
        int end = i + band;
        uint32_t row_min = INF_COST;

        if (start < 1) {
            start = 1;
        }
        if (end > bn) {
            end = bn;
        }
        for (j = 0; j <= bn; j++) {
            cur[j] = INF_COST;
        }
        for (j = start; j <= end; j++) {
            uint32_t local = feature_distance(a[i - 1], b[j - 1]);
            uint32_t best = min3(prev[j], cur[j - 1], prev[j - 1]);
            if (best >= INF_COST - local) {
                cur[j] = INF_COST;
            } else {
                cur[j] = best + local;
            }
            if (cur[j] < row_min) {
                row_min = cur[j];
            }
        }
        if (row_min > reject_total) {
            return normalized_reject_threshold + 1U;
        }
        {
            uint32_t *tmp = prev;
            prev = cur;
            cur = tmp;
        }
    }

    if (prev[bn] >= INF_COST) {
        return INF_COST;
    }
    return prev[bn] / (uint32_t)(an > bn ? an : bn);
}

static int send_all(int fd, const char *buf, size_t len)
{
    size_t off = 0;
    while (off < len) {
        ssize_t n = send(fd, buf + off, len - off, 0);
        if (n < 0) {
            if (errno == EINTR) {
                continue;
            }
            return -1;
        }
        if (n == 0) {
            return -1;
        }
        off += (size_t)n;
    }
    return 0;
}

static int post_wake_event(const net_worker_t *worker, const wake_event_t *event)
{
    int fd;
    struct sockaddr_in addr;
    struct timeval tv;
    char body[192];
    char request[512];
    char response[128];
    int body_len;
    int req_len;
    ssize_t got;

    body_len = snprintf(body, sizeof(body),
                        "{\"wake_word\":\"%s\",\"confidence\":%d.%03d,"
                        "\"score\":%d,\"threshold\":%d,\"timestamp\":%ld}\n",
                        event->wake_word,
                        event->confidence_milli / 1000,
                        event->confidence_milli % 1000,
                        event->score,
                        event->threshold,
                        event->timestamp);
    if (body_len <= 0 || (size_t)body_len >= sizeof(body)) {
        return -1;
    }

    fd = socket(AF_INET, SOCK_STREAM, 0);
    if (fd < 0) {
        return -1;
    }

    tv.tv_sec = 2;
    tv.tv_usec = 0;
    setsockopt(fd, SOL_SOCKET, SO_SNDTIMEO, &tv, sizeof(tv));
    setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));

    memset(&addr, 0, sizeof(addr));
    addr.sin_family = AF_INET;
    addr.sin_port = htons((uint16_t)worker->port);
    if (inet_pton(AF_INET, worker->host, &addr.sin_addr) != 1) {
        fprintf(stderr, "wake_post unsupported host, use numeric IPv4: %s\n", worker->host);
        close(fd);
        return -1;
    }
    if (connect(fd, (struct sockaddr *)&addr, sizeof(addr)) != 0) {
        fprintf(stderr, "wake_post connect failed host=%s port=%d: %s\n",
                worker->host, worker->port, strerror(errno));
        close(fd);
        return -1;
    }

    req_len = snprintf(request, sizeof(request),
                       "POST %s HTTP/1.1\r\n"
                       "Host: %s:%d\r\n"
                       "Content-Type: application/json\r\n"
                       "Content-Length: %d\r\n"
                       "Connection: close\r\n"
                       "\r\n"
                       "%s",
                       worker->path, worker->host, worker->port, body_len, body);
    if (req_len <= 0 || (size_t)req_len >= sizeof(request)) {
        close(fd);
        return -1;
    }
    if (send_all(fd, request, (size_t)req_len) != 0) {
        fprintf(stderr, "wake_post send failed: %s\n", strerror(errno));
        close(fd);
        return -1;
    }
    got = recv(fd, response, sizeof(response) - 1, 0);
    if (got > 0) {
        response[got] = '\0';
        fprintf(stderr, "wake_post response: %.80s\n", response);
    }
    close(fd);
    return 0;
}

static void *net_worker_main(void *arg)
{
    net_worker_t *worker = (net_worker_t *)arg;

    for (;;) {
        wake_event_t event;
        pthread_mutex_lock(&worker->mutex);
        while (!worker->pending && !worker->stop) {
            pthread_cond_wait(&worker->cond, &worker->mutex);
        }
        if (!worker->pending && worker->stop) {
            pthread_mutex_unlock(&worker->mutex);
            break;
        }
        event = worker->event;
        worker->pending = false;
        pthread_mutex_unlock(&worker->mutex);

        (void)post_wake_event(worker, &event);
    }
    return NULL;
}

static int net_worker_start(net_worker_t *worker, const config_t *cfg)
{
    memset(worker, 0, sizeof(*worker));
    if (cfg->no_net) {
        return 0;
    }
    worker->enabled = true;
    copy_string(worker->host, sizeof(worker->host), cfg->host);
    worker->port = cfg->port;
    copy_string(worker->path, sizeof(worker->path), cfg->path);
    pthread_mutex_init(&worker->mutex, NULL);
    pthread_cond_init(&worker->cond, NULL);
    if (pthread_create(&worker->thread, NULL, net_worker_main, worker) != 0) {
        fprintf(stderr, "failed to start net worker\n");
        return -1;
    }
    worker->started = true;
    return 0;
}

static void net_worker_stop(net_worker_t *worker)
{
    if (!worker->started) {
        return;
    }
    pthread_mutex_lock(&worker->mutex);
    worker->stop = true;
    pthread_cond_signal(&worker->cond);
    pthread_mutex_unlock(&worker->mutex);
    pthread_join(worker->thread, NULL);
    pthread_cond_destroy(&worker->cond);
    pthread_mutex_destroy(&worker->mutex);
    worker->started = false;
}

static void submit_wake_event(const config_t *cfg, int score)
{
    wake_event_t event;

    memset(&event, 0, sizeof(event));
    copy_string(event.wake_word, sizeof(event.wake_word), "jarvis");
    event.score = score;
    event.threshold = cfg->threshold;
    if (score < cfg->threshold) {
        event.confidence_milli = ((cfg->threshold - score) * 1000) / cfg->threshold;
    }
    event.timestamp = (long)time(NULL);

    if (cfg->no_net) {
        fprintf(stderr,
                "wake accepted no_net confidence=%d.%03d score=%d threshold=%d timestamp=%ld\n",
                event.confidence_milli / 1000,
                event.confidence_milli % 1000,
                event.score,
                event.threshold,
                event.timestamp);
        return;
    }

    pthread_mutex_lock(&g_net.mutex);
    if (g_net.pending) {
        fprintf(stderr, "wake dropped: previous network event still pending\n");
    } else {
        g_net.event = event;
        g_net.pending = true;
        pthread_cond_signal(&g_net.cond);
    }
    pthread_mutex_unlock(&g_net.mutex);
}

static int next_power_of_two(int value)
{
    int out = 1;
    while (out < value && out < MAX_PRE_SAMPLES) {
        out <<= 1;
    }
    if (out > MAX_PRE_SAMPLES) {
        out = MAX_PRE_SAMPLES;
    }
    return out;
}

static void detector_init(detector_t *det, const config_t *cfg)
{
    memset(det, 0, sizeof(*det));
    det->cfg = cfg;
    det->frame_len = cfg->sample_rate / 50;
    det->max_utt_samples = (cfg->sample_rate * cfg->max_utterance_ms) / 1000;
    det->min_utt_samples = (cfg->sample_rate * cfg->min_utterance_ms) / 1000;
    det->pre_capacity = next_power_of_two((cfg->sample_rate * cfg->pre_ms) / 1000);
    det->pre_mask = det->pre_capacity - 1;
    det->noise_floor = 1;
}

static void pre_ring_push(detector_t *det, const int16_t *samples, int n)
{
    int i;
    for (i = 0; i < n; i++) {
        det->pre_ring[det->pre_write & det->pre_mask] = samples[i];
        det->pre_write++;
        if (det->pre_count < det->pre_capacity) {
            det->pre_count++;
        }
    }
}

static void utterance_append(detector_t *det, const int16_t *samples, int n)
{
    int room = det->max_utt_samples - det->utterance_count;
    if (room <= 0) {
        return;
    }
    if (n > room) {
        n = room;
    }
    memcpy(det->utterance + det->utterance_count, samples, (size_t)n * sizeof(samples[0]));
    det->utterance_count += n;
}

static void utterance_copy_preroll(detector_t *det)
{
    int start = det->pre_write - det->pre_count;
    int i;
    for (i = 0; i < det->pre_count && det->utterance_count < det->max_utt_samples; i++) {
        det->utterance[det->utterance_count++] = det->pre_ring[(start + i) & det->pre_mask];
    }
}

static void detector_reset_utterance(detector_t *det)
{
    det->active = false;
    det->speech_run = 0;
    det->quiet_run = 0;
    det->utterance_count = 0;
}

static int evaluate_utterance(detector_t *det, const char *reason)
{
    feature_t live[MAX_FEATURE_FRAMES];
    int live_count = 0;
    int best_score = (int)INF_COST;
    int best_template = -1;
    int i;
    uint64_t eval_start = now_us();
    uint64_t dtw_total = 0;
    int duration_ms = (det->utterance_count * 1000) / det->cfg->sample_rate;
    bool accepted;

    det->utterances_seen++;
    if (det->utterance_count < det->min_utt_samples) {
        if (det->cfg->verbose) {
            fprintf(stderr, "utterance rejected reason=%s duration_ms=%d too_short\n",
                    reason, duration_ms);
        }
        return 0;
    }
    if (extract_features(det->utterance, det->utterance_count, det->frame_len,
                         live, &live_count) != 0) {
        if (det->cfg->verbose) {
            fprintf(stderr, "utterance rejected reason=%s duration_ms=%d no_features\n",
                    reason, duration_ms);
        }
        return 0;
    }

    for (i = 0; i < det->template_count; i++) {
        uint64_t dtw_start = now_us();
        uint32_t score = dtw_score(live, live_count,
                                   det->templates[i].features,
                                   det->templates[i].feature_count,
                                   det->cfg->band,
                                   (uint32_t)det->cfg->threshold);
        uint64_t dtw_end = now_us();
        dtw_total += dtw_end - dtw_start;
        if (score < (uint32_t)best_score) {
            best_score = (int)score;
            best_template = i;
        }
        if (score <= (uint32_t)det->cfg->threshold) {
            break;
        }
    }

    accepted = best_score <= det->cfg->threshold;
    if (accepted || det->cfg->verbose) {
        fprintf(stderr,
                "utterance reason=%s duration_ms=%d features=%d best_score=%d "
                "threshold=%d accepted=%d template=%s\n",
                reason,
                duration_ms,
                live_count,
                best_score,
                det->cfg->threshold,
                accepted ? 1 : 0,
                best_template >= 0 ? det->templates[best_template].path : "-");
    }
    if (accepted) {
        det->accepted_seen++;
        submit_wake_event(det->cfg, best_score);
        if (det->cfg->once) {
            g_stop = 1;
        }
    }

    det->dtw_us += dtw_total;
    det->eval_us += now_us() - eval_start;
    return accepted ? 1 : 0;
}

static void detector_handle_frame(detector_t *det, const int16_t *frame)
{
    uint64_t start = now_us();
    uint32_t energy = 0;
    uint16_t zcr = 0;
    uint32_t speech_threshold;
    bool is_speech;

    frame_metrics(frame, det->frame_len, &energy, &zcr);
    (void)zcr;
    speech_threshold = det->noise_floor * (uint32_t)det->cfg->vad_ratio;
    if (speech_threshold < (uint32_t)det->cfg->min_energy) {
        speech_threshold = (uint32_t)det->cfg->min_energy;
    }
    is_speech = energy > speech_threshold;

    det->frames_seen++;
    if (det->active) {
        utterance_append(det, frame, det->frame_len);
        if (is_speech) {
            det->quiet_run = 0;
        } else {
            det->quiet_run++;
        }
        if (det->quiet_run >= det->cfg->end_frames ||
            det->utterance_count >= det->max_utt_samples) {
            if (det->quiet_run >= det->cfg->end_frames) {
                evaluate_utterance(det, "quiet");
            } else if (det->cfg->eval_max_len) {
                evaluate_utterance(det, "max_len");
            } else if (det->cfg->verbose) {
                fprintf(stderr, "utterance rejected reason=max_len duration_ms=%d\n",
                        (det->utterance_count * 1000) / det->cfg->sample_rate);
            }
            detector_reset_utterance(det);
            det->pre_count = 0;
        }
    } else {
        if (is_speech) {
            det->speech_run++;
        } else {
            det->speech_run = 0;
            det->noise_floor = (det->noise_floor * 31U + energy + 16U) / 32U;
            if (det->noise_floor < 1U) {
                det->noise_floor = 1U;
            }
        }

        if (det->speech_run >= det->cfg->start_frames) {
            det->active = true;
            det->utterance_count = 0;
            det->quiet_run = 0;
            utterance_copy_preroll(det);
            utterance_append(det, frame, det->frame_len);
        } else {
            pre_ring_push(det, frame, det->frame_len);
        }
    }
    det->frame_us += now_us() - start;
}

static void detector_process_samples(detector_t *det, const int16_t *samples, int count)
{
    int off = 0;
    while (off < count && !g_stop) {
        int need = det->frame_len - det->frame_fill;
        int take = count - off;
        if (take > need) {
            take = need;
        }
        memcpy(det->frame + det->frame_fill, samples + off, (size_t)take * sizeof(samples[0]));
        det->frame_fill += take;
        off += take;
        if (det->frame_fill == det->frame_len) {
            detector_handle_frame(det, det->frame);
            det->frame_fill = 0;
        }
    }
}

static void detector_finish(detector_t *det)
{
    if (det->active && det->utterance_count > 0) {
        evaluate_utterance(det, "eof");
        detector_reset_utterance(det);
    }
}

static int load_templates(detector_t *det, const config_t *cfg)
{
    int i;
    for (i = 0; i < cfg->template_count; i++) {
        if (load_template(cfg->template_paths[i], det->frame_len, &det->templates[i]) != 0) {
            return -1;
        }
        det->template_count++;
    }
    return 0;
}

static int run_stdin(detector_t *det)
{
    int16_t samples[READ_SAMPLES];

    while (!g_stop) {
        size_t got = fread(samples, sizeof(samples[0]), READ_SAMPLES, stdin);
        if (got > 0) {
            detector_process_samples(det, samples, (int)got);
        }
        if (got < READ_SAMPLES) {
            if (ferror(stdin)) {
                fprintf(stderr, "stdin read failed\n");
                return -1;
            }
            break;
        }
    }
    detector_finish(det);
    return 0;
}

static uint32_t read_le32(const uint8_t *p)
{
    return ((uint32_t)p[0]) |
           ((uint32_t)p[1] << 8) |
           ((uint32_t)p[2] << 16) |
           ((uint32_t)p[3] << 24);
}

static int16_t decode_alaw(uint8_t value)
{
    int sign;
    int exponent;
    int mantissa;
    int sample;

    value ^= 0x55U;
    sign = value & 0x80U;
    exponent = (value & 0x70U) >> 4;
    mantissa = value & 0x0fU;
    sample = mantissa << 4;
    if (exponent == 0) {
        sample += 8;
    } else {
        sample += 0x108;
        sample <<= exponent - 1;
    }
    return (int16_t)(sign ? sample : -sample);
}

static int ak_stream_read_index(const uint8_t *map, uint32_t *index)
{
    uint32_t value = read_le32(map + AK_STREAM_HEADER_OFFSET + 4);
    if (value >= AK_STREAM_DESC_COUNT) {
        return -1;
    }
    *index = value;
    return 0;
}

static int ak_stream_parse_entry(const uint8_t *map, uint32_t index, ak_stream_entry_t *entry)
{
    const uint8_t *p;

    if (index >= AK_STREAM_DESC_COUNT) {
        return -1;
    }
    p = map + AK_STREAM_DESC_BASE + index * AK_STREAM_DESC_SIZE;
    entry->stamp = read_le32(p + 0);
    entry->unk = read_le32(p + 4);
    entry->payload_offset = read_le32(p + 8);
    entry->length = read_le32(p + 12);
    entry->type = read_le32(p + 16);
    entry->seq = read_le32(p + 20);

    if (entry->payload_offset >= AK_STREAM_PAYLOAD_SIZE ||
        entry->length == 0 ||
        entry->length > AK_STREAM_MAX_FRAME_BYTES ||
        entry->length > AK_STREAM_PAYLOAD_SIZE) {
        return -1;
    }
    if (entry->type != 0x82U && entry->type != 0x02U) {
        return -1;
    }
    return 0;
}

static void ak_stream_process_entry(detector_t *det, const uint8_t *map,
                                    const ak_stream_entry_t *entry)
{
    int16_t samples[AK_STREAM_MAX_FRAME_BYTES];
    const uint8_t *payload = map + AK_STREAM_PAYLOAD_BASE;
    uint32_t off = entry->payload_offset;
    uint32_t i;
    int out_count = 0;

    if (det->cfg->ak_source_rate == det->cfg->sample_rate) {
        for (i = 0; i < entry->length; i++) {
            samples[out_count++] = decode_alaw(payload[off]);
            off++;
            if (off == AK_STREAM_PAYLOAD_SIZE) {
                off = 0;
            }
        }
    } else {
        for (i = 0; i + 1 < entry->length; i += 2) {
            int32_t a = decode_alaw(payload[off]);
            off++;
            if (off == AK_STREAM_PAYLOAD_SIZE) {
                off = 0;
            }
            {
                int32_t b = decode_alaw(payload[off]);
                off++;
                if (off == AK_STREAM_PAYLOAD_SIZE) {
                    off = 0;
                }
                samples[out_count++] = (int16_t)((a + b) / 2);
            }
        }
    }
    detector_process_samples(det, samples, out_count);
}

static int run_ak_stream(detector_t *det, const char *path)
{
    int fd;
    struct stat st;
    uint8_t *map;
    uint32_t last_index;
    uint32_t start_index;

    fd = open(path, O_RDONLY);
    if (fd < 0) {
        fprintf(stderr, "failed to open ak stream %s: %s\n", path, strerror(errno));
        return -1;
    }
    if (fstat(fd, &st) != 0) {
        fprintf(stderr, "failed to stat ak stream %s: %s\n", path, strerror(errno));
        close(fd);
        return -1;
    }
    if (st.st_size < AK_STREAM_PAYLOAD_BASE + AK_STREAM_PAYLOAD_SIZE) {
        fprintf(stderr, "ak stream file too small: %ld\n", (long)st.st_size);
        close(fd);
        return -1;
    }
    map = mmap(NULL, (size_t)st.st_size, PROT_READ, MAP_SHARED, fd, 0);
    if (map == MAP_FAILED) {
        fprintf(stderr, "failed to mmap ak stream %s: %s\n", path, strerror(errno));
        close(fd);
        return -1;
    }

    if (ak_stream_read_index(map, &start_index) != 0) {
        fprintf(stderr, "ak stream has invalid descriptor index\n");
        munmap(map, (size_t)st.st_size);
        close(fd);
        return -1;
    }
    last_index = (start_index + AK_STREAM_DESC_COUNT - AK_STREAM_LAG_ENTRIES) %
                 AK_STREAM_DESC_COUNT;
    fprintf(stderr,
            "ak_stream path=%s desc_count=%d payload_size=%d source_rate=%d detector_rate=%d start_index=%u\n",
            path, AK_STREAM_DESC_COUNT, AK_STREAM_PAYLOAD_SIZE,
            det->cfg->ak_source_rate, det->cfg->sample_rate, start_index);

    while (!g_stop) {
        uint32_t current_index;
        uint32_t target_index;
        uint32_t pending;
        uint32_t i;

        if (ak_stream_read_index(map, &current_index) != 0) {
            if (det->cfg->verbose) {
                fprintf(stderr, "ak_stream invalid current index\n");
            }
            sleep_ms(AK_STREAM_POLL_MS);
            continue;
        }
        target_index = (current_index + AK_STREAM_DESC_COUNT - AK_STREAM_LAG_ENTRIES) %
                       AK_STREAM_DESC_COUNT;
        pending = (target_index + AK_STREAM_DESC_COUNT - last_index) %
                  AK_STREAM_DESC_COUNT;
        if (pending == 0) {
            sleep_ms(AK_STREAM_POLL_MS);
            continue;
        }
        if (pending > AK_STREAM_MAX_BACKLOG) {
            if (det->cfg->verbose) {
                fprintf(stderr, "ak_stream backlog=%u resyncing to latest frames\n", pending);
            }
            last_index = (target_index + AK_STREAM_DESC_COUNT - AK_STREAM_MAX_BACKLOG) %
                         AK_STREAM_DESC_COUNT;
            pending = AK_STREAM_MAX_BACKLOG;
        }

        for (i = 1; i <= pending && !g_stop; i++) {
            uint32_t index = (last_index + i) % AK_STREAM_DESC_COUNT;
            ak_stream_entry_t entry;
            if (ak_stream_parse_entry(map, index, &entry) != 0) {
                if (det->cfg->verbose) {
                    fprintf(stderr, "ak_stream invalid entry index=%u\n", index);
                }
                continue;
            }
            ak_stream_process_entry(det, map, &entry);
        }
        last_index = target_index;
    }

    detector_finish(det);
    munmap(map, (size_t)st.st_size);
    close(fd);
    return 0;
}

static void print_profile(const detector_t *det)
{
    if (!det->cfg->profile) {
        return;
    }
    fprintf(stderr,
            "profile frames=%llu utterances=%llu accepted=%llu "
            "frame_avg_us=%llu eval_total_us=%llu dtw_total_us=%llu\n",
            (unsigned long long)det->frames_seen,
            (unsigned long long)det->utterances_seen,
            (unsigned long long)det->accepted_seen,
            (unsigned long long)(det->frames_seen ? det->frame_us / det->frames_seen : 0),
            (unsigned long long)det->eval_us,
            (unsigned long long)det->dtw_us);
}

int main(int argc, char **argv)
{
    config_t cfg;
    detector_t det;
    int rc = 0;

    signal(SIGINT, on_signal);
    signal(SIGTERM, on_signal);

    default_config(&cfg);
    if (parse_args(&cfg, argc, argv) != 0) {
        usage(stderr, argv[0]);
        return 2;
    }
    detector_init(&det, &cfg);
    if (load_templates(&det, &cfg) != 0) {
        return 1;
    }
    if (net_worker_start(&g_net, &cfg) != 0) {
        return 1;
    }

    if (cfg.use_ak_stream) {
        rc = run_ak_stream(&det, cfg.ak_stream_path);
    } else if (cfg.use_stdin) {
        rc = run_stdin(&det);
    } else {
        fprintf(stderr, "no input mode selected\n");
        rc = 2;
    }

    net_worker_stop(&g_net);
    print_profile(&det);
    return rc == 0 ? 0 : 1;
}
