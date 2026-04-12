# LSC Codex RTSP

Tools and notes for enabling RTSP on LSC / Tuya Anyka cameras without replacing the stock `anyka_ipc` binary.

The project now has a clear primary path:

- preferred path:
  wake up the vendor RTSP server already linked into the stock firmware and chain the Tuya video callbacks into it
- archived path:
  a custom sidecar RTSP server that rebuilt media from the stock ring buffers

The sidecar path is now archived under:

- `unused/sidecar/`

## Current status

As of `2026-04-12`, the vendor RTSP detour is the recommended hack.

It has been validated on a real camera with:

- model family:
  LSC rotatable / Tuya / Anyka
- firmware:
  `V3.2863.105`
- stock process:
  `anyka_ipc`
- camera IP used during validation:
  `192.168.1.126`

What is already proven:

- the Tuya app remains usable after the detour is installed
- the stock `anyka_ipc` process remains alive
- the stock vendor RTSP server responds on:
  `88`
  `89`
- the main stream is available at:
  `rtsp://CAMERA_IP:88/videoMain`
- the sub stream is available at:
  `rtsp://CAMERA_IP:89/videoSub`
- live RTP media was captured successfully for:
  `88/videoMain/track1` = H.265 video
  `88/videoMain/track2` = PCMA audio
  `89/videoSub/track1` = H.265 video
  `89/videoSub/track2` = PCMA audio

What is not currently active:

- the legacy sidecar on `8554`
- a vendor listener on `554`

Live checks on `2026-04-12` confirmed:

- `88` open
- `89` open
- `554` closed
- `8554` closed

## Tested RTSP URLs

These URLs were validated against the live camera:

- `rtsp://192.168.1.126:88/videoMain`
- `rtsp://192.168.1.126:89/videoSub`

The SDP exposed by the server uses these track URLs:

- `rtsp://192.168.1.126:88/videoMain/track1`
- `rtsp://192.168.1.126:88/videoMain/track2`
- `rtsp://192.168.1.126:89/videoSub/track1`
- `rtsp://192.168.1.126:89/videoSub/track2`

In normal clients, use the base URLs first:

- `rtsp://CAMERA_IP:88/videoMain`
- `rtsp://CAMERA_IP:89/videoSub`

## Why the vendor detour is better

The older common hack replaces `anyka_ipc` with a different firmware build that exposes RTSP. That can work, but it frequently breaks Tuya cloud, PTZ behavior, or app visibility because it swaps out the main stock process.

The current approach is safer because it:

- keeps the original `anyka_ipc` running
- does not replace the firmware's main executable
- does not take direct ownership of the sensor outside the stock pipeline
- reuses the vendor RTSP stack already present in the stock binary

## How it works

The detour uses `src/rtsp_kick.c`.

It does two things:

1. it performs a one-shot `ptrace` call into `ht_rtsp_start` inside the stock process
2. it optionally installs two heap stubs that preserve the active Tuya video callbacks and also call `ht_rtsp_send_video_frame`

That second step is the key to coexistence:

- Tuya keeps receiving frames through the original wrappers
- the stock RTSP worker also starts receiving video frames

The resulting stock RTSP server then serves:

- `videoMain` on port `88`
- `videoSub` on port `89`

## Why not `554` or `8554`

This is the important practical answer:

- `8554` was the old custom sidecar port and is not part of the stock vendor RTSP path
- `554` exists in the factory RTSP path, but that factory path is not the safe coexistence path
- the safe stock worker used by `ht_rtsp_start` serves on `88` and `89`

So:

- changing the port is not a simple `rtsp_kick` flag today
- `554` would require a different patch path or a relay layer
- `8554` would mean reviving the archived sidecar or building a new forwarder

In other words:

- yes, alternate ports are theoretically possible
- no, they are not part of the currently proven safe hack

## Repository layout

Current active files:

- `src/rtsp_kick.c`
- `scripts/build_rtsp_kick_anyka.sh`
- `scripts/make_deploy_rtsp_kick_telnet.sh`
- `scripts/deploy_rtsp_kick.sh`
- `tools/telnet_exec.py`
- `tools/telnet_upload_file.py`
- `tools/rtsp_probe.py`
- `tools/rtsp_probe_windows.ps1`
- `VENDOR_RTSP_DETOUR.md`
- `INVESTIGATION.md`

Archived legacy sidecar files:

- `unused/sidecar/`

## Build

Preferred build path:

- the original Anyka toolchain under `toolchain/arm-anykav200-crosstool`

Command:

```bash
bash scripts/build_rtsp_kick_anyka.sh
```

Output:

```text
out/rtsp_kick_arm
```

The build script first tries the old Anyka toolchain and falls back to `arm-linux-gnueabi-gcc` if needed.

## Deploy to the camera

Recommended upload:

```bash
bash scripts/deploy_rtsp_kick.sh 192.168.1.126 24
```

That script:

- builds `rtsp_kick`
- uploads it to `/tmp/rtsp_kick`

The upload is volatile by design:

- rebooting the camera clears `/tmp/rtsp_kick`

## Start the vendor RTSP server

Dry run:

```bash
python3 tools/telnet_exec.py 192.168.1.126 \
  --command '/tmp/rtsp_kick $(pidof anyka_ipc) --verbose --dry-run'
```

Wake the stock RTSP worker:

```bash
python3 tools/telnet_exec.py 192.168.1.126 \
  --command '/tmp/rtsp_kick $(pidof anyka_ipc) --verbose'
```

Install the video chain after the worker exists:

```bash
python3 tools/telnet_exec.py 192.168.1.126 \
  --command '/tmp/rtsp_kick $(pidof anyka_ipc) --verbose --install-video-chain --no-start-call'
```

Safe dry run for the chain install:

```bash
python3 tools/telnet_exec.py 192.168.1.126 \
  --command '/tmp/rtsp_kick $(pidof anyka_ipc) --verbose --install-video-chain --no-start-call --dry-run'
```

## Validation

### Camera-side state

```bash
python3 tools/telnet_exec.py 192.168.1.126 \
  --command 'ps; netstat -ltn'
```

Expected listeners for the proven vendor path:

- `88`
- `89`
- `6668`

### Windows-side RTP validation

Main stream video:

```powershell
powershell -ExecutionPolicy Bypass -File tools/rtsp_probe_windows.ps1 `
  -CameraHost 192.168.1.126 `
  -Port 88 `
  -Path videoMain `
  -Kind video `
  -ClientPort 50024
```

Main stream audio:

```powershell
powershell -ExecutionPolicy Bypass -File tools/rtsp_probe_windows.ps1 `
  -CameraHost 192.168.1.126 `
  -Port 88 `
  -Path videoMain `
  -Kind audio `
  -ClientPort 50026
```

Sub stream video:

```powershell
powershell -ExecutionPolicy Bypass -File tools/rtsp_probe_windows.ps1 `
  -CameraHost 192.168.1.126 `
  -Port 89 `
  -Path videoSub `
  -Kind video `
  -ClientPort 50028
```

Sub stream audio:

```powershell
powershell -ExecutionPolicy Bypass -File tools/rtsp_probe_windows.ps1 `
  -CameraHost 192.168.1.126 `
  -Port 89 `
  -Path videoSub `
  -Kind audio `
  -ClientPort 50030
```

## WSL note

Be careful with validation from `WSL2`.

`SETUP` and `PLAY` may succeed while RTP appears to time out because the UDP return path lands on the Windows host and not on the WSL VM. That is why the final live RTP proof in this repository was done from native Windows sockets.

## References

The work in this repository was informed by these public sources:

- [tasarren/lsc-tuya-toolkit](https://github.com/tasarren/lsc-tuya-toolkit)
- [guino/LSCOutdoor1080P](https://github.com/guino/LSCOutdoor1080P)
- [Nemobi/ak3918ev300v18](https://github.com/Nemobi/ak3918ev300v18)
- [Nemobi/Anyka](https://github.com/Nemobi/Anyka)
- [MuhammedKalkan/Anyka-Camera-Firmware](https://github.com/MuhammedKalkan/Anyka-Camera-Firmware)
- [ricardojlrufino/arm-anykav200-crosstool](https://github.com/ricardojlrufino/arm-anykav200-crosstool)
- [seydx/tuya-ipc-terminal](https://github.com/seydx/tuya-ipc-terminal)

## Next useful work

- automate the detour after boot
- decide whether a port relay to `554` is worth the added complexity
- run longer soak tests with the Tuya app active
- keep the archived sidecar only as a reverse-engineering aid, not the primary delivery path
