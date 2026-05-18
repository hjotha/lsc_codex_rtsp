# Speaker Playback

This document describes the current validated local speaker playback path for
the stock LSC / Tuya / Anyka camera process.

## Status

Validated on `2026-05-18` against the `quintal` camera:

- camera IP: `192.168.1.165`
- firmware class: `V3.2863.93`
- stock `anyka_ipc` md5: `87f1683cee35353fb2c2be20353bf59c`
- result: audible `dingdong` MP3 playback confirmed at the camera
- stability check: after the successful thread-mode playback, `anyka_ipc`
  stayed on PID `557`, uptime continued from `147s` to `238s`, and ports
  `24`, `88`, `89`, and `6668` stayed open

## Current State From Live Testing

Live test state on `2026-05-18`:

- the new V3.2863.93 SD bundle was copied to `/tmp/sd`
- a clean camera reboot loaded the new bundle
- `vendor_rtsp_boot.sh` copied the new `rtsp_kick` into `/tmp`
- RTSP came back normally on `88` and `89`
- `videoMain`, `videoSub`, and `videoSub` PCMA audio all delivered RTP packets
- direct same-thread `ht_audio_codec_play_audio_file` calls and stale decoder
  retests were unstable and could restart `anyka_ipc`
- after a clean reboot, spawning `ht_audio_codec_play_audio_file` through
  stock `ak_thread_create` played `dingdong` audibly and did not restart
  `anyka_ipc`

Important stability finding:

- the camera did not fully reboot during earlier failures; `/proc/uptime` and
  `/tmp` persisted
- the failing mode restarted only `anyka_ipc`, so RTSP/Tuya briefly dropped
- the SD watchdog rearmed RTSP automatically after the PID changed
- increasing the ptrace wait with `--wait-timeout 30` did not fix direct mode
- the validated path is therefore the thread-mode helper, not the old direct
  same-thread playback call

The issue was not speaker volume. The stock MP3 decoder channel had not been
started. Calling `ht_audio_codec_play_audio_file` directly toggled the AO path
but ended with:

```text
ak_adec_send_stream_end: adec send stream no start
```

Starting the decode channel first with `decode_type=2` fixed playback:

```text
set_decoder_input: decode type=2: MP3
MP3 dec V2 32bit open ok
```

The successful thread-mode run still logged one `_SD_Decode is locked by 0x10`
line, but playback completed, the playback source returned to `0`, and
`anyka_ipc` kept the same PID.

## What Does Not Work

The camera RTSP server is output-only. It advertises `OPTIONS, DESCRIBE, SETUP,
PLAY, PAUSE, TEARDOWN` and does not expose `ANNOUNCE`, `RECORD`, ONVIF
backchannel, or an input audio track. Speaker audio is therefore not sent by
pushing media into the existing RTSP URL.

Direct writes to `/dev/akpcm_dac0` are also not the right path while stock
`anyka_ipc` is running. The process owns:

- `/dev/akpcm_adc0`
- `/dev/akpcm_dac0`
- `/dev/akpcm_loopback0`

## Official V93 Test Command

Use the helper script from a Linux host with telnet access to the camera:

```bash
bash scripts/play_speaker_mp3_v93.sh 192.168.1.165 dingdong 10
```

Arguments:

- `192.168.1.165`: camera IP
- `dingdong`: built-in MP3 asset
- `10`: stock AO volume argument

Available built-in sound names:

- `dingdong`
- `factory`
- `siren`
- `hutong1`
- `hutong2`
- `tmp` for an already-uploaded `/tmp/speaker.wav`

The helper also accepts a local MP3 path. In that case it uploads the file to
`/tmp/speaker.wav` first and then plays it through the stock path string:

```bash
bash scripts/play_speaker_mp3_v93.sh 192.168.1.165 ./chime.mp3 10
```

The script defaults to `PLAY_CALL_MODE=thread`, which is the validated mode. It
uses `/tmp/rtsp_kick` if it already supports `--arg1` and
`--call-in-new-thread`. Otherwise it builds and uploads the current official
`rtsp_kick` to `/tmp/rtsp_kick` first. The upload is volatile and disappears
after a camera reboot.

## Manual V93 Sequence

The working sequence is three `rtsp_kick` calls into stock `anyka_ipc`:

```sh
PID="$(pidof anyka_ipc | awk '{print $1}')"

# Start channel 0 as MP3.  On this firmware, AUDIO_DEC_TYPE_MP3 is 2.
/tmp/rtsp_kick "$PID" --verbose \
  --func-vaddr 0x0007c27c \
  --guard-vaddr 0x0051ab34 \
  --arg0 0 \
  --arg1 2 \
  --no-guard-check

# Set stock audio-output volume.
/tmp/rtsp_kick "$PID" --verbose \
  --func-vaddr 0x0007cf9c \
  --guard-vaddr 0x0051ab34 \
  --arg0 10 \
  --no-guard-check

# Play /usr/share/dingdong.mp3 from a stock ak_thread_create thread.
/tmp/rtsp_kick "$PID" --verbose \
  --func-vaddr 0x0007c6c0 \
  --guard-vaddr 0x0051ab34 \
  --arg0 0x0037b7b0 \
  --call-in-new-thread \
  --malloc-vaddr 0x000607b4 \
  --thread-create-vaddr 0x0012208c \
  --thread-stack-size 0x10000 \
  --wait-timeout 30 \
  --no-guard-check
```

Useful log check:

```sh
tail -120 /var/log/messages | grep -E 'ADEC|AO|playback|MP3|source|volume'
```

## V93 Addresses

| Symbol / value | Address |
|---|---:|
| RTSP guard reused for safe attach override | `0x0051ab34` |
| `ht_audio_codec_start_decode` | `0x0007c27c` |
| `ht_audio_codec_play_audio_file` | `0x0007c6c0` |
| `ht_audio_codec_set_ao_volume` | `0x0007cf9c` |
| `ak_adec_print_runtime_status` | `0x000cba60` |
| `malloc@plt` | `0x000607b4` |
| `ak_thread_create` | `0x0012208c` |
| `AUDIO_DEC_TYPE_MP3` | `2` |

Built-in MP3 path string addresses:

| Sound | Path | Address |
|---|---|---:|
| `dingdong` | `/usr/share/dingdong.mp3` | `0x0037b7b0` |
| `tmp` | `/tmp/speaker.wav` | `0x00378c84` |
| `factory` | `/usr/share/8k16_cn_factory_speaker_test_voice.mp3` | `0x00378ec0` |
| `siren` | `/usr/share/8k16_siren.mp3` | `0x0037e548` |
| `hutong1` | `/usr/share/hutong_sound1.mp3` | `0x0037b7e4` |
| `hutong2` | `/usr/share/hutong_sound2.mp3` | `0x00378d58` |

## `rtsp_kick` Requirement

Speaker playback needs a function call with both `r0` and `r1`. The official
`rtsp_kick` now supports:

- `--arg0`
- `--arg1`
- `--arg2`
- `--arg3`
- `--wait-timeout N`
- `--call-in-new-thread`
- `--thread-create-vaddr`
- `--thread-stack-size`

That keeps the helper generic: RTSP boot still uses the default
`ht_rtsp_start` path, while speaker playback can target stock audio functions
with explicit `--func-vaddr` and register arguments. Thread mode allocates a
small remote stub with `malloc@plt`, flushes it, and starts it through stock
`ak_thread_create`; the playback function receives `--arg0` as its thread
argument.
