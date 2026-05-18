# Jarvis Wake Prototype

Tiny wake-word prototype for the Anyka ARMv5 camera class. This is not a
machine-learning wake-word engine; it is a cheap experiment to see whether a
fixed-template `Jarvis` detector can run on the camera without disturbing the
stock firmware.

## Status

Implemented:

- raw signed 16-bit mono PCM input from `stdin`
- energy VAD
- per-frame squared-energy and zero-crossing features
- bounded two-row DTW against one or more raw templates
- configurable HTTP POST using raw TCP sockets
- network worker thread so the audio loop does not block on `connect()`
- `--no-net`, `--profile`, and `--once`
- max-length VAD windows are rejected by default; pass `--eval-max-len` only
  when debugging continuous-audio cases
- `extract_candidates.sh` to split RTSP-recorded calibration clips into
  template candidates
- experimental Anyka `/tmp/AudioStream` input, using read-only mmap of the
  stock shared audio ring

Not implemented yet:

- native Anyka callback hook
- MFCC or heavier features
- speech-server `/v1/wake` endpoint

## Build

Host build:

```bash
make -C jarvis_wake
```

Anyka cross build, using the repo's existing toolchain wrapper:

```bash
make -C jarvis_wake anyka
```

The cross build writes:

```text
out/jarvis_wake_arm
```

## Record A Template From The Camera Microphone

The preferred template source is the same camera microphone path that will feed
the detector. Use RTSP audio from the target camera and say `Jarvis` clearly
near the camera:

```bash
cd jarvis_wake
./record_template.sh rtsp 192.168.1.130 templates/jarvis_01.raw 3 8000 sub udp
```

The output is raw `s16le` mono PCM at 8 kHz, not WAV.

Current capture finding:

- direct second-process open of `/dev/akpcm_adc0` on the `sala` camera failed
  with `Operation not permitted`
- `anyka_ipc` owns `/dev/akpcm_adc0`, `/dev/akpcm_dac0`, and
  `/dev/akpcm_loopback0`
- `/tmp/AudioStream` is a safer on-camera live input than taking ownership of
  `/dev/akpcm_adc0`; it is a stock shared PCM ring already fed by
  `anyka_ipc`
- RTSP should still be treated as the first practical host-side microphone
  capture path for template recording
- the script defaults to the lower-bandwidth `89/videoSub` RTSP stream and
  UDP transport; use `main` as the stream argument to force `88/videoMain`, or
  `tcp` as the final argument when explicitly comparing TCP behavior

If RTSP audio is too quiet, record several samples and inspect them on the host
before tuning thresholds.

## Run Against The Anyka Audio Ring

On the camera, the experimental live source is:

```bash
nice -n 5 /tmp/jarvis_wake \
  --ak-stream /tmp/AudioStream \
  --template /tmp/jarvis_01.raw \
  --template /tmp/jarvis_02.raw \
  --template /tmp/jarvis_03.raw \
  --rate 8000 \
  --threshold 65000 \
  --no-net --profile
```

The detected layout on the `sala` firmware is:

```text
descriptor base: 0x4d0
descriptor count: 896
descriptor size: 24 bytes
payload base: 0x6000
payload size: 0x8000
payload frame length observed: 996 bytes
per-frame header: 16 bytes
payload encoding: signed 16-bit little-endian PCM after the header
source rate observed: about 8 kHz
```

The reader skips the 16-byte frame header and feeds the remaining `s16le` PCM
directly to the detector. `--ak-source-rate` defaults to `8000`; override it
only if a different firmware proves the ring uses another sample rate.

## Run Against A Raw Stream

```bash
cat some_camera_audio.raw | ./run.sh --no-net --profile --verbose
```

With network enabled, the default POST target is:

```text
POST http://192.168.1.70:18070/v1/wake
```

That endpoint is not present in the current speech-server yet. Use `--no-net`
until the receiver exists, or override host/port/path:

```bash
cat some_camera_audio.raw | ./run.sh --host 192.168.1.70 --port 18070 --path /v1/wake
```

## Detector Notes

Lower DTW score is better. The default threshold is intentionally a starting
point, not a calibrated value:

```bash
cat positive.raw | ./run.sh --no-net --verbose --threshold 90000
cat negative.raw | ./run.sh --no-net --verbose --threshold 90000
```

Tune only after collecting positives and negatives from the same camera
microphone path. For background audio, keep the default max-length rejection;
otherwise continuous audio can be chopped into repeated false trigger windows.

Current first-pass calibration from `sala`:

```bash
cat positive.raw | ./jarvis_wake \
  --stdin \
  --template templates/jarvis_01.raw \
  --template templates/jarvis_02.raw \
  --template templates/jarvis_03.raw \
  --threshold 65000 \
  --no-net --verbose --profile
```

That accepted 13 wake candidates in the positive clip and 0 in the negative
continuous-audio clip.

Keep the camera run low priority:

```bash
nice -n 5 ./jarvis_wake --stdin --template templates/jarvis_01.raw --no-net
```

## Expected First Test Flow

1. Build locally with `make -C jarvis_wake`.
2. Record `templates/jarvis_01.raw` through RTSP from the target camera.
3. Record a longer raw clip from the same RTSP stream containing silence,
   normal speech, and `Jarvis`.
4. Run the detector offline with `--no-net --verbose --profile`.
5. Tune `--threshold`, `--vad-ratio`, and `--min-energy`.
6. Cross-compile and replay the same raw files on the camera through `stdin`.
7. Only then attach a live camera audio source.
