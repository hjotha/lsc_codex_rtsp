#!/usr/bin/env python3
import argparse
import pathlib
import sys
import telnetlib
import time


def main() -> int:
    parser = argparse.ArgumentParser(description="Run commands over telnet and print output.")
    parser.add_argument("host", help="Target host")
    parser.add_argument("--port", type=int, default=24, help="Target port")
    parser.add_argument(
        "--wait",
        type=float,
        default=1.0,
        help="Seconds to wait after connecting and after sending commands",
    )
    group = parser.add_mutually_exclusive_group(required=True)
    group.add_argument("--command", help="Single command string to send")
    group.add_argument("--file", help="Path to a file containing commands to send")
    args = parser.parse_args()

    if args.file:
        data = pathlib.Path(args.file).read_text(encoding="utf-8")
    else:
        data = args.command

    if not data.endswith("\n"):
        data += "\n"
    if "exit\n" not in data:
        data += "exit\n"

    try:
        tn = telnetlib.Telnet(args.host, args.port, timeout=5)
        time.sleep(args.wait)
        try:
            banner = tn.read_very_eager()
            if banner:
                sys.stdout.buffer.write(banner)
        except EOFError:
            pass
        tn.write(data.encode("utf-8"))
        time.sleep(args.wait)
        try:
            output = tn.read_very_eager()
            if output:
                sys.stdout.buffer.write(output)
        except EOFError:
            pass
        tn.close()
    except Exception as exc:
        print(f"ERROR: {exc}", file=sys.stderr)
        return 1

    return 0


if __name__ == "__main__":
    raise SystemExit(main())
