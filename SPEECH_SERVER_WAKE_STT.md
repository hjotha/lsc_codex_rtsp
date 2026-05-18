# Speech Server Wake/STT Bridge

Status on `2026-05-18`: implemented and smoke-tested on the deployed
speech-server at `192.168.1.70:18070`.

The intended split is:

- the Anyka camera runs only the tiny local `jarvis_wake` detector
- after a wake hit, the camera sends a small HTTP event to the S10+
- the S10+ speech-server opens the camera RTSP audio stream itself
- Whisper on the S10+ transcribes the captured audio

This avoids continuously pushing audio from the ARMv5 camera to the S10+. It
also avoids tying the STT path to the experimental `/tmp/AudioStream` ring
reader. The ring reader is still useful for local wake testing, but full STT is
better handled from the RTSP microphone stream on the S10+.

## Direct RTSP WebSocket STT

`/v1/stt/stream` now accepts a stream URL to open directly:

```text
ws://192.168.1.70:18070/v1/stt/stream?rtsp_url=rtsp://192.168.1.130:89/videoSub&duration_ms=8000
```

Supported selectors:

- `rtsp_url=rtsp://CAMERA_IP:89/videoSub`
- `url=rtsp://CAMERA_IP:89/videoSub`
- `camera=sala`
- `camera=quintal`
- `camera_ip=192.168.1.130`
- `stream=sub` maps to `:89/videoSub`
- `stream=main` maps to `:88/videoMain`

The server uses RTSP TCP for control (`OPTIONS`, `DESCRIBE`, `SETUP`, `PLAY`)
and UDP RTP for the media stream. The audio payload type and sample rate are
read from SDP instead of hardcoded. On both checked cameras the SDP advertised
PCMA as dynamic RTP payload type `97` at `8000 Hz`.

The WebSocket emits:

- `ready`
- `capture_start`
- `capture_done`
- `final` or `error`

`capture_done` includes `sample_rate`, `payload_type`, `packets`,
`total_packets`, `rtp_bytes`, and `pcm_bytes`.

## Wake Endpoint

The deployed speech-server now exposes:

```text
POST http://192.168.1.70:18070/v1/wake
GET  http://192.168.1.70:18070/v1/wake/status
```

`POST /v1/wake` is asynchronous. It returns quickly with `accepted:true`, then
captures RTSP audio and runs Whisper in a background thread. Poll
`/v1/wake/status` for the final transcript and capture metrics.

Example using a camera alias:

```bash
curl -sS -X POST \
  'http://192.168.1.70:18070/v1/wake?camera=sala&duration_ms=8000&engine=whisper&model=base&threads=8&language=pt-BR' \
  -H 'Content-Type: application/json' \
  --data '{"wake_word":"jarvis","confidence":0.900,"score":12345,"timestamp":1779099999}'
```

Example using an explicit RTSP URL:

```bash
curl -sS -X POST \
  'http://192.168.1.70:18070/v1/wake?rtsp_url=rtsp://192.168.1.165:89/videoSub&duration_ms=8000' \
  -H 'Content-Type: application/json' \
  --data '{"wake_word":"jarvis","confidence":0.900,"score":12345,"timestamp":1779099999}'
```

For `jarvis_wake`, the path can carry the camera selector:

```bash
/tmp/jarvis_wake \
  --ak-stream /tmp/AudioStream \
  --rate 8000 \
  --template /tmp/jarvis_01.raw \
  --host 192.168.1.70 \
  --port 18070 \
  --path '/v1/wake?camera=sala&duration_ms=8000'
```

Use `camera=quintal` for the V93 camera at `192.168.1.165`.

## Validation

Smoke tests from the workstation after deployment:

```text
sala    rtsp://192.168.1.130:89/videoSub payload_type=97 sample_rate=8000 packets=38  audio_ms=1216
quintal rtsp://192.168.1.165:89/videoSub payload_type=97 sample_rate=8000 packets=68  audio_ms=2176
```

`POST /v1/wake?camera=sala&duration_ms=1200` returned `accepted:true`.
Polling `/v1/wake/status` then returned a completed session with RTSP capture
metrics and a Whisper transcript.

Whisper must be running in Termux on the S10+:

```bash
cd /home/hjotha/speech-server
WHISPER_ANDROID_SSH_PASSWORD='amdk6x' ./scripts/start-whisper-server.sh
```
