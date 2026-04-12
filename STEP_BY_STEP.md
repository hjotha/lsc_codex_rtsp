# Step-by-Step Guide

This is the simplest documented path for enabling RTSP on the tested LSC / Tuya / Anyka camera while keeping the stock Tuya app working.

## Tested version

- camera family:
  LSC rotatable / Tuya / Anyka
- firmware:
  `V3.2863.105`
- validated camera IP during development:
  `192.168.1.126`

## What this hack does

In simple terms:

- it keeps the stock firmware process running
- it wakes the vendor RTSP server already inside that stock firmware
- it keeps the Tuya app video path alive
- it makes RTSP available on:
  `88` for `videoMain`
  `89` for `videoSub`

It does not replace `anyka_ipc` and it does not require the old custom sidecar server.

## What you need

- the camera on stock firmware `V3.2863.105`
- a microSD card
- a computer to copy files to the microSD card

## The short version

1. Format the microSD card as `FAT32`.
   Do not use `exFAT`.
2. Copy every file from:
   `packages/sd_root_v3.2863.105/root/`
   to the root of the microSD card.
3. Safely eject the microSD card.
4. Turn the camera off.
5. Insert the microSD card.
6. Turn the camera on.
7. Wait for the camera to connect to Wi-Fi.
8. Open:
   `rtsp://CAMERA_IP:88/videoMain`
   `rtsp://CAMERA_IP:89/videoSub`

That is the main beginner workflow.

You do not copy `install_vendor_bootstrap.sh` to the SD card.
That script is only for advanced remote installation over telnet from a Linux host.

The SD bootstrap checks the running stock binary hash before patching.
If the camera is not running the tested `V3.2863.105` stock build, it refuses to apply the hack.

This bundle is now based on the real SD backup that was known to work on the tested camera.

## Files to copy

Copy everything from the bundle directory to the root of the microSD card.

The most important files are:

- `hack`
- `hack.sh`
- `custom.sh`
- `rtsp_kick`
- `vendor_rtsp_boot.sh`
- `vendor_rtsp_boot.md5`

They are already collected here:

- `packages/sd_root_v3.2863.105/root/`
- `sdcard/`

Keep both `hack` and `hack.sh` on the card.
On the tested camera, the known-good setup used:

- `hack` as a zero-byte sentinel file
- `hack.sh` as the real startup script

The optional old `8080` web UI files are no longer part of the main bundle.

## What happens on boot

After the power cycle:

- the stock firmware executes `/mnt/hack.sh` from the SD card
- `hack.sh` starts a loop that runs `/tmp/sd/custom.sh`
- `vendor_rtsp_boot.sh` copies `rtsp_kick` into `/tmp`
- it starts the stock vendor RTSP worker if needed
- it installs the video callback chain
- it writes:
  `/tmp/vendor_rtsp_boot.done`
  `/tmp/vendor_rtsp_boot.log`

One practical detail is normal:

- the first boot attempt can happen too early
- that first attempt may log a temporary `Protocol error`
- the next retry from the `custom.sh` loop succeeds

That behavior was seen during the validated cold-boot test and is expected.

## RTSP URLs

Use these URLs:

```text
rtsp://CAMERA_IP:88/videoMain
rtsp://CAMERA_IP:89/videoSub
```

On the validated camera, that meant:

```text
rtsp://192.168.1.126:88/videoMain
rtsp://192.168.1.126:89/videoSub
```

## Quick verification

### Check from Windows

```powershell
powershell -ExecutionPolicy Bypass -File tools/rtsp_probe_windows.ps1 `
  -CameraHost 192.168.1.126 `
  -Port 88 `
  -Path videoMain `
  -Kind video
```

### Check from the camera

```bash
python3 tools/telnet_exec.py 192.168.1.126 --port 24 \
  --command 'ls -l /tmp/vendor_rtsp_boot.*; tail -n 40 /tmp/vendor_rtsp_boot.log; ps; netstat -an'
```

Good signs are:

- `/tmp/vendor_rtsp_boot.done` exists
- the log says `vendor RTSP bootstrap finished successfully`
- `88` is listening
- `89` is listening
- `24` is listening
- `8554` is closed
- `554` is closed
- `anyka_ipc` is running

## What should be running inside the camera

In the validated final state:

- the stock `anyka_ipc` process is the real RTSP server
- `rtsp_kick` is only a bootstrap helper copied to `/tmp`
- there is no custom long-running sidecar RTSP server process

## If you want to remove the hack

Delete these files from the SD card root:

- `hack`
- `hack.sh`
- `custom.sh`
- `rtsp_kick`
- `vendor_rtsp_boot.sh`
- `vendor_rtsp_boot.md5`

Then reboot the camera without the hacked SD contents.
