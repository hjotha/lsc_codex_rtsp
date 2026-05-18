# Jarvis Wake ARMv5 Findings

Date: `2026-05-18`

## Prototype State

The first prototype is implemented under `jarvis_wake/`.

Implemented:

- `stdin` raw PCM reader
- energy VAD
- squared-energy + zero-crossing features
- bounded two-row DTW
- configurable HTTP POST through raw TCP sockets
- one-slot network worker thread
- `--no-net`
- `--profile`
- host Makefile build
- ARMv5 cross-build script with fallback to `arm-linux-gnueabi-gcc`
- RTSP template recording script

Not implemented yet:

- live PCM capture inside the camera
- Anyka audio callback hook
- speech-server `/v1/wake` endpoint
- real `Jarvis` threshold calibration

## Build Findings

Host build:

```bash
make -C jarvis_wake
```

Result: passed.

ARM build:

```bash
make -C jarvis_wake anyka
```

The extracted Anyka toolchain was not present in this checkout, so the build
used the fallback `arm-linux-gnueabi-gcc`.

Result:

```text
out/jarvis_wake_arm: ELF 32-bit LSB executable, ARM, EABI5, statically linked
size: 549988 bytes
ABI: soft-float
```

## Camera Offline Replay

Test camera:

- name: `sala`
- IP: `192.168.1.130`
- `anyka_ipc` PID before/after: `567`

The ARM binary and synthetic RAW files were copied to `/tmp` with the
telnet+BusyBox `nc` uploader.

Positive synthetic replay:

```text
loaded template path=/tmp/jarvis_template.raw samples=4400 features=27
utterance reason=quiet duration_ms=1016 features=29 best_score=46990 threshold=90000 accepted=1
wake accepted no_net confidence=0.477 score=46990 threshold=90000
profile frames=77 utterances=1 accepted=1 frame_avg_us=249 eval_total_us=13933 dtw_total_us=57
```

Negative synthetic replay:

```text
loaded template path=/tmp/jarvis_template.raw samples=4400 features=27
utterance reason=quiet duration_ms=1016 features=29 best_score=90001 threshold=90000 accepted=0
profile frames=77 utterances=1 accepted=0 frame_avg_us=64 eval_total_us=3918 dtw_total_us=52
```

Interpretation:

- the ARM binary runs on the camera
- `anyka_ipc` did not restart during these offline replays
- the synthetic positive/negative split works at the default threshold
- the measured frame loop cost is comfortably below the 20 ms frame interval
  in this tiny replay
- this does not prove wake-word quality for real speech yet

## HTTP POST Validation

The raw socket POST path was validated against a local `nc` listener.

Captured request:

```http
POST /wake HTTP/1.1
Host: 127.0.0.1:18080
Content-Type: application/json
Content-Length: 97
Connection: close

{"wake_word":"jarvis","confidence":0.477,"score":46990,"threshold":90000,"timestamp":1779098167}
```

The deployed speech-server now has a receiver endpoint. The default target is:

```text
POST http://192.168.1.70:18070/v1/wake
```

`POST /v1/wake` accepts `rtsp_url=...` or camera selectors such as
`camera=sala` and `camera=quintal`, starts RTSP capture plus Whisper in the
background, and exposes the latest result via:

```text
GET http://192.168.1.70:18070/v1/wake/status
```

The direct WebSocket STT endpoint can also open camera RTSP audio itself:

```text
ws://192.168.1.70:18070/v1/stt/stream?rtsp_url=rtsp://192.168.1.130:89/videoSub&duration_ms=8000
```

Smoke tests after deployment showed both cameras using SDP-advertised PCMA
payload type `97` at `8000 Hz`:

```text
sala    192.168.1.130:89/videoSub packets=38 audio_ms=1216
quintal 192.168.1.165:89/videoSub packets=68 audio_ms=2176
```

See `SPEECH_SERVER_WAKE_STT.md` for the endpoint contract and run examples.

## RTSP Transport Finding

After a continuous reference audio source was placed near the camera, RTSP
transport was re-tested by copying the audio track as raw A-law.

Results for a 20-second target window:

```text
88/videoMain over TCP: 28928 bytes
89/videoSub  over TCP: 32256 bytes
88/videoMain over UDP: 165376 bytes
89/videoSub  over UDP: 174336 bytes
expected continuous 8 kHz A-law: about 160000 bytes
```

The `record_template.sh` helper now defaults to:

```text
rtsp://CAMERA_IP:89/videoSub
transport=udp
```

A 10-second smoke capture through that default path produced `167440` bytes of
decoded s16le PCM, close to the `160000` bytes expected for 10 seconds at
8 kHz mono 16-bit. The measured sample level for that smoke clip was near the
silence floor, so the byte-count result validates continuity of transport, not
voice content.

Interpretation:

- the camera can deliver continuous audio payload over RTSP when using UDP
- the sparse captures were caused by the RTSP-over-TCP path or ffmpeg's
  interaction with this firmware's TCP/interleaved RTSP behavior
- for template recording from the host, prefer `videoSub` + UDP
- for a local on-camera detector, avoid depending on the camera's own RTSP path
  if an internal Anyka audio tap can be implemented

## Microphone Capture Findings

Direct ADC open test on `sala`:

```text
timeout 2 dd if=/dev/akpcm_adc0 of=/tmp/adc_probe.raw bs=320 count=25
dd: can't open '/dev/akpcm_adc0': Operation not permitted
```

`anyka_ipc` owns the PCM devices:

```text
fd 7 -> /dev/akpcm_adc0
fd 8 -> /dev/akpcm_dac0
fd 9 -> /dev/akpcm_loopback0
```

Opening the already-owned ADC fd through `/proc` was also blocked:

```text
/proc/567/fd/7 -> /dev/akpcm_adc0
dd: can't open '/proc/567/fd/7': Operation not permitted
```

The same process maps a shared audio ring:

```text
/tmp/AudioStream size: 57344 bytes
anyka_ipc fd 49 -> /tmp/AudioStream
maps:
b42c1000-b42c7000 rwxs 00000000 ... /tmp/AudioStream
b42c7000-b42cf000 -w-s 00006000 ... /tmp/AudioStream
b42cf000-b42d7000 -w-s 00006000 ... /tmp/AudioStream
```

Snapshot comparison while audio played near the camera showed this layout:

```text
descriptor current index: u32 at 0x494
descriptor base:          0x4d0
descriptor count:         896
descriptor size:          24 bytes
payload base:             0x6000
payload size:             0x8000
payload frame length:     996 bytes
payload types observed:   0x82 and 0x02
per-frame header:         16 bytes
payload encoding:         signed 16-bit little-endian PCM after the header
source rate observed:     about 8 kHz
```

The current prototype has an experimental read-only input for this ring:

```bash
/tmp/jarvis_wake \
  --ak-stream /tmp/AudioStream \
  --template /tmp/jarvis_01.raw \
  --template /tmp/jarvis_02.raw \
  --template /tmp/jarvis_03.raw \
  --rate 8000 \
  --threshold 65000 \
  --no-net --profile --verbose
```

For `--rate 8000`, the reader skips the 16-byte frame header and feeds the
remaining `s16le` samples directly into VAD/DTW.

Silence validation on `sala` after fixing the frame format:

```text
ak_stream path=/tmp/AudioStream desc_count=896 payload_size=32768 frame_header=16 source_rate=8000 detector_rate=8000 start_index=784
profile frames=407 utterances=0 accepted=0 frame_avg_us=48 eval_total_us=0 dtw_total_us=0
anyka_ipc pid before/after: 567
```

The important correction is that raw zero bytes in `/tmp/AudioStream` represent
silence after the 16-byte frame header. Treating those bytes as A-law creates a
false non-silent signal, so the on-camera reader must use the `s16le` path.

This is safer than trying to take ownership of `/dev/akpcm_adc0`: it does not
change the stock process' file descriptors, it does not require an RTSP client
on the camera, and the first run did not restart `anyka_ipc`.

The user confirmed that RTSP carries normal microphone audio when speaking at
the camera. Therefore RTSP is the preferred first path for recording real
`Jarvis` templates from the camera microphone:

```bash
cd jarvis_wake
./record_template.sh rtsp 192.168.1.130 templates/jarvis_01.raw 3 8000 sub udp
```

The first capture used `88/videoMain` over TCP. The script now defaults to the
lower-bandwidth `89/videoSub` path over UDP because both RTSP streams advertise
PCMA 8 kHz mono audio, and UDP delivers continuous audio payload on this
firmware. Pass `main` as the stream argument to force `88/videoMain`, or `tcp`
as the final argument to reproduce the TCP/interleaved behavior.

First 60-second wall-clock RTSP capture from `sala` while the user spoke
`Jarvis`:

```text
raw: jarvis_wake/captures/sala-jarvis-20260518-115839.raw
wav: jarvis_wake/captures/sala-jarvis-20260518-115839.wav
raw bytes: 156160
decoded PCM duration at 8000 Hz s16le: 9.76 s
peak level: -0.14 dB
RMS level: -23.69 dB
samples: 78080
```

The capture ran for 60 seconds, but the resulting PCM contains only about
9.76 seconds of samples. ffmpeg logged RTSP timestamp/CSeq warnings during the
recording. The audio level itself is not silent, so the next tuning pass should
inspect and segment the captured `Jarvis` utterances rather than discard it.

Second 60-second wall-clock RTSP capture from the lower-bandwidth `89/videoSub`
stream while the user spoke `Jarvis`:

```text
raw: jarvis_wake/captures/sala-sub-jarvis-20260518-120254.raw
wav: jarvis_wake/captures/sala-sub-jarvis-20260518-120254.wav
raw bytes: 63488
decoded PCM duration at 8000 Hz s16le: 3.97 s
peak level: -0.14 dB
RMS level: -20.27 dB
samples: 31744
```

The sub stream had lower video bandwidth but still produced sparse audio in
the raw capture. It produced less PCM than the first `88/videoMain` capture.
This suggests the capture issue is not simply video bandwidth. The next RTSP
capture experiment should either record the audio track with original
timestamps into a container first, or force a wall-clock capture strategy that
fills missing periods with silence before extracting raw PCM.

Additional RTSP copy test:

- copying PCMA directly from `88/videoMain` for a 20-second target window
  produced only `7936` bytes; continuous 8 kHz A-law for 20 seconds would be
  about `160000` bytes
- copying PCMA directly from `89/videoSub` in the same style produced no useful
  payload before the host timeout in that run

Because these tests used RTSP over TCP, ordinary UDP packet loss is unlikely to
be the whole explanation. The evidence points more toward sparse/gated audio
delivery from the RTSP mux path, timestamp problems, or server-side RTSP
session behavior. PCMA encoding itself is trivial and should not create this
kind of multi-second audio sparsity.

With continuous audio playing near the camera, UDP behaved very differently:

```text
88/videoMain over TCP, 20 s target: 28928 bytes A-law
89/videoSub  over TCP, 20 s target: 32256 bytes A-law
88/videoMain over UDP, 20 s target: 165376 bytes A-law
89/videoSub  over UDP, 20 s target: 174336 bytes A-law
continuous 8 kHz A-law expectation for 20 s: 160000 bytes
```

Interpretation:

- the microphone and PCMA encoder are capable of continuous audio
- the audio sparsity is specific to the RTSP/TCP-interleaved capture path, or
  to how ffmpeg interacts with this RTSP implementation over TCP
- UDP still logs reorder/missed-packet warnings, but it delivers roughly the
  expected amount of audio payload
- template recording should default to UDP until the RTSP/TCP behavior is
  understood or fixed

For the on-camera wake-word detector, an in-process microphone path should be
better than consuming the camera's own RTSP:

- it avoids RTSP packetization, timestamps, CSeq/session quirks, and network
  buffering
- it can copy audio before the RTSP mux drops/gates packets
- it runs inside the process that already owns `/dev/akpcm_adc0`

Do not take exclusive ownership of the microphone away from `anyka_ipc`.
Piggyback on the existing capture path instead: hook/copy from the Anyka AI or
AENC callback into a small ring buffer, then feed `jarvis_wake`-style logic.
If the easiest accessible callback is already PCMA/A-law rather than PCM, A-law
decode to 16-bit PCM is tiny and still cheaper than implementing RTSP locally.

## First Real Calibration

Positive capture:

```text
jarvis_wake/captures/sala-sub-udp-jarvis-positive-20260518-121634.raw
bytes: 1004688
duration at 8000 Hz s16le: 62.79 s
RMS level: -21.92 dB
```

Negative capture:

```text
jarvis_wake/captures/sala-sub-udp-jarvis-negative-20260518-122148.raw
bytes: 1000432
duration at 8000 Hz s16le: 62.53 s
RMS level: -22.20 dB
```

`extract_candidates.sh` found 17 positive speech candidates from the positive
capture. Three were copied into local ignored templates:

```text
jarvis_wake/templates/jarvis_01.raw
jarvis_wake/templates/jarvis_02.raw
jarvis_wake/templates/jarvis_03.raw
```

Important algorithm adjustment:

- with continuous background audio, evaluating forced `max_len` windows caused
  many false positives
- `jarvis_wake` now rejects `max_len` windows by default
- pass `--eval-max-len` only for debugging

First useful calibration:

```text
templates: jarvis_01.raw, jarvis_02.raw, jarvis_03.raw
threshold: 65000
positive result: 13 accepted wake candidates
negative result: 0 accepted wake candidates
```

This is only same-session calibration. It proves the prototype can separate
this positive clip from this continuous-audio negative clip. It does not yet
prove robustness across distance, voice level, different rooms, or other
speakers.

## Camera ARM Replay With Real Captures

The updated ARM binary and the three local templates were uploaded to the
`sala` camera:

```text
/tmp/jarvis_wake
/tmp/jarvis_01.raw
/tmp/jarvis_02.raw
/tmp/jarvis_03.raw
/tmp/jarvis_positive.raw
/tmp/jarvis_negative.raw
```

Command shape:

```bash
/tmp/jarvis_wake \
  --stdin \
  --template /tmp/jarvis_01.raw \
  --template /tmp/jarvis_02.raw \
  --template /tmp/jarvis_03.raw \
  --rate 8000 \
  --no-net \
  --profile \
  --threshold 65000
```

Positive replay on camera:

```text
frames: 3139
utterances: 15
accepted: 13
frame_avg_us: 129
eval_total_us: 232138
dtw_total_us: 576
```

Negative replay on camera:

```text
frames: 3126
utterances: 1
accepted: 0
frame_avg_us: 28
eval_total_us: 381
dtw_total_us: 48
```

Stability:

```text
anyka_ipc PID before: 567
anyka_ipc PID after: 567
```

Interpretation:

- the ARM binary can replay 60-second real camera-mic clips locally
- CPU cost is acceptable for this offline pass
- rejecting `max_len` windows is essential for continuous-background-audio
  negative cases
- the next hard problem is not detector CPU; it is attaching a live in-process
  microphone source without disrupting `anyka_ipc`

Important distinction:

- RTSP is good for host-side template/test-clip recording from the real camera
  microphone.
- RTSP is not yet the final on-camera live source, because the camera does not
  have ffmpeg/sox and the detector should not become a full RTSP/RTP/PCMA
  client unless the internal Anyka capture path is worse.
- For a detector running locally on the camera, the likely next path is an
  internal Anyka audio callback/hook or a tiny shim inside the stock process.

## Next Practical Step

Record real camera-mic data:

1. one or more `Jarvis` templates via RTSP
2. a longer positive clip with silence + normal speech + `Jarvis`
3. a longer negative clip with silence + normal speech without `Jarvis`

Then run:

```bash
cat positive.raw | ./jarvis_wake/run.sh --no-net --verbose --profile
cat negative.raw | ./jarvis_wake/run.sh --no-net --verbose --profile
```

Tune:

- `--threshold`
- `--vad-ratio`
- `--min-energy`
- number of templates

Only after the real RTSP-recorded clips separate cleanly should we attach a
live on-camera capture source.
