#!/usr/bin/env python3
import argparse
import base64
import pathlib
import sys
import telnetlib
import time


def write_safely(tn: telnetlib.Telnet, data: bytes, chunk_size: int, chunk_delay: float) -> None:
    if chunk_size <= 0 or len(data) <= chunk_size:
        tn.write(data)
        return

    for offset in range(0, len(data), chunk_size):
        tn.write(data[offset : offset + chunk_size])
        if chunk_delay > 0:
            time.sleep(chunk_delay)


def run_telnet_script(
    host: str,
    port: int,
    wait: float,
    chunk_size: int,
    chunk_delay: float,
    script: str,
) -> bytes:
    if not script.endswith("\n"):
        script += "\n"
    if "exit\n" not in script:
        script += "exit\n"

    tn = telnetlib.Telnet(host, port, timeout=5)
    try:
        time.sleep(wait)
        try:
            tn.read_very_eager()
        except EOFError:
            pass

        write_safely(tn, script.encode("utf-8"), chunk_size, chunk_delay)
        time.sleep(wait)
        try:
            return tn.read_very_eager()
        except EOFError:
            return b""
    finally:
        tn.close()


def make_append_script(remote_b64: str, chunk: str) -> str:
    return f"printf '%s' '{chunk}' >> {remote_b64}\n"


def main() -> int:
    parser = argparse.ArgumentParser(description="Upload a local file to a remote target over telnet using base64 chunks.")
    parser.add_argument("host", help="Target host")
    parser.add_argument("local_file", help="Local file to upload")
    parser.add_argument("remote_path", help="Destination path on the remote system")
    parser.add_argument("--port", type=int, default=24, help="Target telnet port")
    parser.add_argument("--wait", type=float, default=0.8, help="Seconds to wait after connect and send")
    parser.add_argument("--chunk-size", type=int, default=512, help="Bytes per telnet write call")
    parser.add_argument("--chunk-delay", type=float, default=0.01, help="Seconds between telnet writes")
    parser.add_argument("--chunk-lines", type=int, default=4, help="How many 76-char base64 lines worth of data to append per printf command")
    parser.add_argument("--commands-per-session", type=int, default=64, help="How many append commands to bundle into one telnet session")
    parser.add_argument("--mode", default="755", help="Mode to apply to the remote file after decode")
    args = parser.parse_args()

    local_path = pathlib.Path(args.local_file)
    data = local_path.read_bytes()
    b64_text = base64.b64encode(data).decode("ascii")
    chunk_chars = max(76, args.chunk_lines * 76)
    wrapped = [b64_text[i : i + chunk_chars] for i in range(0, len(b64_text), chunk_chars)]
    remote_b64 = f"{args.remote_path}.b64"

    try:
        run_telnet_script(
            args.host,
            args.port,
            args.wait,
            args.chunk_size,
            args.chunk_delay,
            f"rm -f {args.remote_path} {remote_b64}",
        )

        total_batches = len(wrapped)
        for batch_start in range(0, total_batches, args.commands_per_session):
            batch_end = min(batch_start + args.commands_per_session, total_batches)
            script = "".join(
                make_append_script(remote_b64, wrapped[index])
                for index in range(batch_start, batch_end)
            )
            output = run_telnet_script(
                args.host,
                args.port,
                args.wait,
                args.chunk_size,
                args.chunk_delay,
                script,
            )
            start = batch_start * chunk_chars
            end = min(batch_end * chunk_chars, len(b64_text))
            sys.stdout.write(
                f"[{batch_start + 1}-{batch_end}/{total_batches}] appended base64 chars {start + 1}-{end}\n"
            )
            if output:
                sys.stdout.buffer.write(output)

        final_output = run_telnet_script(
            args.host,
            args.port,
            args.wait,
            args.chunk_size,
            args.chunk_delay,
            "\n".join(
                [
                    f"base64 -d {remote_b64} > {args.remote_path}",
                    f"chmod {args.mode} {args.remote_path}",
                    f"ls -l {args.remote_path}",
                    f"wc -c < {args.remote_path}",
                    f"rm -f {remote_b64}",
                ]
            ),
        )
        if final_output:
            sys.stdout.buffer.write(final_output)
    except Exception as exc:
        print(f"ERROR: {exc}", file=sys.stderr)
        return 1

    return 0


if __name__ == "__main__":
    raise SystemExit(main())
