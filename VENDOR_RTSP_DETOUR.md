# Vendor RTSP Detour

This document describes the currently proven approach for enabling RTSP on the stock camera firmware while keeping Tuya functionality alive.

## Summary

The stock `anyka_ipc` binary already contains a full RTSP implementation.

The working detour on firmware `V3.2863.105` is:

1. call `ht_rtsp_start` inside the live process with `ptrace`
2. chain the active Tuya video callbacks so they still run
3. also call `ht_rtsp_send_video_frame` from those same callbacks

That gives us:

- stock `anyka_ipc`
- stock Tuya app flow
- stock vendor RTSP on `88` and `89`

## Live result

Validated on `2026-04-12` against camera `192.168.1.126`.

Observed state:

- `88` open
- `89` open
- `554` closed
- `8554` closed

Validated RTSP base URLs:

- `rtsp://192.168.1.126:88/videoMain`
- `rtsp://192.168.1.126:89/videoSub`

Validated track URLs exposed by SDP:

- `rtsp://192.168.1.126:88/videoMain/track1`
- `rtsp://192.168.1.126:88/videoMain/track2`
- `rtsp://192.168.1.126:89/videoSub/track1`
- `rtsp://192.168.1.126:89/videoSub/track2`

Media proven live:

- `videoMain/track1` = H.265 RTP
- `videoMain/track2` = PCMA RTP
- `videoSub/track1` = H.265 RTP
- `videoSub/track2` = PCMA RTP

## Why the plain one-shot call was not enough

Calling `ht_rtsp_start` alone starts the vendor worker thread and opens the listeners, but that does not automatically feed video into the RTSP path.

The missing piece was callback wiring:

- the active Tuya video callbacks were occupying the single callback slots
- the vendor path needed those frames too

The fix was to install a video chain stub in RWX memory:

- call the original Tuya wrapper first
- then call `ht_rtsp_send_video_frame`

That preserves Tuya behavior while pumping the stock RTSP path.

## Important reverse-engineering conclusions

These findings explain why the final detour works:

- the stock binary exports the full RTSP stack
- `ht_rtsp_start` does not depend on `factory_cfg.ini`
- the normal stock RTSP worker uses `88` and `89`
- the factory RTSP path uses `554`, but it is a different path and was not selected as the safe coexistence route
- the video callback slots are single-slot, not chaining
- audio was already less problematic because one live audio callback already pointed into the RTSP path

## Port discussion

This is the current answer for ports:

- `88` and `89` are the proven safe vendor RTSP ports for the stock worker path
- `554` belongs to the factory RTSP path, not the safe coexistence path
- `8554` belonged to the archived custom sidecar

So if the goal is:

- safest working stock coexistence:
  stay on `88` and `89`
- exactly `554`:
  that requires a different patch strategy or a relay layer
- exactly `8554`:
  that means reviving the archived sidecar or adding a forwarder

At the moment, there is no simple `rtsp_kick` flag that switches the vendor worker to `554` or `8554`.

## Firmware and addresses

Tested target:

- firmware:
  `V3.2863.105`
- stock binary md5:
  `c31358a8f598c56073720e96c004fa9c`

Default addresses used by `rtsp_kick`:

- `ht_rtsp_start`:
  `0x000d1800`
- RTSP guard:
  `0x00587600`
- `malloc@plt`:
  `0x000798b4`
- `ht_rtsp_send_video_frame`:
  `0x000d1310`
- video callback slot 0:
  `0x0058767c`
- video callback slot 1:
  `0x005876b8`
- expected Tuya callback 0:
  `0x000897cc`
- expected Tuya callback 1:
  `0x000898f4`

## Build

Build command:

```bash
bash scripts/build_rtsp_kick_anyka.sh
```

Output:

```text
out/rtsp_kick_arm
```

Preferred toolchain:

- `toolchain/arm-anykav200-crosstool`

Fallback:

- `arm-linux-gnueabi-gcc`

## Upload

Recommended upload:

```bash
bash scripts/deploy_rtsp_kick.sh 192.168.1.126 24
```

That uploads:

- local:
  `out/rtsp_kick_arm`
- remote:
  `/tmp/rtsp_kick`

The helper is intentionally deployed to `/tmp` so a reboot clears it.

## Commands

### Dry run

```bash
python3 tools/telnet_exec.py 192.168.1.126 \
  --command '/tmp/rtsp_kick $(pidof anyka_ipc) --verbose --dry-run'
```

### Start the vendor worker

```bash
python3 tools/telnet_exec.py 192.168.1.126 \
  --command '/tmp/rtsp_kick $(pidof anyka_ipc) --verbose'
```

### Dry-run the video chain

```bash
python3 tools/telnet_exec.py 192.168.1.126 \
  --command '/tmp/rtsp_kick $(pidof anyka_ipc) --verbose --install-video-chain --no-start-call --dry-run'
```

Expected dry-run state on the tested camera:

- guard value:
  `0x00000001`
- slot 0 callback:
  `0x000897cc`
- slot 1 callback:
  `0x000898f4`

### Install the video chain

```bash
python3 tools/telnet_exec.py 192.168.1.126 \
  --command '/tmp/rtsp_kick $(pidof anyka_ipc) --verbose --install-video-chain --no-start-call'
```

Observed live output on the tested camera:

```text
installed video chain stubs: stub0=0x01d22f88 stub1=0x01d22ff8 slot0=0x0058767c slot1=0x005876b8
video chain install completed and target thread was detached cleanly
```

## Validation

### Camera-side

```bash
python3 tools/telnet_exec.py 192.168.1.126 \
  --command 'ps; netstat -ltn'
```

### Windows-side RTP probes

Main video:

```powershell
powershell -ExecutionPolicy Bypass -File tools/rtsp_probe_windows.ps1 `
  -CameraHost 192.168.1.126 `
  -Port 88 `
  -Path videoMain `
  -Kind video `
  -ClientPort 50024
```

Main audio:

```powershell
powershell -ExecutionPolicy Bypass -File tools/rtsp_probe_windows.ps1 `
  -CameraHost 192.168.1.126 `
  -Port 88 `
  -Path videoMain `
  -Kind audio `
  -ClientPort 50026
```

Sub video:

```powershell
powershell -ExecutionPolicy Bypass -File tools/rtsp_probe_windows.ps1 `
  -CameraHost 192.168.1.126 `
  -Port 89 `
  -Path videoSub `
  -Kind video `
  -ClientPort 50028
```

Sub audio:

```powershell
powershell -ExecutionPolicy Bypass -File tools/rtsp_probe_windows.ps1 `
  -CameraHost 192.168.1.126 `
  -Port 89 `
  -Path videoSub `
  -Kind audio `
  -ClientPort 50030
```

## Transport note

The tested stock server did not accept interleaved TCP:

- `RTP/AVP/TCP` returned:
  `461 Unsupported Transport`

The proven working path is:

- RTSP control over TCP
- RTP media over UDP

## WSL caveat

If you test from `WSL2`, you may see a false negative:

- `OPTIONS`, `DESCRIBE`, `SETUP`, and `PLAY` succeed
- RTP appears to time out

That can happen because the UDP return path lands on the Windows host instead of the WSL VM. The final live proof in this repo was therefore done from native Windows PowerShell.

## Related files

Active:

- `src/rtsp_kick.c`
- `scripts/build_rtsp_kick_anyka.sh`
- `scripts/deploy_rtsp_kick.sh`
- `scripts/make_deploy_rtsp_kick_telnet.sh`
- `tools/telnet_exec.py`
- `tools/telnet_upload_file.py`
- `tools/rtsp_probe.py`
- `tools/rtsp_probe_windows.ps1`

Archived sidecar:

- `unused/sidecar/`
