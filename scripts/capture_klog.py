#!/usr/bin/env python3
"""Capture a PS5 kernel log from klogsrv using only the Python standard library."""

import argparse
import os
import socket
import sys


def port_number(value: str) -> int:
    try:
        port = int(value)
    except ValueError as exc:
        raise argparse.ArgumentTypeError("port must be an integer") from exc
    if not 1 <= port <= 65535:
        raise argparse.ArgumentTypeError("port must be between 1 and 65535")
    return port


def main() -> int:
    parser = argparse.ArgumentParser(
        description="Save the TCP log stream provided by a running PS5 klogsrv."
    )
    parser.add_argument("console_ip", help="IP address or hostname of the PS5")
    parser.add_argument(
        "-p", "--port", type=port_number, default=3232,
        help="klogsrv TCP port (default: 3232)",
    )
    parser.add_argument(
        "-o", "--output", default="klog.txt",
        help="new file to create (default: klog.txt)",
    )
    args = parser.parse_args()

    try:
        output = open(args.output, "xb")
    except FileExistsError:
        print(
            f"error: {args.output} already exists; choose another --output path",
            file=sys.stderr,
        )
        return 2
    except OSError as exc:
        print(f"error: cannot create {args.output}: {exc}", file=sys.stderr)
        return 1

    try:
        connection = socket.create_connection(
            (args.console_ip, args.port), timeout=10
        )
    except OSError as exc:
        try:
            created = os.fstat(output.fileno())
        except OSError:
            created = None
        output.close()
        if created is not None:
            try:
                current = os.stat(args.output, follow_symlinks=False)
                if (
                    current.st_dev == created.st_dev
                    and current.st_ino == created.st_ino
                    and current.st_size == 0
                ):
                    os.unlink(args.output)
            except OSError:
                pass
        print(
            f"error: cannot connect to {args.console_ip}:{args.port}: {exc}",
            file=sys.stderr,
        )
        return 1

    received = 0
    try:
        with connection, output:
            connection.settimeout(None)
            print(
                f"Connected to {args.console_ip}:{args.port}; "
                f"capturing to {args.output}. Press Ctrl+C to stop.",
                file=sys.stderr,
            )
            try:
                while True:
                    chunk = connection.recv(65536)
                    if not chunk:
                        break
                    output.write(chunk)
                    received += len(chunk)
            except KeyboardInterrupt:
                print("Capture stopped.", file=sys.stderr)
    except OSError as exc:
        print(f"error: capture failed: {exc}", file=sys.stderr)
        return 1
    print(f"Saved {received} bytes to {args.output}.", file=sys.stderr)
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
