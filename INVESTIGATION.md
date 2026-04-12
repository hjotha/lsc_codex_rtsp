# Investigation Notes

This file keeps a compact English record of the main findings so a future context reset does not send the work back to zero.

## Current bottom line

On firmware `V3.2863.105`, the stock vendor RTSP path can coexist with the Tuya app.

The working recipe is:

1. wake the stock RTSP worker with `ht_rtsp_start`
2. install a video callback chain so Tuya keeps receiving frames
3. also feed those frames into `ht_rtsp_send_video_frame`

That gives live RTSP on:

- `rtsp://CAMERA_IP:88/videoMain`
- `rtsp://CAMERA_IP:89/videoSub`

Validated on live camera `192.168.1.126` on `2026-04-12`.

## What we proved live

After the detour was installed:

- `anyka_ipc` stayed alive
- the Tuya app remained usable in manual testing
- port `88` was listening
- port `89` was listening
- port `554` was closed
- port `8554` was closed

The following control-plane operations succeeded:

- `OPTIONS`
- `DESCRIBE`
- `SETUP`
- `PLAY`

The following media paths were proven with live RTP/UDP capture from the Windows host:

- `88/videoMain/track1` = H.265
- `88/videoMain/track2` = PCMA
- `89/videoSub/track1` = H.265
- `89/videoSub/track2` = PCMA

## Why this took more than a one-shot kick

Calling `ht_rtsp_start` alone was not enough.

That only:

- created the worker thread
- opened the RTSP listeners

It did not ensure that video frames were reaching the vendor RTSP path.

The missing piece was the callback topology:

- video callback slots are single-slot
- the live Tuya wrappers were already occupying those slots

So the stock RTSP path needed an explicit chain instead of a simple replacement.

## Important callback findings

The normal Tuya boot path was observed using these live callback values:

- video slot 0:
  `0x000897cc`
- video slot 1:
  `0x000898f4`
- audio callback already pointing into RTSP path on one channel:
  `0x000d13d8`

This led to the key conclusion:

- audio was not the main blocker
- video delivery into the RTSP path was the real missing link

## Why ports `88` and `89` matter

The stock worker used by the safe detour serves on:

- `88`
- `89`

The reason `554` is not the answer here:

- `554` belongs to the factory RTSP path
- the factory path was not chosen as the safe coexistence route

The reason `8554` is not active:

- `8554` belonged to the old custom sidecar
- that sidecar is now archived under `unused/sidecar/`

## WSL note

Live RTP validation from `WSL2` produced a false negative:

- RTSP control worked
- UDP RTP looked dead

The issue was network topology, not camera behavior. When the same probe was run from native Windows, RTP arrived immediately. That is why the final proof in this repo uses:

- `tools/rtsp_probe_windows.ps1`

## Legacy sidecar status

The custom sidecar is no longer the main path.

It was useful for reverse engineering and for proving that stock ring buffers contained enough media to expose a stream, but it had major drawbacks:

- much more complicated HEVC reconstruction
- more moving parts
- no longer necessary for day-to-day RTSP access once the vendor path was proven

It has now been moved to:

- `unused/sidecar/`

## Open items

The most useful next tasks are:

- run longer soak tests with the Tuya app active
- decide whether a relay to `554` is worth implementing
- keep the sidecar only as a lab tool, not the primary delivery path

## Addendum 2026-04-12: boot persistence path

The tested camera already had a useful SD hack loop:

- `/mnt/hack.sh` mounts the SD card
- then it runs `/tmp/sd/custom.sh` every 10 seconds

That means the clean persistence path is:

- keep the stock firmware binary untouched
- place `rtsp_kick` on the SD card
- add an idempotent bootstrap helper called from `custom.sh`

This repo now includes that path:

- `sdcard/hack`
- `sdcard/hack.sh`
- `sdcard/custom.sh`
- `sdcard/vendor_rtsp_boot.sh`
- `sdcard/vendor_rtsp_boot.md5`
- `packages/sd_root_v3.2863.105/`
- `scripts/install_vendor_bootstrap.sh`

Important result from the same investigation:

- the firmware did not expose a usable `iptables` binary
- therefore a trivial NAT-based alias from `8554` to `88` was not available
- a raw TCP forward was intentionally not enabled by default because the stock RTSP SDP advertises absolute control URLs on `88` and `89`

## Addendum 2026-04-12: validated cold boot

The SD bootstrap path was then validated with a real unplug/replug cold boot on the tested camera.

Observed sequence:

- first `custom.sh` pass copied `rtsp_kick` into `/tmp`
- the very first `ht_rtsp_start` attempt was too early and logged a temporary `Protocol error`
- the same early pass saw video callback slots still at `0x00000000`
- the next `custom.sh` retry about 10 seconds later succeeded
- `ht_rtsp_start` completed cleanly
- the callback chain installed against the expected live Tuya wrappers
- `/tmp/vendor_rtsp_boot.done` was created

Observed steady state after the successful cold boot:

- stock `anyka_ipc` still running
- no custom long-running RTSP sidecar process
- listeners on `23`, `24`, `88`, `89`, `6668`, and `8080` on the older validation SD card
- `554` closed
- `8554` closed
- live RTP still present on `88/videoMain/track1`

That result closes the boot automation item for the tested firmware and SD hack environment.

## Addendum 2026-04-12: simpler blank-SD bundle

The SD mount on the tested camera was:

- `vfat`

So the beginner docs now recommend:

- `FAT32`, not exFAT

The repository also now contains a simpler blank-card bundle:

- `packages/sd_root_v3.2863.105/root/`

The key simplification was recognizing that the essential boot path only needs:

- `hack`
- `hack.sh`
- `custom.sh`
- `rtsp_kick`
- `vendor_rtsp_boot.sh`
- `vendor_rtsp_boot.md5`

The MD5 file gates the hack to the tested stock binary:

- `c31358a8f598c56073720e96c004fa9c`

That keeps the beginner path simple while still refusing unsupported firmware by default.

## Addendum 2026-04-12: restore hack compatibility file

Removing the SD-root `hack` file caused a regression in manual testing:

- the camera came back on Wi-Fi
- `6668` was open
- `24`, `88`, and `89` stayed closed

That strongly suggests the tested firmware path is sensitive to the presence of the SD-root `hack` compatibility file.

The repository now ships both:

- `hack`
- `hack.sh`

And `hack` is no longer an empty marker. It now carries the same startup script body as `hack.sh` to maximize compatibility.
