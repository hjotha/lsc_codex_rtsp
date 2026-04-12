# SD Root Bundle for `V3.2863.105`

This folder is the simplest packaging for the validated camera firmware.

## Important

This is not a universal package for every LSC / Tuya / Anyka camera.

It was validated against:

- firmware:
  `V3.2863.105`
- running stock binary md5:
  `c31358a8f598c56073720e96c004fa9c`

The bootstrap now checks that md5 before patching. If the running `anyka_ipc` does not match, it refuses to apply the hack.

## What this bundle assumes

This bundle is meant to be copied onto a blank SD card root for the tested stock firmware path.

The stock camera on the tested firmware executes:

- `/mnt/hack.sh`

from the SD card during boot.

## What to copy

Format the SD card as `FAT32`.

Do not use `exFAT`.

Then copy every file from:

- `packages/sd_root_v3.2863.105/root/`

into the root of the SD card.

That means copying:

- `hack`
- `hack.sh`
- `custom.sh`
- `rtsp_kick`
- `vendor_rtsp_boot.sh`
- `vendor_rtsp_boot.md5`

## What happens after that

After you copy the files:

1. insert the SD card into the camera
2. power-cycle the camera
3. wait for the camera to reconnect to Wi-Fi
4. open:
   `rtsp://CAMERA_IP:88/videoMain`
   `rtsp://CAMERA_IP:89/videoSub`

That is the beginner path.

`install_vendor_bootstrap.sh` is not copied to the SD card.
It is a host-side helper for people who already have a working hacked SD setup and want to patch it remotely over telnet.

Keep both `hack` and `hack.sh` on the card.
On the tested camera, that is the safest compatibility setup.

## Safety behavior

If the running stock `anyka_ipc` binary hash does not match the tested value, the bootstrap logs the mismatch and refuses to patch.

It will write:

- `/tmp/vendor_rtsp_boot.unsupported`

You can inspect the reason in:

- `/tmp/vendor_rtsp_boot.log`

## Research override

If you are intentionally testing another firmware and want to bypass the MD5 guard, create this marker file on the SD card:

- `vendor_rtsp_boot.allow_unsupported`

That override is for research only, not for the documented safe path.
