#!/usr/bin/env python3
"""Export full native-render texture samples to DDS files."""

from __future__ import annotations

import argparse
import json
import struct
import sys
from pathlib import Path
from typing import Any


DDSD_CAPS = 0x00000001
DDSD_HEIGHT = 0x00000002
DDSD_WIDTH = 0x00000004
DDSD_PIXELFORMAT = 0x00001000
DDSD_LINEARSIZE = 0x00080000
DDPF_FOURCC = 0x00000004
DDSCAPS_TEXTURE = 0x00001000

ENDIAN_NONE = 0
ENDIAN_8_IN_16 = 1
ENDIAN_8_IN_32 = 2
ENDIAN_16_IN_32 = 3


def read_samples(path: Path) -> list[dict[str, Any]]:
    rows: list[dict[str, Any]] = []
    with path.open("r", encoding="utf-8") as handle:
        for line_number, line in enumerate(handle, 1):
            line = line.strip()
            if not line:
                continue
            try:
                rows.append(json.loads(line))
            except json.JSONDecodeError as exc:
                raise SystemExit(f"{path}:{line_number}: invalid JSON: {exc}") from exc
    return rows


def fourcc(value: bytes) -> int:
    return struct.unpack("<I", value)[0]


def build_dds_header(width: int, height: int, linear_size: int, fourcc_name: bytes) -> bytes:
    header = bytearray()
    header += b"DDS "
    header += struct.pack("<I", 124)
    header += struct.pack("<I", DDSD_CAPS | DDSD_HEIGHT | DDSD_WIDTH | DDSD_PIXELFORMAT | DDSD_LINEARSIZE)
    header += struct.pack("<I", height)
    header += struct.pack("<I", width)
    header += struct.pack("<I", linear_size)
    header += struct.pack("<I", 0)
    header += struct.pack("<I", 0)
    header += bytes(11 * 4)
    header += struct.pack("<I", 32)
    header += struct.pack("<I", DDPF_FOURCC)
    header += struct.pack("<I", fourcc(fourcc_name))
    header += struct.pack("<I", 0)
    header += struct.pack("<I", 0)
    header += struct.pack("<I", 0)
    header += struct.pack("<I", 0)
    header += struct.pack("<I", 0)
    header += struct.pack("<I", DDSCAPS_TEXTURE)
    header += struct.pack("<I", 0)
    header += struct.pack("<I", 0)
    header += struct.pack("<I", 0)
    header += struct.pack("<I", 0)
    if len(header) != 128:
        raise AssertionError(f"DDS header size mismatch: {len(header)}")
    return bytes(header)


def swap_block(block: bytes, endian: int) -> bytes:
    if endian == ENDIAN_NONE:
        return block
    if endian == ENDIAN_8_IN_16:
        return b"".join(block[i : i + 2][::-1] for i in range(0, len(block), 2))
    if endian == ENDIAN_8_IN_32:
        return b"".join(block[i : i + 4][::-1] for i in range(0, len(block), 4))
    if endian == ENDIAN_16_IN_32:
        return b"".join(block[i + 2 : i + 4] + block[i : i + 2] for i in range(0, len(block), 4))
    raise ValueError(f"unsupported endian {endian}")


def export_bc3(row: dict[str, Any], sample_root: Path, output_root: Path, raw_endian: bool) -> Path:
    texture = row.get("texture", {})
    width = int(texture["width"])
    height = int(texture["height"])
    width_blocks = int(texture["width_blocks"])
    height_blocks = int(texture["height_blocks"])
    row_pitch = int(texture["row_pitch_bytes"])
    endian = int(texture.get("endian", 0))
    tight_row_bytes = width_blocks * 16

    input_path = sample_root / row["file"]
    data = input_path.read_bytes()
    expected = int(row.get("expected_full_size", 0))
    if expected and len(data) < expected:
        raise ValueError(f"{input_path.name}: only {len(data)} bytes, expected {expected}")

    output_data = bytearray()
    for y in range(height_blocks):
        row_offset = y * row_pitch
        row_end = row_offset + tight_row_bytes
        if row_end > len(data):
            raise ValueError(f"{input_path.name}: row {y} exceeds sample size")
        source_row = data[row_offset:row_end]
        if raw_endian:
            output_data += source_row
        else:
            for offset in range(0, len(source_row), 16):
                output_data += swap_block(source_row[offset : offset + 16], endian)

    output_root.mkdir(parents=True, exist_ok=True)
    output_path = output_root / f"{input_path.stem}_{width}x{height}_bc3.dds"
    output_path.write_bytes(build_dds_header(width, height, len(output_data), b"DXT5") + output_data)
    return output_path


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("sample_root", type=Path, help="Folder containing samples.jsonl and .bin dumps")
    parser.add_argument("--out", type=Path, default=None, help="Output folder for DDS files")
    parser.add_argument(
        "--raw-endian",
        action="store_true",
        help="Do not apply the Xenos endian swap before writing DDS blocks",
    )
    args = parser.parse_args()

    sample_root = args.sample_root
    metadata_path = sample_root / "samples.jsonl"
    if not metadata_path.is_file():
        print(f"Missing metadata: {metadata_path}", file=sys.stderr)
        return 2

    output_root = args.out or (sample_root / ("dds_raw" if args.raw_endian else "dds"))
    rows = read_samples(metadata_path)
    candidates = [
        row
        for row in rows
        if row.get("kind") == "texture"
        and row.get("dump_full") is True
        and row.get("texture", {}).get("pc_format") == "BC3_UNORM"
        and int(row.get("texture", {}).get("tiled", 1)) == 0
    ]

    exported = 0
    for row in candidates:
        try:
            output_path = export_bc3(row, sample_root, output_root, args.raw_endian)
        except Exception as exc:  # noqa: BLE001 - keep batch export going.
            print(f"skip {row.get('file', '<unknown>')}: {exc}", file=sys.stderr)
            continue
        exported += 1
        print(output_path)

    print(f"Exported {exported} DDS files from {metadata_path}")
    return 0 if exported else 1


if __name__ == "__main__":
    raise SystemExit(main())
