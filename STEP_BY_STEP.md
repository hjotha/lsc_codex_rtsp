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

- the camera connected to your Wi-Fi
- the camera reachable by IP
- telnet access on port `24`
- the SD card inserted in the camera
- the SD hack environment already working so the camera runs `/tmp/sd/custom.sh`
- a host machine with:
  `bash`
  `python3`
  access to this repository

## What you must do manually

These are the manual actions:

1. Put the SD card in the camera.
2. Power the camera on and let it connect to Wi-Fi.
3. Find the camera IP address.
4. After the installer finishes, unplug the camera power.
5. Wait about 10 to 15 seconds.
6. Plug the camera back in.
7. Open the RTSP URL in your player.

## What you run on the host

Open a shell in this repository and run:

```bash
bash scripts/install_vendor_bootstrap.sh 192.168.1.126 24
```

What that command does:

- builds `rtsp_kick`
- uploads it to `/tmp/sd/rtsp_kick`
- uploads `sdcard/vendor_rtsp_boot.sh` to `/tmp/sd/vendor_rtsp_boot.sh`
- patches `/tmp/sd/custom.sh` so the bootstrap helper runs automatically on boot
- keeps a backup at:
  `/tmp/sd/custom.sh.pre_vendor_rtsp`

## What happens on boot

After the power cycle:

- the stock SD hack loop starts
- `/tmp/sd/custom.sh` runs
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
- `8554` is closed
- `554` is closed
- `anyka_ipc` is running

## What should be running inside the camera

In the validated final state:

- the stock `anyka_ipc` process is the real RTSP server
- `rtsp_kick` is only a bootstrap helper copied to `/tmp`
- there is no custom long-running sidecar RTSP server process

## If you want to disable the bootstrap

The installer keeps a backup of `custom.sh`:

```text
/tmp/sd/custom.sh.pre_vendor_rtsp
```

So the safe manual rollback is:

1. connect by telnet
2. restore the backup over `/tmp/sd/custom.sh`
3. remove `/tmp/sd/vendor_rtsp_boot.sh`
4. optionally remove `/tmp/sd/rtsp_kick`
5. reboot the camera
