#!/usr/bin/env python3
import argparse
import subprocess
import sys
import tempfile
from dataclasses import dataclass
from pathlib import Path
from typing import Iterable


HEADER_SIZE = 0x6000
ENTRY_START = 0x4E8
ENTRY_SIZE = 0x18
FRAME_HEADER_SIZE = 36
VIDEO_TYPE = 129

FRAME_WIDTH = 2304
FRAME_HEIGHT = 1296
FRAME_RGB_SIZE = FRAME_WIDTH * FRAME_HEIGHT * 3


@dataclass
class Entry:
    idx: int
    ts_ms: int
    seq: int
    type: int
    total_len: int
    frame: bytes
    structured: bool


def read_ring_entries(path: Path) -> list[Entry]:
    data = path.read_bytes()
    if len(data) <= HEADER_SIZE:
        raise ValueError(f"{path} is too small to be a ring dump")
    payload_size = len(data) - HEADER_SIZE
    entries: list[Entry] = []

    def read_frame(logical_offset: int, total_len: int) -> bytes:
        payload_off = logical_offset % payload_size
        first = min(total_len, payload_size - payload_off)
        frame = data[HEADER_SIZE + payload_off : HEADER_SIZE + payload_off + first]
        if first < total_len:
            frame += data[HEADER_SIZE : HEADER_SIZE + (total_len - first)]
        return frame

    max_entries = (HEADER_SIZE - ENTRY_START) // ENTRY_SIZE
    for idx in range(max_entries):
        off = ENTRY_START + idx * ENTRY_SIZE
        ts_ms = int.from_bytes(data[off + 0 : off + 8], "little")
        logical_offset = int.from_bytes(data[off + 8 : off + 12], "little")
        total_len = int.from_bytes(data[off + 12 : off + 16], "little")
        typ = int.from_bytes(data[off + 16 : off + 20], "little")
        seq = int.from_bytes(data[off + 20 : off + 24], "little")
        if typ != VIDEO_TYPE or total_len <= FRAME_HEADER_SIZE or total_len > payload_size or ts_ms == 0 or seq == 0:
            continue
        frame = read_frame(logical_offset, total_len)
        structured = frame_has_structured_prefix(frame, total_len)
        entries.append(
            Entry(
                idx=idx,
                ts_ms=ts_ms,
                seq=seq,
                type=typ,
                total_len=total_len,
                frame=frame,
                structured=structured,
            )
        )

    entries.sort(key=lambda e: (e.ts_ms, e.seq, e.idx))
    return entries


def frame_has_structured_prefix(frame: bytes, total_len: int) -> bool:
    if len(frame) <= FRAME_HEADER_SIZE:
        return False
    return (
        int.from_bytes(frame[4:8], "little") == 413
        and int.from_bytes(frame[8:12], "little") == total_len - FRAME_HEADER_SIZE
        and int.from_bytes(frame[12:16], "little") == 0
        and int.from_bytes(frame[16:20], "little") == 0
    )


def find_start_code(buf: bytes, offset: int = 0) -> tuple[int, int] | None:
    i = offset
    while i + 3 < len(buf):
        if buf[i] == 0 and buf[i + 1] == 0:
            if buf[i + 2] == 1:
                return i, 3
            if i + 4 < len(buf) and buf[i + 2] == 0 and buf[i + 3] == 1:
                return i, 4
        i += 1
    return None


def hevc_header_valid(nal: bytes) -> tuple[bool, int | None]:
    if len(nal) < 2:
        return False, None
    nal_type = (nal[0] >> 1) & 0x3F
    layer_id = ((nal[0] & 0x01) << 5) | ((nal[1] >> 3) & 0x1F)
    tid = nal[1] & 0x07
    if tid == 0 or layer_id != 0 or nal_type > 50:
        return False, None
    return True, nal_type


def first_key_start_offset(entry: Entry, structured_skip: int) -> int | None:
    payload = entry.frame[structured_skip:] if entry.structured else entry.frame
    offset = 0
    while True:
        found = find_start_code(payload, offset)
        if found is None:
            return None
        start, sc_size = found
        valid, nal_type = hevc_header_valid(payload[start + sc_size :])
        if valid and nal_type in {32, 33, 34, 19, 20}:
            score = hevc_sequence_score_from_start(payload, start)
            if score >= 3:
                return start
        offset = start + sc_size


def hevc_sequence_score_from_start(payload: bytes, start: int) -> int:
    has_vps = False
    has_sps = False
    has_pps = False
    has_idr = False
    offset = start
    while True:
        found = find_start_code(payload, offset)
        if found is None:
            break
        cur_start, sc_size = found
        valid, nal_type = hevc_header_valid(payload[cur_start + sc_size :])
        if not valid or nal_type is None:
            offset = cur_start + sc_size
            continue
        if nal_type == 32:
            has_vps = True
        elif nal_type == 33:
            has_sps = True
        elif nal_type == 34:
            has_pps = True
        elif nal_type in {19, 20}:
            has_idr = True
        nxt = find_start_code(payload, cur_start + sc_size)
        if nxt is None:
            break
        next_start, _ = nxt
        if next_start <= cur_start:
            break
        offset = next_start

    if has_vps and has_sps and has_pps and has_idr:
        return 4
    if has_sps and has_pps and has_idr:
        return 3
    if has_idr:
        return 2
    if has_vps or has_sps or has_pps:
        return 1
    return 0


def find_bootstrap_group(entries: list[Entry], structured_skip: int) -> tuple[Entry, list[Entry]]:
    for i, entry in enumerate(entries):
        if not entry.structured:
            continue
        key_start = first_key_start_offset(entry, structured_skip)
        if key_start is None:
            continue
        cont: list[Entry] = []
        for later in entries[i + 1 : i + 12]:
            if later.structured:
                break
            cont.append(later)
        if cont:
            return entry, cont
    raise RuntimeError("No bootstrap keyframe group found")


def build_idr_bytes_prefix(key_entry: Entry, cont_entries: Iterable[Entry], structured_skip: int, cont_skips: dict[int, int]) -> bytes:
    key_payload = key_entry.frame[structured_skip:] if key_entry.structured else key_entry.frame
    key_start = first_key_start_offset(key_entry, structured_skip)
    if key_start is None:
        raise RuntimeError(f"Could not find key start in entry {key_entry.idx}")

    bitstream = bytearray(key_payload[key_start:])
    for entry in cont_entries:
        skip = cont_skips.get(entry.idx, 0)
        if skip < len(entry.frame):
            bitstream += entry.frame[skip:]
    return bytes(bitstream)


def build_idr_bytes_tail(key_entry: Entry, cont_entries: Iterable[Entry], structured_skip: int, cont_trims: dict[int, int]) -> bytes:
    key_payload = key_entry.frame[structured_skip:] if key_entry.structured else key_entry.frame
    key_start = first_key_start_offset(key_entry, structured_skip)
    if key_start is None:
        raise RuntimeError(f"Could not find key start in entry {key_entry.idx}")

    bitstream = bytearray(key_payload[key_start:])
    for entry in cont_entries:
        trim = cont_trims.get(entry.idx, 0)
        if trim >= len(entry.frame):
            continue
        bitstream += entry.frame[: len(entry.frame) - trim]
    return bytes(bitstream)


def decode_first_frame_metrics(bitstream: bytes) -> dict[str, int]:
    with tempfile.TemporaryDirectory(prefix="scan_idr_") as tmpdir:
        src = Path(tmpdir) / "candidate.h265"
        src.write_bytes(bitstream)
        proc = subprocess.run(
            [
                "ffmpeg",
                "-v",
                "error",
                "-i",
                str(src),
                "-frames:v",
                "1",
                "-f",
                "rawvideo",
                "-pix_fmt",
                "rgb24",
                "-",
            ],
            stdout=subprocess.PIPE,
            stderr=subprocess.PIPE,
            check=False,
        )
        raw = proc.stdout
        stderr = proc.stderr.decode("utf-8", "replace")

    if len(raw) != FRAME_RGB_SIZE:
        return {
            "decode_ok": 0,
            "green_start": -1,
            "pink_rows": FRAME_HEIGHT,
            "err_penalty": 999,
        }

    mv = memoryview(raw)
    green_start = FRAME_HEIGHT
    pink_rows = 0
    for y in range(FRAME_HEIGHT):
        row = mv[y * FRAME_WIDTH * 3 : (y + 1) * FRAME_WIDTH * 3]
        green_dominant = 0
        pink_magenta = 0
        for x in range(0, len(row), 3):
            r = row[x]
            g = row[x + 1]
            b = row[x + 2]
            if g > max(r, b) + 25 and g > 70:
                green_dominant += 1
            if r > 150 and b > 150 and g < 180:
                pink_magenta += 1
        if green_start == FRAME_HEIGHT and green_dominant / FRAME_WIDTH > 0.9:
            green_start = y
        if pink_magenta / FRAME_WIDTH > 0.8:
            pink_rows += 1

    err_penalty = stderr.count("cu_qp_delta") + stderr.count("PPS") + stderr.count("SPS") + stderr.count("VPS")
    return {
        "decode_ok": 1,
        "green_start": green_start,
        "pink_rows": pink_rows,
        "err_penalty": err_penalty,
    }


def metric_key(metrics: dict[str, int]) -> tuple[int, int, int, int]:
    return (
        metrics["decode_ok"],
        metrics["green_start"],
        -metrics["pink_rows"],
        -metrics["err_penalty"],
    )


def print_single_scan(key_entry: Entry, cont_entries: list[Entry], structured_skip: int, max_skip: int, top_n: int, mode: str) -> None:
    base_skips = {entry.idx: 0 for entry in cont_entries}
    for target in cont_entries:
        rows = []
        for skip in range(max_skip + 1):
            cont_skips = dict(base_skips)
            cont_skips[target.idx] = skip
            if mode == "prefix":
                bitstream = build_idr_bytes_prefix(key_entry, cont_entries, structured_skip, cont_skips)
            else:
                bitstream = build_idr_bytes_tail(key_entry, cont_entries, structured_skip, cont_skips)
            metrics = decode_first_frame_metrics(bitstream)
            rows.append((metric_key(metrics), skip, metrics))
        rows.sort(reverse=True)
        label = "skip" if mode == "prefix" else "trim"
        print(f"single-scan mode={mode} target idx={target.idx}")
        for _, skip, metrics in rows[:top_n]:
            print(
                f"  {label}=%02d decode_ok=%d green_start=%d pink_rows=%d err_penalty=%d"
                % (
                    skip,
                    metrics["decode_ok"],
                    metrics["green_start"],
                    metrics["pink_rows"],
                    metrics["err_penalty"],
                )
            )
        print()


def print_pair_scan(key_entry: Entry, cont_entries: list[Entry], structured_skip: int, max_skip: int, top_n: int) -> None:
    if len(cont_entries) < 2:
        return

    first = cont_entries[0]
    second = cont_entries[1]
    rows = []
    for skip_a in range(max_skip + 1):
        for skip_b in range(max_skip + 1):
            cont_skips = {entry.idx: 0 for entry in cont_entries}
            cont_skips[first.idx] = skip_a
            cont_skips[second.idx] = skip_b
            metrics = decode_first_frame_metrics(build_idr_bytes_prefix(key_entry, cont_entries, structured_skip, cont_skips))
            rows.append((metric_key(metrics), skip_a, skip_b, metrics))
    rows.sort(reverse=True)

    print(f"pair-scan targets idx={first.idx},{second.idx} (others fixed at 0)")
    for _, skip_a, skip_b, metrics in rows[:top_n]:
        print(
            "  skip_%d=%02d skip_%d=%02d decode_ok=%d green_start=%d pink_rows=%d err_penalty=%d"
            % (
                first.idx,
                skip_a,
                second.idx,
                skip_b,
                metrics["decode_ok"],
                metrics["green_start"],
                metrics["pink_rows"],
                metrics["err_penalty"],
            )
        )
    print()


def main() -> int:
    parser = argparse.ArgumentParser(description="Scan candidate skip values for the continuation chunks of the bootstrap IDR.")
    parser.add_argument("ring", type=Path, help="Path to VideoMainStream0.full style dump")
    parser.add_argument("--structured-skip", type=int, default=36, help="Bytes to drop from structured type=129 entries")
    parser.add_argument("--max-skip", type=int, default=48, help="Maximum continuation skip to test")
    parser.add_argument("--top", type=int, default=10, help="How many top candidates to print")
    parser.add_argument("--mode", choices=("prefix", "tail"), default="prefix", help="Whether to scan prefix skips or tail trims on continuation chunks")
    parser.add_argument("--no-pair", action="store_true", help="Skip the expensive pair scan and print only single-variable scans")
    args = parser.parse_args()

    entries = read_ring_entries(args.ring)
    key_entry, cont_entries = find_bootstrap_group(entries, args.structured_skip)

    print(f"key-entry idx={key_entry.idx} seq={key_entry.seq} total_len={key_entry.total_len}")
    print("continuations:", " ".join(f"idx={entry.idx}:len={entry.total_len}" for entry in cont_entries))
    print()

    print_single_scan(key_entry, cont_entries, args.structured_skip, args.max_skip, args.top, args.mode)
    if args.mode == "prefix" and not args.no_pair:
        print_pair_scan(key_entry, cont_entries, args.structured_skip, args.max_skip, args.top)
    return 0


if __name__ == "__main__":
    sys.exit(main())
