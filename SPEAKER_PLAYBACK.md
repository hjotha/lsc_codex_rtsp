# Speaker Playback

This document describes the proven local speaker playback path for the stock
LSC / Tuya / Anyka camera process.

## Status

Validated on `2026-05-18` against the `quintal` camera:

- camera IP: `192.168.1.165`
- firmware class: `V3.2863.93`
- stock `anyka_ipc` md5: `87f1683cee35353fb2c2be20353bf59c`
- result: audible MP3 playback confirmed at the camera

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

The script uses `/tmp/rtsp_kick` if it already supports `--arg1`. Otherwise it
builds and uploads the current official `rtsp_kick` to `/tmp/rtsp_kick` first.
The upload is volatile and disappears after a camera reboot.

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

# Play /usr/share/dingdong.mp3.
/tmp/rtsp_kick "$PID" --verbose \
  --func-vaddr 0x0007c6c0 \
  --guard-vaddr 0x0051ab34 \
  --arg0 0x0037b7b0 \
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
| `AUDIO_DEC_TYPE_MP3` | `2` |

Built-in MP3 path string addresses:

| Sound | Path | Address |
|---|---|---:|
| `dingdong` | `/usr/share/dingdong.mp3` | `0x0037b7b0` |
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

That keeps the helper generic: RTSP boot still uses the default
`ht_rtsp_start` path, while speaker playback can target stock audio functions
with explicit `--func-vaddr` and register arguments.
