#!/usr/bin/env python3
"""Scan SW2E scenario-control table entry 68 into a conservative CSV."""

from __future__ import annotations

import argparse
import csv
import struct
from pathlib import Path


ROW_SIZE = 100


def byte_list(values: bytes, *, skip: int | None = None) -> str:
    items = [value for value in values if skip is None or value != skip]
    return " ".join(f"{value:02X}" for value in items)


def u16be(row: bytes, offset: int) -> int:
    return struct.unpack_from(">H", row, offset)[0]


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("input", type=Path, help="Entry 68 binary table")
    parser.add_argument("--csv", type=Path, required=True, help="CSV output path")
    args = parser.parse_args()

    data = args.input.read_bytes()
    if len(data) < 4:
        raise SystemExit(f"{args.input}: too small for a count header")

    count = struct.unpack_from(">I", data, 0)[0]
    expected_size = 4 + count * ROW_SIZE
    if len(data) != expected_size:
        raise SystemExit(
            f"{args.input}: expected {expected_size} bytes for {count} rows, got {len(data)}"
        )

    args.csv.parent.mkdir(parents=True, exist_ok=True)
    with args.csv.open("w", newline="", encoding="utf-8") as file:
        writer = csv.writer(file)
        writer.writerow(
            [
                "row",
                "file_offset",
                "u16_00",
                "u16_02",
                "b04",
                "b05",
                "b06",
                "b07",
                "b08",
                "b09",
                "flags_10_39",
                "b40",
                "b41",
                "b42",
                "b43",
                "list_45_59_non_ff",
                "slots_60_65",
                "tail_66_99",
                "raw_hex",
            ]
        )

        for row_index in range(count):
            offset = 4 + row_index * ROW_SIZE
            row = data[offset : offset + ROW_SIZE]
            writer.writerow(
                [
                    row_index,
                    f"0x{offset:08X}",
                    u16be(row, 0),
                    u16be(row, 2),
                    row[4],
                    row[5],
                    row[6],
                    row[7],
                    row[8],
                    row[9],
                    byte_list(row[10:40]),
                    row[40],
                    row[41],
                    row[42],
                    row[43],
                    byte_list(row[45:60], skip=0xFF),
                    byte_list(row[60:66]),
                    byte_list(row[66:100]),
                    row.hex(" "),
                ]
            )

    print(f"rows={count} row_size={ROW_SIZE} wrote={args.csv}")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
