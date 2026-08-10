#!/usr/bin/env python3
"""Require a normal process exit with an exact status within a time limit."""

import argparse
import os
import signal
import subprocess
import sys


def stop_process_group(process: subprocess.Popen[bytes]) -> None:
    """Terminate a timed-out command and all descendants in its process group."""
    try:
        os.killpg(process.pid, signal.SIGTERM)
    except ProcessLookupError:
        return
    try:
        process.wait(timeout=2)
        return
    except subprocess.TimeoutExpired:
        pass
    try:
        os.killpg(process.pid, signal.SIGKILL)
    except ProcessLookupError:
        return
    process.wait()


def main() -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument("--expected", required=True, type=int)
    parser.add_argument("--timeout", required=True, type=float)
    parser.add_argument("command", nargs=argparse.REMAINDER)
    args = parser.parse_args()

    command = args.command
    if command[:1] == ["--"]:
        command = command[1:]
    if not command:
        parser.error("a command is required after --")

    process = subprocess.Popen(command, start_new_session=True)
    try:
        status = process.wait(timeout=args.timeout)
    except subprocess.TimeoutExpired:
        stop_process_group(process)
        print(
            f"expected normal exit {args.expected}, but command timed out "
            f"after {args.timeout:g}s",
            file=sys.stderr,
        )
        return 1

    if status < 0:
        signum = -status
        print(
            f"expected normal exit {args.expected}, but command was killed by "
            f"signal {signum} ({signal.strsignal(signum)})",
            file=sys.stderr,
        )
        return 1
    if status != args.expected:
        print(
            f"expected normal exit {args.expected}, got {status}",
            file=sys.stderr,
        )
        return 1
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
