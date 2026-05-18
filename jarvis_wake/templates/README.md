# Jarvis Templates

Place camera-recorded raw templates here.

Expected first file:

```text
templates/jarvis_01.raw
```

Format:

- signed 16-bit little-endian PCM
- mono
- 8000 Hz unless `jarvis_wake` is run with `--rate 16000`
- no WAV header

Record from the camera microphone path when possible:

```bash
../record_template.sh rtsp 192.168.1.130 jarvis_01.raw 3 8000
```

Raw template files are local calibration artifacts and are ignored by git.
