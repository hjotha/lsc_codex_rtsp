# Jarvis Wake Word ARMv5 Prototype Plan

Draft date: `2026-05-18`

Implementation started on `2026-05-18`. The first prototype lives under
`jarvis_wake/`; this document remains the design and refinement plan.

## Goal

Build a very small local wake-word prototype for the Anyka camera class:

- target wake word: `Jarvis`
- target CPU: `ARM926EJ-S`, ARMv5TEJ, `AK39EV330`
- target memory class: 32 MB RAM, no swap
- target firmware process: stock `anyka_ipc`
- action after detection: HTTP POST to a wake endpoint on the speech server
  host `192.168.1.70`
- implementation language: simple C / C99

The camera should only detect the wake word. Real transcription stays off
device and is handled by the Samsung speech server / Whisper path.

## Non-Goals

Do not use these in the on-camera detector:

- TensorFlow
- ONNX
- PyTorch
- Whisper
- openWakeWord
- Python runtime
- libcurl
- large dynamic allocations

This prototype is expected to be crude. The first target is to prove whether
the chip can run a tiny detector continuously without destabilizing video,
RTSP, Tuya, speaker playback, or Wi-Fi.

## Current Camera Constraints

Live checks on the `sala` and `quintal` cameras showed:

- CPU: `ARM926EJ-S rev 5`, ARMv5TEJ
- hardware: `AK39EV330`
- BogoMIPS: about `351`
- RAM: about 32 MB total
- available RAM: roughly 6-7 MB during normal operation
- no swap
- no Python, ffmpeg, sox, arecord, or aplay installed on the camera
- exposed PCM devices:
  - `/dev/akpcm_adc0`
  - `/dev/akpcm_dac0`
  - `/dev/akpcm_loopback0`
- stock `anyka_ipc` already owns the audio devices
- earlier RTSP audio captures looked effectively silent / constant because no
  useful speech was present near the camera during those captures
- the user confirmed that RTSP audio carries normal microphone audio when
  speaking at the camera
- direct second-process open of `/dev/akpcm_adc0` on the `sala` camera failed
  with `Operation not permitted`, so a standalone on-camera reader cannot just
  open the ADC device while `anyka_ipc` owns it

Important implication: the first detector should be able to read raw PCM from
`stdin`. The camera-specific audio capture path should be a replaceable input
module, not baked into the detector core.

## Native Audio/VAD Findings

The stock firmware has lightweight audio detection symbols:

- `ak_sound_detect`
- `ak_ai_enable_vad`
- `ak_ai_set_vad_attr`
- `ak_ai_get_vad_status`
- `ht_audio_codec_sd_open`
- `ht_audio_codec_sd_set_sensitivity`

These look like sound activity / VAD helpers, not keyword spotting.

Baby-cry symbols also exist, but current runtime state does not show an active
cry detector:

- `g_cry_detect_support` is `0` on both checked cameras
- `ht_tuya_func_set_cry_detect` is effectively a stub in the checked firmware

Use the native sound/VAD path only as an optional future trigger or reference.
Do not assume it can recognize `Jarvis`.

## Proposed Files

First prototype file set now exists under `jarvis_wake/`:

- `jarvis_wake/jarvis_wake.c`
- `jarvis_wake/Makefile`
- `jarvis_wake/record_template.sh`
- `jarvis_wake/run.sh`
- `jarvis_wake/templates/README.md`

`templates/jarvis_01.raw` is intentionally a local calibration artifact and is
ignored by git. It should be recorded from the camera microphone path.

## Build Strategy

The camera probably does not have ALSA. Keep two build modes:

```bash
# Minimal camera-oriented build: raw PCM on stdin, no ALSA.
gcc -O2 -std=c99 -Wall jarvis_wake.c -lm -o jarvis_wake

# Optional host/dev build if ALSA is available.
gcc -O2 -std=c99 -Wall -DJARVIS_WITH_ALSA=1 jarvis_wake.c -lasound -lm -o jarvis_wake
```

Runtime modes:

```bash
# Raw PCM path, preferred first.
cat audio.raw | ./jarvis_wake --stdin --rate 8000 --channels 1 --format s16le

# Optional ALSA/dev path if available on a host.
./jarvis_wake --alsa default --rate 16000
```

Cross-compile later with the existing Anyka/ARMv5 toolchain flow once the
algorithm works on known raw test clips.

## Audio Capture Plan

Input priority:

1. `--stdin` raw PCM, for deterministic testing and easiest camera deployment.
2. optional host ALSA input for development only.
3. host-side RTSP capture for recording templates from the real camera
   microphone, preferably through the lower-bandwidth `89/videoSub` stream
   over UDP.
4. camera-specific Anyka `/tmp/AudioStream` input for on-camera live testing.
5. deeper in-process callback hook only if the shared ring proves inaccurate.

Camera-specific input options:

- do not read `/dev/akpcm_adc0` directly while `anyka_ipc` owns it; direct open
  and `/proc/<pid>/fd/7` both failed with `Operation not permitted`
- use `/tmp/AudioStream` first: it is a stock shared PCMA/A-law ring mapped by
  `anyka_ipc`, and read-only mmap did not disturb the process
- hook or reuse the internal Anyka AI/AENC callback path only if the shared
  ring cannot provide clean wake-word input
- expose a tiny PCM stream from inside the existing `rtsp_kick`/process hook
  only as a later fallback
- use RTSP audio as the first calibration/template capture path, because it is
  confirmed to carry microphone audio when someone speaks at the camera
- prefer `rtsp://CAMERA_IP:89/videoSub` over UDP for capture; use
  `rtsp://CAMERA_IP:88/videoMain` only when comparing behavior between streams
- avoid RTSP-over-TCP for template capture unless specifically debugging the
  TCP/interleaved path; it delivered sparse audio on the `sala` tests

Do not open `/dev/akpcm_adc0` aggressively in the first camera run. The stock
process owns that device and destabilizing it would make the wake prototype
hard to evaluate.

Current `/tmp/AudioStream` layout on the checked `sala` firmware:

- descriptor current index: `u32` at `0x494`
- descriptor base: `0x4d0`
- descriptor count: `896`
- descriptor size: `24`
- payload base: `0x6000`
- payload size: `0x8000`
- observed frame length: `996` bytes
- observed encoding: PCMA/A-law
- observed source rate: about 16 kHz

The prototype supports this path with:

```bash
./jarvis_wake --ak-stream /tmp/AudioStream --rate 8000 --template templates/jarvis_01.raw
```

With `--rate 8000`, the Anyka reader decodes A-law from the 16 kHz source ring
and downsamples 2:1 before feeding the detector.

RTSP is not, by itself, the final on-camera live source because the camera does
not ship ffmpeg/sox and the detector should not grow into a full RTSP/RTP/PCMA
client unless the internal Anyka callback path proves worse. Use RTSP first for
recording real `Jarvis` templates and test clips on the host.

## Signal Format

Start with:

- mono
- signed 16-bit little-endian PCM
- 8000 Hz preferred
- 16000 Hz accepted if the capture path naturally provides it

Reasoning:

- 8 kHz keeps CPU and RAM low
- the wake word is speech-band limited enough for a prototype
- one second at 8 kHz s16le is only 16 KB
- one second at 16 kHz s16le is 32 KB

## Runtime Pipeline

High-level loop:

1. read PCM frames
2. compute frame energy and zero-crossing rate
3. run simple VAD state machine
4. collect a short utterance around detected speech
5. trim leading/trailing silence
6. extract low-cost features
7. compare against one or more `Jarvis` templates using DTW
8. if score passes threshold, POST wake event to `192.168.1.70`

Frame settings for v1:

- frame length: 20 ms
  - 160 samples at 8 kHz
  - 320 samples at 16 kHz
- hop length: 10 ms or 20 ms
- utterance window: initially 1.0-1.2 seconds
- pre-roll: 100-200 ms if using a circular buffer

## VAD v1

Use energy-only VAD first:

- maintain a noise floor estimate during non-speech
- mark speech when frame energy exceeds `noise_floor * ratio`
- require several consecutive speech frames before starting capture
- require several consecutive quiet frames before ending capture
- cap utterance length to avoid runaway buffers

Optional v1.1:

- add zero-crossing sanity bounds
- reject very low energy and very high noise bursts
- reject utterances much shorter or longer than expected for `Jarvis`

## Features v1

Keep the first feature extractor intentionally small:

- log/RMS energy per frame
- zero-crossing rate per frame
- optional delta energy
- optional 3-4 crude band energies if CPU allows it

Avoid MFCC in the first camera test. MFCC is possible in C, but it adds FFT or
filterbank code, more tuning, and more CPU. Add it only after the simple feature
set proves that the device can run the loop continuously.

Represent features as fixed-point integers where possible. Floating point is
acceptable for a host prototype, but the ARMv5 camera may not have useful FPU
performance.

## Template Matching

Template storage:

- start with `templates/jarvis_01.raw`
- later support multiple templates:
  - `templates/jarvis_01.raw`
  - `templates/jarvis_02.raw`
  - `templates/jarvis_03.raw`

At startup:

- load each raw template
- extract the same features used for live audio
- keep feature arrays in small fixed-size buffers

Matching:

- compare live utterance features to template features with simple DTW
- use a Sakoe-Chiba band or max path width to bound CPU
- normalize score by path length
- accept if score is below threshold

Initial DTW constraints:

- max frames per utterance: about 120
- max template frames: about 120
- local distance: weighted absolute difference between feature dimensions
- memory: use two DTW rows, not a full matrix

## Speech Server Endpoint Reality Check

Validated on `2026-05-18`:

- the main Android speech-server responds on `http://192.168.1.70:18070`
- `GET http://192.168.1.70:18070/health` reports `"port": 18070`
- `192.168.1.70:8080` is not listening
- `POST http://192.168.1.70:18070/wake` currently returns `404 not_found`
- the checked speech-server code exposes:
  - `GET /health`
  - `POST /v1/tts`
  - `POST /v1/stt/file`
  - websocket `/v1/stt/stream`

So the detector must not hardcode `192.168.1.70:8080/wake`.

For this prototype, make the wake target configurable:

- default host: `192.168.1.70`
- default port: `18070`
- default path: `/v1/wake`

`/v1/wake` does not exist yet. It is the proposed endpoint to add to the
existing speech-server later, or to serve with a separate tiny receiver during
testing. The on-camera detector should only need host, port, and path strings.

## HTTP Wake POST

Use raw TCP sockets. No libcurl.

Proposed endpoint after the speech-server side is added:

```text
POST http://192.168.1.70:18070/v1/wake
```

Payload:

```json
{
  "wake_word": "jarvis",
  "confidence": SCORE,
  "timestamp": UNIX_TIME
}
```

Implementation details:

- `socket(AF_INET, SOCK_STREAM, 0)`
- `connect()` to configurable host/port; default `192.168.1.70:18070`
- write a minimal HTTP/1.1 request
- include `Content-Type: application/json`
- include `Content-Length`
- read a small response or close after send
- use short timeouts so detection cannot block the audio loop for long

If `time()` is unreliable on a camera before NTP sync, allow timestamp `0` or
monotonic seconds as a fallback.

## Memory Budget

Target rough budget:

- audio circular buffer: 16-64 KB
- feature buffer: under 16 KB
- template features: under 32 KB for a few templates
- DTW rows: under 8 KB
- total detector working memory: ideally under 128 KB

Avoid heap-heavy design. Prefer fixed arrays with explicit maximums.

## CPU Budget

The detector must run below a small fraction of the CPU. The camera already
runs video, RTSP, Wi-Fi, Tuya, and audio services on one old core.

First benchmark targets:

- process 60 seconds of raw audio faster than real time on the camera
- then run continuously for 10-30 minutes while RTSP stays healthy
- check that `anyka_ipc` PID remains stable
- check that ports `88`, `89`, `24`, and `6668` remain available

## Performance Engineering for ARMv5TEJ

The `ARM926EJ-S` on `AK39EV330` has no NEON, no SIMD, no hardware integer
divider, and likely no usable FPU. The pipeline is 5-stage in-order
single-issue. Caches are small (commonly 16 KB I-cache + 16 KB D-cache, sometimes
less). Every cycle matters. Apply the following to keep the detector well below
10% CPU while video, RTSP, Wi-Fi, Tuya and audio continue normally.

### Toolchain Flags

Use the existing ARMv5 cross toolchain. Suggested compile flags:

```
-O2 -mcpu=arm926ej-s -mtune=arm926ej-s -marm -mthumb-interwork
-msoft-float -fno-math-errno -fomit-frame-pointer
-ffunction-sections -fdata-sections -Wl,--gc-sections -flto
-Wa,-mcpu=arm926ej-s
```

Notes:

- prefer `-O2` over `-O3`; `-O3` can bloat hot code past the I-cache
- consider `-Os` for the audio frame loop only if `objdump`/profiling shows
  I-cache pressure
- keep hot inner loops as ARM (`-marm`); switch to `-mthumb` per-file for cold
  code (HTTP path, argv parsing, init) to shrink overall size
- `-flto` lets the linker inline tiny helpers across translation units
- `-Wa,-mcpu=arm926ej-s` so the assembler accepts ARMv5E DSP mnemonics
- do NOT pass `-fno-strict-aliasing` blindly; it disables real optimizations

### Avoid These in the Hot Loop

These are expensive on a softfloat ARMv5:

- `log`, `logf`, `log10`, `exp`, `expf`
- `sqrt`, `sqrtf` (compare squared distances instead)
- floating-point divide and modulo
- 64-bit integer divide
- any `malloc` / `free`
- any `printf` / `snprintf`
- any blocking syscall (`connect`, `send`, `gethostbyname`, file open) on the
  audio thread

The "log/RMS energy" choice in the Features v1 section is misleading on this
chip: both `log` and `sqrt` are softfloat helpers. For v1 prefer **squared
energy** (`sum of x*x`) and compare ratios in fixed-point. Add `log`/`sqrt`
only after profiling proves the simpler form is insufficient.

### Fixed-Point Math and DSP Instructions

ARMv5TEJ has the "E" DSP extensions: `SMLAxy`, `SMULxy`, `SMLALxy`. These do a
single-cycle 16×16 multiply-accumulate into a 32-bit or 64-bit accumulator —
exactly what is needed by:

- energy accumulation (sum of squares of int16 samples)
- DTW local distances (sum of squared or absolute feature differences)
- low-pass / pre-emphasis filters

Represent samples as `int16_t` and features as Q15 (or Q7 if memory is tight).
GCC emits `SMLA*` patterns automatically for `(int32_t)a * (int32_t)b` where
both operands were originally `int16_t`. Verify with `arm-linux-objdump -d`.
If GCC misses it, write a handful of lines of inline asm for the DTW inner
loop only.

### Cache and Data Layout

- keep the entire frame processing loop body well under 16 KB of code
- pad all feature buffers, DTW rows, and template features to 32-byte
  boundaries (`__attribute__((aligned(32)))`)
- make the audio circular buffer a power of two so wrap is `idx & (N-1)`,
  never a `%` (no hardware divide)
- pick frame length as a power of two when the capture path allows it (128
  or 256 samples at 8 kHz)
- store template features contiguously in the same memory layout as live
  features so DTW does stride-1 reads
- mark inner-loop pointers with `restrict` so the compiler can hoist loads

### DTW Micro-Optimizations

- enforce a **Sakoe-Chiba band** of 10-15 frames; this drops cost from
  ~14400 cells per match to ~3000
- use squared difference as the local distance; skip `sqrt`
- **early-exit**: track the running minimum across the current DTW row; if
  it already exceeds the rejection threshold, abandon the match
- reuse two int32 rows across calls; never `malloc` inside DTW
- with multiple templates, run them serially and abort remaining templates
  as soon as one passes

### I/O and Syscall Amortization

- read PCM from `stdin` in chunks of ≥1024 samples (≥2 KB at s16le); one
  `read()` per 20 ms frame wastes context switches
- batch logs: write once per detection event, not once per frame
- run HTTP POST on a **dedicated worker thread** with a single-slot
  mailbox (one `pthread_mutex_t` + condition variable, or a one-deep ring);
  the audio loop must never block on `connect()` or `send()`
- pre-resolve the speech server IP once at startup; cache the
  `struct sockaddr_in`
- use a short `SO_SNDTIMEO` / `SO_RCVTIMEO` so a wedged server cannot leak
  worker threads

### Scheduling and Priority

- start the detector at `nice -n 5` (lower priority than `anyka_ipc`) so
  video and RTSP stay smooth; only raise if the loop falls behind
- do **not** use `SCHED_FIFO` on the audio loop; it can starve `anyka_ipc`,
  which is exactly the failure mode this prototype must avoid
- run the HTTP worker thread at `nice -n 10`
- `mlock()` the large feature/template arrays if RAM headroom allows; a
  single major fault during a wake event causes a 100+ ms hiccup

### Profiling on Device

The camera has no `perf` and probably no `gprof`. Use:

- `clock_gettime(CLOCK_MONOTONIC, ...)` around each pipeline stage,
  reported as a small histogram every N seconds
- frames-processed-per-wall-second counter
- `/proc/self/status` peak RSS once per minute
- offline `gprof` only on the host replay build, not on the camera

Hard target: each 10 ms hop must complete in **≤ 1 ms** of CPU. If a hop
costs more than 2 ms, the detector is too heavy for continuous operation
alongside `anyka_ipc`, and a feature must be cut before going further.

### Stretch Optimizations

Try these only if the simple loop is still too heavy:

- inline ARMv5E asm using `SMLAL` for the DTW inner row and energy sum
- downsample to 4 kHz for the VAD path only; switch to 8 kHz feature
  extraction after VAD opens (cuts VAD cost ~50%)
- store templates as `int8` deltas plus a per-template gain; halves
  template RAM and halves DTW load traffic
- replace `time()` in the wake payload with a cached `times(NULL)`-based
  monotonic counter to avoid syscall cost on the hot path
- consider linking with `-Bstatic` against musl/uclibc to drop dynamic
  loader cost at boot and shrink the runtime footprint

### Plan Adjustments Implied by the Above

Concrete changes to apply when implementation starts:

1. **Features v1** → `squared energy + ZCR`, not "log/RMS"; defer `log`/`sqrt`
2. **HTTP Wake POST** → mandatory worker thread + one-slot mailbox; audio
   loop never blocks on the network
3. **CPU Budget / First benchmark** → require **≥10× real-time** on the
   camera, not merely "faster than real time"
4. add a `--profile` flag that prints per-stage timing histograms
5. add a `--no-net` flag that skips the HTTP POST so the detector can be
   load-tested in isolation

## Calibration Plan

Template recording:

1. record several `Jarvis` samples from the intended microphone path
2. normalize to the selected sample rate / PCM format
3. keep one template first, then test multiple templates

Threshold tuning:

- positive set: several real `Jarvis` recordings
- negative set: silence, room noise, normal speech without `Jarvis`
- choose a conservative threshold that avoids constant false triggers

Log each candidate:

- utterance duration
- VAD start/end
- score
- accepted/rejected
- best matching template

## Validation Plan

Stage 1: host/offline

- run `jarvis_wake --stdin` against saved raw files
- confirm VAD segments speech
- confirm template DTW produces lower scores for `Jarvis` than negatives

Stage 2: camera/offline

- copy the static/small binary and raw clips to SD or `/tmp`
- run `cat clip.raw | ./jarvis_wake --stdin`
- measure runtime, memory, and stability

Stage 3: camera/live audio

- attach the safest available live PCM source
- run continuously with logs
- verify no restart of `anyka_ipc`
- verify RTSP still works

Stage 4: server trigger

- run the speech server wake endpoint on `192.168.1.70:18070/v1/wake`
- verify HTTP POST delivery
- after POST, let the server handle real STT from its chosen audio source

## Open Questions

- What is the safest live PCM source on the camera?
- Does `/dev/akpcm_adc0` allow a second reader, or is it exclusive to
  `anyka_ipc`?
- Can the internal Anyka AI/AENC callback be reused without destabilizing the
  process?
- Is RTSP audio silent because the mic path is disabled, muted, or connected to
  a placeholder source?
- Is 8 kHz enough for acceptable `Jarvis` separation in the real room?
- How many templates are needed before false negatives become acceptable?
- Will fixed-point features be necessary, or is limited float use tolerable?

## Recommended First Implementation Slice

When implementation starts, keep the first slice narrow:

1. `--stdin` raw PCM reader
2. energy VAD
3. energy + zero-crossing features
4. two-row bounded DTW
5. one raw template
6. socket-only HTTP POST
7. verbose logs for score and timing

Do not start with camera live capture. Prove the detector loop and DTW cost on
known raw audio first, then connect it to the Anyka audio path.
