# Archived Sidecar

This folder contains the older custom RTSP sidecar that rebuilt media from the stock Anyka ring buffers and served it on:

- `rtsp://CAMERA_IP:8554/main_ch`

It was useful for reverse engineering, but it is no longer the preferred path because the stock vendor RTSP detour is now proven and much cleaner.

Why this folder exists:

- to keep the working vendor RTSP path at the repository root
- to preserve the older sidecar work without deleting it
- to make it clear that `8554` is legacy, not the primary recommendation

Contents:

- `scripts/`
- `src/`
- `tools/`

Use the root-level documentation for the current recommended workflow.
