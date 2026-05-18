# V3.2863.93 Adaptation Notes

Work log of the effort to adapt the vendor RTSP detour for firmware `V3.2863.93` (MD5 `87f1683cee35353fb2c2be20353bf59c`).

Camera under test: `192.168.1.162`. Reference working camera: `192.168.1.126` (V3.2863.105).

## Result: validated

On `2026-05-01`, live RTSP on both streams confirmed on firmware `V3.2863.93`:

- `rtsp://192.168.1.162:88/videoMain` — H.265 2304x1296 @ ~25 fps + PCMA audio
- `rtsp://192.168.1.162:89/videoSub` — H.265 640x360 @ 15 fps + PCMA audio

The boot automation path (SD `hack.sh` + `custom.sh` + `vendor_rtsp_boot.sh`) auto-applies the detour on every cold boot. After reboot the full sequence fires in ~25 seconds and both streams come back up with audio+video present, while the stock `anyka_ipc` keeps running and Tuya port `6668` stays open.

On `2026-05-18`, built-in MP3 speaker playback was also validated on the active
V93 `quintal` camera at `192.168.1.165`. The missing step was starting the MP3
decode channel before calling the file playback helper:

```sh
/tmp/rtsp_kick "$(pidof anyka_ipc)" --verbose \
    --func-vaddr 0x0007c27c \
    --guard-vaddr 0x0051ab34 \
    --arg0 0 \
    --arg1 2 \
    --no-guard-check
```

`decode_type=2` is MP3 on this firmware. With that channel open,
`ht_audio_codec_play_audio_file(0x0037b7b0)` played
`/usr/share/dingdong.mp3` audibly through the camera speaker. See
`SPEAKER_PLAYBACK.md` for the full command sequence and sound-path table.

## Offsets derived for V3.2863.93

All via `.dynsym` comparison against V3.2863.105, backed by live `--peek-vaddr` verification on the running camera.

| Field | V3.2863.105 | V3.2863.93 |
|---|---|---|
| `ht_rtsp_start` | `0x000d1800` | `0x00091548` |
| `ht_rtsp_stop` | `0x000d18f4` | `0x0009163c` |
| `ht_rtsp_send_video_frame` | `0x000d1310` | `0x00091064` |
| `ht_rtsp_send_audio_frame` | `0x000d13d8` | `0x0009112c` |
| `ak_app_video_set_cb` | `0x000e8e78` | `0x000c6adc` |
| `ak_app_video_unset_cb` | `0x000e8ee8` | `0x000c6b4c` |
| `app_video_find_src_gchn` | `0x000e7200` | `0x000c4e64` |
| `malloc@plt` | `0x000798b4` | `0x000607b4` |
| `ak_thread_create` | `0x00144428` | `0x0012208c` |
| rtsp struct base (.bss) | `0x005875e8` | `0x0051ab1c` |
| `guard` (struct+0x18) | `0x00587600` | `0x0051ab34` |
| `video_slot0` (struct+0xa4 / +0x94) | `0x0058767c` | `0x0051abc0` |
| `video_slot1` (struct+0xe0 / +0xd0) | `0x005876b8` | `0x0051abfc` |
| expected Tuya video wrapper 0 | `0x000897cc` | `0x000a7124` |
| expected Tuya video wrapper 1 | `0x000898f4` | `0x000a723c` |

Note: the video slot layout between versions shifted by +0x10 bytes inside the rtsp context struct. In V105 the slots sit at `base+0x94`/`base+0xd0`; in V93 they sit at `base+0xa4`/`base+0xe0`. The `expected_cb0` / `expected_cb1` addresses are identified by signature matching the first 48 bytes of the V105 wrappers and finding exactly two matches in `.text` at `0x000a7124` and `0x000a723c` — the characteristic prefixed Tuya video callback prologue.

## Runtime command that works on V93

```sh
# 1. Wake the stock RTSP worker (thread, listeners on 88/89)
/tmp/sd/rtsp_kick $(pidof anyka_ipc) --verbose \
    --func-vaddr 0x00091548 \
    --guard-vaddr 0x0051ab34

# 2. Install the video callback chain (after a short delay so Tuya has registered its wrappers)
/tmp/sd/rtsp_kick $(pidof anyka_ipc) --verbose \
    --install-video-chain --no-start-call \
    --func-vaddr 0x00091548 \
    --guard-vaddr 0x0051ab34 \
    --malloc-vaddr 0x000607b4 \
    --video-send-vaddr 0x00091064 \
    --video-slot0-vaddr 0x0051abc0 \
    --video-slot1-vaddr 0x0051abfc \
    --expected-video-cb0 0x000a7124 \
    --expected-video-cb1 0x000a723c
```

Both steps are wrapped by `sdcard/vendor_rtsp_boot.sh`, which detects the firmware MD5 and selects the correct offset set. No rebuild of `rtsp_kick` itself is required — the binary accepts all offsets as flags.

## Observed symptom before video chain installed

Running only step 1 (`ht_rtsp_start`) opens ports `88` and `89` but the server does not respond to RTSP requests. The kernel accepts the TCP 3-way handshake, `Recv-Q` fills with pending bytes on the listening socket, but `ht_rtsp_th` never serves them. `ffmpeg` reports `Invalid data found when processing input`.

Running step 2 (`--install-video-chain`) immediately fixes this. After the chain install the server serves `OPTIONS`/`DESCRIBE`/`SETUP`/`PLAY` normally and both streams deliver full-rate RTP. This is interesting because on V105 the same thread state (`ht_rtsp_th` in `hrtimer_nanosleep`) coexists with a fully working server — which strongly suggests the acceptor runs on a different thread that is only fed once `ak_app_video_set_cb` has finished wiring the video sink side. Registering `ht_rtsp_send_video_frame` via the stubs seems to be what unblocks it.

## Approach that was tried and abandoned: swap to V105 binary

We tried copying the V3.2863.105 `anyka_ipc` onto the SD as `anyka_ipc_rtsp` and letting `hack.sh` bind-mount it over `/usr/bin/anyka_ipc`. After reboot the camera did not come back up on the network — the V105 userspace code was not compatible enough with the V93 system (different config formats, different device nodes). Removing the `anyka_ipc_rtsp` file from the SD restored the stock V93 boot.

That negative result is itself useful: it confirms that the right path is to keep the V93 binary and adapt the detour to it, rather than firmware-swapping.

## Files on SD (required for the V93 detour)

From `packages/sd_root_v3.2863.93/root/`:

- `_ht_ap_mode.conf` — firmware trigger marker (0 bytes)
- `hack` — sentinel (0 bytes)
- `hack.sh` — SD-side startup script
- `hostapd` — launcher the firmware runs from SD
- `custom.sh` — telnet + vendor_rtsp_boot.sh dispatcher
- `rtsp_kick` — the ARM binary doing the ptrace work
- `vendor_rtsp_boot.sh` — idempotent wrapper, detects MD5 and applies offsets
- `vendor_rtsp_boot.md5` — holds `87f1683cee35353fb2c2be20353bf59c` so the legacy md5-only guard also accepts V93

`vendor_rtsp_boot.sh` has a builtin table of supported MD5s, so it covers both V3.2863.93 and V3.2863.105 with one script.

## Validation on hardware

Cold-boot sequence captured from `/tmp/vendor_rtsp_boot.log` on the V93 camera at `2026-05-01 15:35`:

```
[15:35:21] copied rtsp_kick from SD to /tmp
[15:35:22] anyka_ipc not ready yet
[15:35:44] confirmed supported anyka_ipc md5=87f1683cee35353fb2c2be20353bf59c
[15:35:45] starting stock RTSP worker for pid 557
[15:35:45] ht_rtsp_start=0x00091548 guard=0x0051ab34 guard_value=0x00000000
[15:35:46] installing video callback chain for pid 557
[15:35:46] video slots before chain: slot0=0x000a7124 slot1=0x000a723c
[15:35:46] installed video chain stubs: stub0=0x014f3648 stub1=0x014f36b0
[15:35:46] vendor RTSP bootstrap finished successfully
```

Post-boot ports:

```
tcp   0.0.0.0:23     LISTEN   (telnet / vendor)
tcp   0.0.0.0:24     LISTEN   (telnet / SD hack)
tcp   0.0.0.0:88     LISTEN   (RTSP videoMain)
tcp   0.0.0.0:89     LISTEN   (RTSP videoSub)
tcp   0.0.0.0:6668   LISTEN   (Tuya)
```

`ffmpeg -rtsp_transport tcp -i rtsp://192.168.1.162:88/videoMain -frames:v 1 -y test.jpg` produced a valid 2304x1296 JPEG on the first try after reboot.

## Files saved locally

- `firmware_dumps/anyka_ipc_v3.2863.93` — stock V93 binary (md5 `87f1683cee35353fb2c2be20353bf59c`)
- `firmware_dumps/anyka_ipc_v3.2863.105` — reference V105 binary (md5 `c31358a8f598c56073720e96c004fa9c`)
- `firmware_dumps/compare.pkl` — pickled section/symbol tables for both (analysis cache)
