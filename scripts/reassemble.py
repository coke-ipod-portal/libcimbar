#!/usr/bin/env python3
"""
reassemble.py -- Reassemble cfc_send_chunked part files into a single file.

Usage:
    python3 reassemble.py <original_filename>
    python3 reassemble.py <original_filename> --parts-dir /path/to/parts
    python3 reassemble.py <original_filename> --output /path/to/output

Example:
    python3 reassemble.py myvideo.mp4
    # Looks for myvideo.mp4.part0, myvideo.mp4.part1, ... in the current
    # directory and writes myvideo.mp4.

This script is safe to run on Android via Termux:
    termux-setup-storage  # if needed
    cd ~/storage/shared/cfc_downloads
    python3 reassemble.py myvideo.mp4
"""

import argparse
import os
import sys

BUFFER_SIZE = 4 * 1024 * 1024  # 4 MB copy buffer


def find_parts(basename: str, parts_dir: str) -> list[str]:
    """Return sorted list of existing partN files."""
    parts = []
    idx = 0
    while True:
        name = os.path.join(parts_dir, f"{basename}.part{idx}")
        if os.path.exists(name):
            parts.append(name)
            idx += 1
        else:
            break
    return parts


def reassemble(original: str, parts_dir: str, output: str) -> int:
    basename = os.path.basename(original)
    parts = find_parts(basename, parts_dir)

    if not parts:
        print(f"[reassemble] ERROR: No part files found for '{basename}' in '{parts_dir}'",
              file=sys.stderr)
        print(f"  Expected: {os.path.join(parts_dir, basename + '.part0')}", file=sys.stderr)
        return 1

    total_parts = len(parts)
    print(f"[reassemble] Found {total_parts} part(s) for '{basename}'")
    for p in parts:
        size_mb = os.path.getsize(p) / (1024 * 1024)
        print(f"  {os.path.basename(p)}  ({size_mb:.1f} MB)")

    if os.path.exists(output):
        overwrite = input(f"[reassemble] '{output}' already exists. Overwrite? [y/N]: ")
        if overwrite.strip().lower() != 'y':
            print("[reassemble] Aborted.")
            return 0

    total_bytes = 0
    try:
        with open(output, 'wb') as out_f:
            for i, part_path in enumerate(parts):
                part_size = os.path.getsize(part_path)
                print(f"[reassemble] Writing part {i+1}/{total_parts}: "
                      f"{os.path.basename(part_path)} ({part_size} bytes) ...",
                      end='', flush=True)
                with open(part_path, 'rb') as in_f:
                    while True:
                        chunk = in_f.read(BUFFER_SIZE)
                        if not chunk:
                            break
                        out_f.write(chunk)
                        total_bytes += len(chunk)
                print(" OK")
    except OSError as e:
        print(f"\n[reassemble] ERROR during write: {e}", file=sys.stderr)
        return 1

    print(f"[reassemble] Done. Output: '{output}'  ({total_bytes:,} bytes total)")

    # Offer to remove part files
    remove = input("[reassemble] Remove part files? [y/N]: ")
    if remove.strip().lower() == 'y':
        for p in parts:
            try:
                os.remove(p)
                print(f"  Removed {os.path.basename(p)}")
            except OSError as e:
                print(f"  WARNING: could not remove {p}: {e}", file=sys.stderr)

    return 0


def main() -> int:
    parser = argparse.ArgumentParser(
        description="Reassemble cfc_send_chunked part files into a single file.")
    parser.add_argument("filename",
        help="Original filename (e.g. myvideo.mp4). Parts are named <filename>.partN.")
    parser.add_argument("--parts-dir", default=".",
        help="Directory containing part files (default: current directory).")
    parser.add_argument("--output", default="",
        help="Output file path (default: <parts-dir>/<filename>).")
    args = parser.parse_args()

    parts_dir = os.path.abspath(args.parts_dir)
    basename  = os.path.basename(args.filename)
    output    = args.output if args.output else os.path.join(parts_dir, basename)

    return reassemble(args.filename, parts_dir, output)


if __name__ == "__main__":
    sys.exit(main())
