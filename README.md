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
- the same camera survived a full unplug/replug power cycle and came back with the detour auto-applied from the SD bootstrap
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
- on the validation SD card, `23`, `24`, `6668`, and `8080` were also present
- no custom long-running RTSP sidecar process was required

The beginner SD bundle does not rely on `8080`.

## Easiest install

For normal users, the easiest documented path is now:

1. format a microSD card as `FAT32`
   not `exFAT`
2. copy everything from:
   `packages/sd_root_v3.2863.105/root/`
   to the root of the card
3. insert the card into the camera
4. power-cycle the camera
5. open:
   `rtsp://CAMERA_IP:88/videoMain`
   `rtsp://CAMERA_IP:89/videoSub`

That is the beginner path on the tested firmware.

Do not copy `install_vendor_bootstrap.sh` to the SD card.
That script is for advanced remote installation from a Linux host after telnet is already available.

The same SD-ready files also live in:

- `sdcard/`

So beginners can use either:

- `packages/sd_root_v3.2863.105/root/`
- `sdcard/`

On the tested camera, keeping both `hack` and `hack.sh` on the SD card is the safest path.
The known-good backup used:

- `_ht_ap_mode.conf` as the firmware trigger marker
- `hack` as a zero-byte sentinel
- `hostapd` as the launcher the firmware executes from SD
- `hack.sh` as the actual startup script

The repository now follows that layout again.

This path is intentionally gated to the tested stock `anyka_ipc` build:

- firmware:
  `V3.2863.105`
- md5:
  `c31358a8f598c56073720e96c004fa9c`

If the running stock binary does not match, the SD bootstrap refuses to patch.

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

### Why not just use `iptables`

On the tested firmware, no usable `iptables` binary was present.

A plain TCP forward from `8554 -> 88` is also not a clean RTSP alias, because the stock server advertises absolute control URLs on ports `88` and `89` inside SDP. That makes a raw TCP relay only a best-effort convenience hack, not a correct transparent alias.

So the current recommendation remains:

- use `88` for `videoMain`
- use `89` for `videoSub`
- only add a friendly alias later with an RTSP-aware proxy or a known-tolerant client

## Repository layout

Current active files:

- `src/rtsp_kick.c`
- `sdcard/_ht_ap_mode.conf`
- `sdcard/hack`
- `sdcard/hack.sh`
- `sdcard/hostapd`
- `sdcard/custom.sh`
- `sdcard/rtsp_kick`
- `sdcard/vendor_rtsp_boot.sh`
- `sdcard/vendor_rtsp_boot.md5`
- `packages/sd_root_v3.2863.105/`
- `STEP_BY_STEP.md`
- `scripts/build_rtsp_kick_anyka.sh`
- `scripts/prepare_sd_root_bundle.sh`
- `scripts/install_vendor_bootstrap.sh`
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

## Boot automation

On the tested camera, the boot path starts when `_ht_ap_mode.conf` triggers the SD `hostapd` launcher. That launcher runs `/mnt/hack.sh`, and `hack.sh` then runs `/tmp/sd/custom.sh` every 10 seconds. That makes boot persistence possible without replacing the stock firmware binary.

This repository now includes:

- `sdcard/hack`
- `sdcard/hack.sh`
- `sdcard/custom.sh`
- `sdcard/vendor_rtsp_boot.sh`
  an idempotent SD-side helper that:
  copies `rtsp_kick` from SD into `/tmp`
  starts the stock RTSP worker if needed
  installs the video callback chain
- `sdcard/vendor_rtsp_boot.md5`
  the supported stock binary hash for the tested firmware
- `packages/sd_root_v3.2863.105/root/`
  the copy-to-card bundle for beginners
- `scripts/install_vendor_bootstrap.sh`
  a host-side installer that:
  uploads `rtsp_kick` to `/tmp/sd/rtsp_kick`
  uploads `vendor_rtsp_boot.sh` to `/tmp/sd/vendor_rtsp_boot.sh`
  patches `custom.sh` to call it on each boot cycle

Install the bootstrap:

```bash
bash scripts/install_vendor_bootstrap.sh 192.168.1.126 24
```

After that, a reboot should re-apply the vendor RTSP detour automatically through the SD hack path.

What was observed on the validated cold boot:

- the first early boot attempt can happen before `anyka_ipc` is fully ready
- that early attempt may log a temporary `Protocol error` or see null callback slots
- the next `custom.sh` retry about 10 seconds later succeeds cleanly
- once the callbacks match the expected Tuya wrappers, the chain is installed and `/tmp/vendor_rtsp_boot.done` is created

## Simple step-by-step

If you just want the shortest safe path, read [`STEP_BY_STEP.md`](STEP_BY_STEP.md).

The simple version is:

1. Format the microSD card as `FAT32`.
2. Copy every file from `packages/sd_root_v3.2863.105/root/` to the root of the card.
3. Turn the camera off.
4. Insert the card.
5. Turn the camera on.
6. Wait for the camera to reconnect to Wi-Fi.
7. Open the stream in your client:

```text
rtsp://192.168.1.126:88/videoMain
rtsp://192.168.1.126:89/videoSub
```

8. Optional host-side verification:

```powershell
powershell -ExecutionPolicy Bypass -File tools/rtsp_probe_windows.ps1 `
  -CameraHost 192.168.1.126 `
  -Port 88 `
  -Path videoMain `
  -Kind video
```

9. Optional camera-side verification:

```bash
python3 tools/telnet_exec.py 192.168.1.126 --port 24 \
  --command 'ls -l /tmp/vendor_rtsp_boot.*; tail -n 40 /tmp/vendor_rtsp_boot.log; ps; netstat -an'
```

Expected result:

- the Tuya app still works
- `rtsp://CAMERA_IP:88/videoMain` plays
- `rtsp://CAMERA_IP:89/videoSub` plays
- `554` and `8554` remain closed unless you intentionally add another proxy later

Important note:

- the repository now mirrors the known-good SD backup layout more closely
- that includes the zero-byte `hack` sentinel and the original `hack.sh`/`custom.sh` pair from the working card
- the optional `8080` web UI files were removed from the primary bundle because the RTSP/Tuya coexistence path does not use them

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
