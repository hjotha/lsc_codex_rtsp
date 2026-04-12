#!/usr/bin/env python3
import argparse
import re
import socket
import sys
from dataclasses import dataclass


RTSP_VERSION = "RTSP/1.0"


@dataclass
class RtspResponse:
    status_line: str
    headers: dict[str, str]
    body: bytes


def recv_exact(sock: socket.socket, size: int) -> bytes:
    chunks: list[bytes] = []
    remaining = size
    while remaining > 0:
        chunk = sock.recv(remaining)
        if not chunk:
            raise RuntimeError("connection closed while reading RTSP body")
        chunks.append(chunk)
        remaining -= len(chunk)
    return b"".join(chunks)


def recv_rtsp_response(sock: socket.socket) -> RtspResponse:
    data = b""
    while b"\r\n\r\n" not in data:
        chunk = sock.recv(4096)
        if not chunk:
            raise RuntimeError("connection closed while reading RTSP headers")
        data += chunk

    header_blob, remainder = data.split(b"\r\n\r\n", 1)
    header_lines = header_blob.decode("latin1", "replace").split("\r\n")
    status_line = header_lines[0]
    headers: dict[str, str] = {}
    for line in header_lines[1:]:
        if ":" not in line:
            continue
        key, value = line.split(":", 1)
        headers[key.strip().lower()] = value.strip()

    body = remainder
    content_length = int(headers.get("content-length", "0"))
    if len(body) < content_length:
        body += recv_exact(sock, content_length - len(body))
    elif len(body) > content_length:
        body = body[:content_length]

    return RtspResponse(status_line=status_line, headers=headers, body=body)


def send_rtsp(sock: socket.socket, cseq: int, method: str, uri: str, extra_headers: list[str]) -> RtspResponse:
    lines = [f"{method} {uri} {RTSP_VERSION}", f"CSeq: {cseq}", "User-Agent: codex-rtsp-probe"]
    lines.extend(extra_headers)
    payload = ("\r\n".join(lines) + "\r\n\r\n").encode("ascii")
    sock.sendall(payload)
    response = recv_rtsp_response(sock)
    print(response.status_line)
    for key, value in response.headers.items():
        print(f"{key}: {value}")
    if response.body:
        print()
        print(response.body.decode("latin1", "replace"))
    print()
    return response


def parse_track_uri(base_uri: str, sdp: str, kind: str) -> str:
    current_kind = None
    for line in sdp.splitlines():
        if line.startswith("m="):
            if line.startswith("m=video "):
                current_kind = "video"
            elif line.startswith("m=audio "):
                current_kind = "audio"
            else:
                current_kind = None
            continue
        if current_kind != kind:
            continue
        if not line.startswith("a=control:"):
            continue
        control = line[len("a=control:") :].strip()
        if control.startswith("rtsp://"):
            return control
        return f"{base_uri.rstrip('/')}/{control.lstrip('/')}"
    raise RuntimeError(f"could not find {kind} track control URI in SDP")


def describe_rtp_packet(packet: bytes) -> str:
    if len(packet) < 12:
        return f"short packet ({len(packet)} bytes)"
    version = packet[0] >> 6
    payload_type = packet[1] & 0x7F
    marker = (packet[1] >> 7) & 1
    seq = int.from_bytes(packet[2:4], "big")
    timestamp = int.from_bytes(packet[4:8], "big")
    ssrc = int.from_bytes(packet[8:12], "big")
    return (
        f"len={len(packet)} version={version} pt={payload_type} marker={marker} "
        f"seq={seq} ts={timestamp} ssrc=0x{ssrc:08x}"
    )


def session_id(headers: dict[str, str]) -> str:
    raw = headers.get("session")
    if not raw:
        raise RuntimeError("SETUP response did not include Session header")
    return raw.split(";", 1)[0].strip()


def main() -> int:
    parser = argparse.ArgumentParser(description="Minimal RTSP UDP probe for the Anyka/Tuya vendor RTSP server.")
    parser.add_argument("host", help="Camera IP or hostname")
    parser.add_argument("--port", type=int, default=88, help="RTSP TCP port")
    parser.add_argument("--path", default="videoMain", help="RTSP path, e.g. videoMain or videoSub")
    parser.add_argument(
        "--kind",
        choices=("video", "audio"),
        default="video",
        help="Which media section to SETUP/PLAY",
    )
    parser.add_argument("--client-port", type=int, default=50000, help="Local RTP UDP port")
    parser.add_argument("--packets", type=int, default=5, help="How many RTP packets to wait for after PLAY")
    parser.add_argument("--timeout", type=float, default=3.0, help="Socket timeout in seconds")
    args = parser.parse_args()

    base_uri = f"rtsp://{args.host}:{args.port}/{args.path}"
    rtsp = socket.socket(socket.AF_INET, socket.SOCK_STREAM)
    rtsp.settimeout(args.timeout)

    rtp = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
    rtcp = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
    rtp.settimeout(args.timeout)
    rtcp.settimeout(args.timeout)
    rtp.bind(("", args.client_port))
    rtcp.bind(("", args.client_port + 1))

    try:
        rtsp.connect((args.host, args.port))

        send_rtsp(rtsp, 1, "OPTIONS", base_uri, [])
        describe = send_rtsp(rtsp, 2, "DESCRIBE", base_uri, ["Accept: application/sdp"])
        if "200" not in describe.status_line:
            raise RuntimeError("DESCRIBE did not return 200 OK")

        sdp = describe.body.decode("latin1", "replace")
        track_uri = parse_track_uri(base_uri, sdp, args.kind)
        print(f"Selected {args.kind} track URI: {track_uri}\n")

        setup = send_rtsp(
            rtsp,
            3,
            "SETUP",
            track_uri,
            [f"Transport: RTP/AVP;unicast;client_port={args.client_port}-{args.client_port + 1}"],
        )
        if "200" not in setup.status_line:
            raise RuntimeError("SETUP did not return 200 OK")
        sid = session_id(setup.headers)

        play = send_rtsp(rtsp, 4, "PLAY", base_uri, [f"Session: {sid}", "Range: npt=0.000-"])
        if "200" not in play.status_line:
            raise RuntimeError("PLAY did not return 200 OK")

        print(f"Waiting for up to {args.packets} RTP packets on UDP {args.client_port}...\n")
        received = 0
        while received < args.packets:
            packet, source = rtp.recvfrom(65535)
            received += 1
            print(f"RTP[{received}] from {source[0]}:{source[1]} {describe_rtp_packet(packet)}")

        return 0
    except Exception as exc:
        print(f"ERROR: {exc}", file=sys.stderr)
        return 1
    finally:
        try:
            rtp.close()
        finally:
            rtcp.close()
            rtsp.close()


if __name__ == "__main__":
    raise SystemExit(main())
