# SD Bundle Compatibility

This note records the current relationship between the SD bundles for
`V3.2863.105` and `V3.2863.93`.

## Short Answer

The two SD root bundles use the same implementation.

As of `2026-05-18`, the files under these two root directories are byte-identical
except for the MD5 hint file:

- `packages/sd_root_v3.2863.105/root/`
- `packages/sd_root_v3.2863.93/root/`

The only intentional root-file difference is:

- `root/vendor_rtsp_boot.md5`

That file names the matching stock `anyka_ipc` MD5 for the package label:

| Bundle | `vendor_rtsp_boot.md5` value |
|---|---|
| `V3.2863.105` | `c31358a8f598c56073720e96c004fa9c` |
| `V3.2863.93` | `87f1683cee35353fb2c2be20353bf59c` |

## Shared Files

The following root files are intentionally shared by both bundles:

| File | Current MD5 |
|---|---|
| `_ht_ap_mode.conf` | `d41d8cd98f00b204e9800998ecf8427e` |
| `hack` | `d41d8cd98f00b204e9800998ecf8427e` |
| `hack.sh` | `ebfb480740a70387173b65197784d197` |
| `hostapd` | `c7beec53bc806c97ef5ddcb1b375ed94` |
| `custom.sh` | `e33012e33282b150d299824c2c408df2` |
| `rtsp_kick` | `22c340acae830bb884d7b9d1900e0543` |
| `vendor_rtsp_boot.sh` | `b53454f0d064d5f58808ef05974e3d7f` |

## How One Script Supports Both Firmwares

`vendor_rtsp_boot.sh` has a builtin table of supported stock `anyka_ipc` MD5s:

- `V3.2863.105`: `c31358a8f598c56073720e96c004fa9c`
- `V3.2863.93`: `87f1683cee35353fb2c2be20353bf59c`

The script computes the MD5 of the running `/usr/bin/anyka_ipc`, then selects
the proper RTSP offsets.

For `V3.2863.105`, the script passes no extra offset flags because the current
`rtsp_kick` defaults match that firmware.

For `V3.2863.93`, the script passes explicit offset flags for:

- `ht_rtsp_start`
- RTSP guard
- `malloc@plt`
- `ht_rtsp_send_video_frame`
- video callback slots
- expected Tuya video callback wrappers

That means the same `rtsp_kick` binary and the same `vendor_rtsp_boot.sh` can be
used on both packages. The package label and MD5 hint file are mostly there to
make the SD card contents self-describing and to preserve the older
single-firmware guard behavior.

## Validation State

Current live validation:

| Camera | IP | Firmware | Stock MD5 | State |
|---|---|---|---|---|
| `quintal` | `192.168.1.165` | `V3.2863.93` | `87f1683cee35353fb2c2be20353bf59c` | RTSP and speaker playback validated |
| `sala` | `192.168.1.130` | `V3.2863.105` | `c31358a8f598c56073720e96c004fa9c` | RTSP and speaker playback validated |

The `sala` camera used the `V3.2863.105` package label but the same root
implementation as the `V3.2863.93` bundle, with firmware-specific behavior
selected at runtime by MD5.
