# SD Root Bundle for `V3.2863.93`

This folder is the SD-ready package for the validated `V3.2863.93` camera
firmware.

It was validated against:

- firmware: `V3.2863.93`
- running stock binary md5: `87f1683cee35353fb2c2be20353bf59c`

## Relationship To The `V3.2863.105` Bundle

The root bundle implementation is shared with `V3.2863.105`.

As of `2026-05-18`, every file under:

- `packages/sd_root_v3.2863.93/root/`
- `packages/sd_root_v3.2863.105/root/`

is byte-identical except:

- `vendor_rtsp_boot.md5`

The shared `vendor_rtsp_boot.sh` detects the running stock `anyka_ipc` MD5 and
selects the correct RTSP offsets automatically. The `V3.2863.93` path passes
explicit offset flags to `rtsp_kick`; the `V3.2863.105` path uses the current
`rtsp_kick` defaults.

See `../../BUNDLE_COMPATIBILITY.md` for the full comparison.

## What To Copy

Format the SD card as `FAT32`, not `exFAT`.

Copy every file from:

- `packages/sd_root_v3.2863.93/root/`

into the root of the SD card:

- `_ht_ap_mode.conf`
- `hack`
- `hack.sh`
- `hostapd`
- `custom.sh`
- `rtsp_kick`
- `vendor_rtsp_boot.sh`
- `vendor_rtsp_boot.md5`

After copying the files:

1. insert the SD card into the camera
2. power-cycle the camera
3. wait for Wi-Fi to reconnect
4. open `rtsp://CAMERA_IP:88/videoMain`
5. open `rtsp://CAMERA_IP:89/videoSub`

## Safety Behavior

The bootstrap checks the running stock `anyka_ipc` MD5 before patching. If the
binary is not one of the known validated builds and there is no explicit
research override, it refuses to patch and writes:

```text
/tmp/vendor_rtsp_boot.unsupported
```

Logs are written to:

```text
/tmp/vendor_rtsp_boot.log
```

For intentional unsupported-firmware research, create this marker on the SD
card:

```text
vendor_rtsp_boot.allow_unsupported
```

That marker should not be used for the normal documented path.
