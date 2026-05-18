#!/usr/bin/env python3
"""Upload a file to the camera through telnet plus a temporary BusyBox nc listener."""

from __future__ import annotations

import argparse
import pathlib
import shlex
import shutil
import subprocess
import sys
import time


REPO_ROOT = pathlib.Path(__file__).resolve().parents[1]
TELNET_EXEC = REPO_ROOT / "tools" / "telnet_exec.py"


def run_checked(cmd: list[str], *, timeout: float) -> subprocess.CompletedProcess[str]:
    return subprocess.run(
        cmd,
        cwd=str(REPO_ROOT),
        text=True,
        stdout=subprocess.PIPE,
        stderr=subprocess.STDOUT,
        timeout=timeout,
        check=False,
    )


def telnet(host: str, command: str, port: int, wait: float, timeout: float) -> str:
    result = run_checked(
        [
            sys.executable,
            str(TELNET_EXEC),
            host,
            "--port",
            str(port),
            "--wait",
            str(wait),
            "--command",
            command,
        ],
        timeout=timeout,
    )
    if result.returncode != 0:
        raise RuntimeError(result.stdout)
    return result.stdout


def upload_nc(
    host: str,
    local: pathlib.Path,
    remote: str,
    telnet_port: int,
    wait: float,
    nc_port: int,
    mode: str,
) -> str:
    if shutil.which("nc") is None:
        raise RuntimeError("local nc is required")

    size = local.stat().st_size
    remote_q = shlex.quote(remote)
    remote_log = shlex.quote(f"{remote}.nc.log")
    remote_wc = shlex.quote(f"{remote}.wc")
    mode_q = shlex.quote(mode)

    setup = (
        f"rm -f {remote_q} {remote_log} {remote_wc}; "
        f"(nc -l -p {nc_port} > {remote_q}; "
        f"wc -c < {remote_q} > {remote_wc}; "
        f"chmod {mode_q} {remote_q}) > {remote_log} 2>&1 & "
        "echo nc_listener=$!"
    )
    output = ["# remote listener", telnet(host, setup, telnet_port, wait, 8.0)]
    time.sleep(0.3)

    with local.open("rb") as fp:
        sent = subprocess.run(
            ["nc", "-w", "5", host, str(nc_port)],
            cwd=str(REPO_ROOT),
            stdin=fp,
            stdout=subprocess.PIPE,
            stderr=subprocess.STDOUT,
            timeout=20.0,
            check=False,
        )
    output.append(f"# local nc rc={sent.returncode} expected_size={size}")
    if sent.stdout:
        output.append(sent.stdout.decode("utf-8", errors="replace"))
    if sent.returncode != 0:
        raise RuntimeError("\n".join(output))

    time.sleep(0.8)
    verify = telnet(
        host,
        f"echo remote_size=$(cat {remote_wc} 2>/dev/null); "
        f"ls -l {remote_q} 2>/dev/null; "
        f"cat {remote_log} 2>/dev/null",
        telnet_port,
        wait,
        8.0,
    )
    output.extend(["# verify", verify])
    if f"remote_size={size}" not in verify:
        raise RuntimeError(
            f"nc upload size mismatch for {remote}: expected {size}; verify output:\n{verify}"
        )

    return "\n".join(output)


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("host")
    parser.add_argument("local_file")
    parser.add_argument("remote_path")
    parser.add_argument("--port", type=int, default=24, help="camera telnet port")
    parser.add_argument("--wait", type=float, default=0.8, help="telnet wait seconds")
    parser.add_argument("--nc-port", type=int, default=10121, help="temporary camera nc listener port")
    parser.add_argument("--mode", default="755", help="remote chmod mode")
    args = parser.parse_args()

    try:
        print(
            upload_nc(
                args.host,
                pathlib.Path(args.local_file),
                args.remote_path,
                args.port,
                args.wait,
                args.nc_port,
                args.mode,
            )
        )
    except Exception as exc:
        print(f"ERROR: {exc}", file=sys.stderr)
        return 1

    return 0


if __name__ == "__main__":
    raise SystemExit(main())
