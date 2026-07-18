#!/usr/bin/env python3
"""Split SW2E scenario blob entry 65 into offset/size sections."""

from __future__ import annotations

import argparse
import csv
import struct
from pathlib import Path


def ascii_preview(data: bytes, length: int = 32) -> str:
    out = []
    for value in data[:length]:
        out.append(chr(value) if 32 <= value <= 126 else ".")
    return "".join(out)


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("input", type=Path, help="Entry 65 scenario blob")
    parser.add_argument("--out-dir", type=Path, required=True, help="Directory for split sections")
    args = parser.parse_args()

    data = args.input.read_bytes()
    if len(data) < 4:
        raise SystemExit(f"{args.input}: too small for a section count")

    count = struct.unpack_from(">I", data, 0)[0]
    header_size = 4 + count * 8
    if count == 0 or header_size > len(data):
        raise SystemExit(f"{args.input}: invalid section count {count}")

    sections: list[tuple[int, int]] = []
    for index in range(count):
        offset, size = struct.unpack_from(">II", data, 4 + index * 8)
        if offset > len(data) or size > len(data) or offset + size > len(data):
            raise SystemExit(
                f"{args.input}: section {index} offset/size out of range: 0x{offset:X}/0x{size:X}"
            )
        sections.append((offset, size))

    args.out_dir.mkdir(parents=True, exist_ok=True)
    manifest_path = args.out_dir / "sections.csv"
    with manifest_path.open("w", newline="", encoding="utf-8") as manifest:
        writer = csv.writer(manifest)
        writer.writerow(["section", "offset", "size", "path", "preview"])
        for index, (offset, size) in enumerate(sections):
            section = data[offset : offset + size]
            out_path = args.out_dir / f"section_{index:02d}_off_{offset:08X}_size_{size:08X}.bin"
            out_path.write_bytes(section)
            writer.writerow(
                [
                    index,
                    f"0x{offset:08X}",
                    f"0x{size:08X}",
                    out_path.name,
                    ascii_preview(section),
                ]
            )

    print(f"sections={count} wrote={args.out_dir}")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
