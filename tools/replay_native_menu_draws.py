#!/usr/bin/env python3
"""Replay captured SW2E menu texture draws into a native PC PNG."""

from __future__ import annotations

import argparse
import json
import math
import struct
import sys
from dataclasses import dataclass
from pathlib import Path
from typing import Any

from PIL import Image


ENDIAN_NONE = 0
ENDIAN_8_IN_16 = 1
ENDIAN_8_IN_32 = 2
ENDIAN_16_IN_32 = 3


@dataclass(frozen=True)
class Vertex:
    x: float
    y: float
    z: float
    w: float
    u: float
    v: float


def read_metadata(sample_root: Path) -> list[dict[str, Any]]:
    metadata_path = sample_root / "samples.jsonl"
    if not metadata_path.is_file():
        raise SystemExit(f"Missing metadata: {metadata_path}")
    rows: list[dict[str, Any]] = []
    with metadata_path.open("r", encoding="utf-8") as handle:
        for line_number, line in enumerate(handle, 1):
            line = line.strip()
            if not line:
                continue
            try:
                rows.append(json.loads(line))
            except json.JSONDecodeError as exc:
                raise SystemExit(f"{metadata_path}:{line_number}: invalid JSON: {exc}") from exc
    return rows


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


def decode_rgb565(value: int) -> tuple[int, int, int]:
    r = (value >> 11) & 0x1F
    g = (value >> 5) & 0x3F
    b = value & 0x1F
    return (r * 255 + 15) // 31, (g * 255 + 31) // 63, (b * 255 + 15) // 31


def decode_bc3_block(block: bytes) -> list[tuple[int, int, int, int]]:
    a0, a1 = block[0], block[1]
    alpha_bits = int.from_bytes(block[2:8], "little")
    alphas = [a0, a1]
    if a0 > a1:
        alphas.extend(((7 - i) * a0 + i * a1 + 3) // 7 for i in range(1, 7))
    else:
        alphas.extend(((5 - i) * a0 + i * a1 + 2) // 5 for i in range(1, 5))
        alphas.extend([0, 255])

    c0, c1, color_bits = struct.unpack_from("<HHI", block, 8)
    rgb0 = decode_rgb565(c0)
    rgb1 = decode_rgb565(c1)
    colors = [
        rgb0,
        rgb1,
        tuple((2 * rgb0[i] + rgb1[i]) // 3 for i in range(3)),
        tuple((rgb0[i] + 2 * rgb1[i]) // 3 for i in range(3)),
    ]

    pixels: list[tuple[int, int, int, int]] = []
    for i in range(16):
        alpha_index = (alpha_bits >> (3 * i)) & 0x7
        color_index = (color_bits >> (2 * i)) & 0x3
        r, g, b = colors[color_index]
        pixels.append((r, g, b, alphas[alpha_index]))
    return pixels


def decode_bc3_texture(sample_root: Path, row: dict[str, Any]) -> Image.Image:
    texture = row["texture"]
    width = int(texture["width"])
    height = int(texture["height"])
    width_blocks = int(texture["width_blocks"])
    height_blocks = int(texture["height_blocks"])
    row_pitch = int(texture["row_pitch_bytes"])
    endian = int(texture.get("endian", 0))
    data = (sample_root / row["file"]).read_bytes()

    image = Image.new("RGBA", (width, height))
    pixels = image.load()
    for block_y in range(height_blocks):
        for block_x in range(width_blocks):
            offset = block_y * row_pitch + block_x * 16
            if offset + 16 > len(data):
                raise ValueError(f"{row['file']}: BC3 block exceeds sample size")
            decoded = decode_bc3_block(swap_block(data[offset : offset + 16], endian))
            for py in range(4):
                y = block_y * 4 + py
                if y >= height:
                    continue
                for px in range(4):
                    x = block_x * 4 + px
                    if x >= width:
                        continue
                    pixels[x, y] = decoded[py * 4 + px]
    return image


def vertex_float_format(endian: int) -> str:
    if endian == ENDIAN_8_IN_32:
        return ">6f"
    if endian == ENDIAN_NONE:
        return "<6f"
    raise ValueError(f"unsupported vertex endian {endian}")


def decode_vertices(sample_root: Path, row: dict[str, Any]) -> list[Vertex]:
    vertex = row["vertex"]
    stride = int(vertex["stride_words"]) * 4
    if stride != 24:
        raise ValueError(f"{row['file']}: expected 24-byte menu vertex stride, got {stride}")
    fmt = vertex_float_format(int(vertex.get("endian", 0)))
    data = (sample_root / row["file"]).read_bytes()
    vertices: list[Vertex] = []
    for offset in range(0, len(data), stride):
        values = struct.unpack(fmt, data[offset : offset + stride])
        vertices.append(Vertex(*values))
    return vertices


def draw_rect(canvas: Image.Image, texture: Image.Image, vertices: list[Vertex]) -> None:
    xs = [v.x for v in vertices]
    ys = [v.y for v in vertices]
    us = [v.u for v in vertices]
    vs = [v.v for v in vertices]

    left = max(0, int(math.floor(min(xs))))
    top = max(0, int(math.floor(min(ys))))
    right = min(canvas.width, int(math.ceil(max(xs))))
    bottom = min(canvas.height, int(math.ceil(max(ys))))
    if right <= left or bottom <= top:
        return

    crop_left = max(0, min(texture.width - 1, int(math.floor(min(us) * texture.width))))
    crop_top = max(0, min(texture.height - 1, int(math.floor(min(vs) * texture.height))))
    crop_right = max(crop_left + 1, min(texture.width, int(math.ceil(max(us) * texture.width))))
    crop_bottom = max(crop_top + 1, min(texture.height, int(math.ceil(max(vs) * texture.height))))
    crop = texture.crop((crop_left, crop_top, crop_right, crop_bottom))
    resample = getattr(Image, "Resampling", Image).BILINEAR
    resized = crop.resize((right - left, bottom - top), resample)
    canvas.alpha_composite(resized, (left, top))


def draw_order_key(row: dict[str, Any]) -> tuple[int, int]:
    return int(row["frame"]), int(row["draw"])


def replay(sample_root: Path, width: int, height: int, max_frame: int | None) -> tuple[Image.Image, list[str]]:
    rows = read_metadata(sample_root)
    by_draw: dict[tuple[int, int], dict[str, dict[str, Any]]] = {}
    for row in rows:
        key = int(row["frame"]), int(row["draw"])
        by_draw.setdefault(key, {})[row["kind"]] = row

    pairs = []
    for key, group in by_draw.items():
        vertex = group.get("vertex")
        texture = group.get("texture")
        if not vertex or not texture:
            continue
        if max_frame is not None and int(texture["frame"]) > max_frame:
            continue
        texture_info = texture.get("texture", {})
        if texture.get("dump_full") is not True or texture_info.get("pc_format") != "BC3_UNORM":
            continue
        if int(texture_info.get("tiled", 1)) != 0:
            continue
        if int(vertex.get("primitive", 0)) != 4 or int(vertex.get("index_count", 0)) != 6:
            continue
        pairs.append((key, vertex, texture))

    canvas = Image.new("RGBA", (width, height), (0, 0, 0, 255))
    summaries: list[str] = []
    for key, vertex_row, texture_row in sorted(pairs, key=lambda item: item[0]):
        vertices = decode_vertices(sample_root, vertex_row)
        texture = decode_bc3_texture(sample_root, texture_row)
        draw_rect(canvas, texture, vertices)
        xs = [v.x for v in vertices]
        ys = [v.y for v in vertices]
        summaries.append(
            f"frame={key[0]} draw={key[1]} texture={texture_row['texture']['width']}x"
            f"{texture_row['texture']['height']} rect=({min(xs):.0f},{min(ys):.0f})-"
            f"({max(xs):.0f},{max(ys):.0f}) file={texture_row['file']}"
        )
    return canvas, summaries


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("sample_root", type=Path, help="Folder containing samples.jsonl and full texture dumps")
    parser.add_argument("--out", type=Path, default=None, help="Output PNG path")
    parser.add_argument("--width", type=int, default=1280, help="Output canvas width")
    parser.add_argument("--height", type=int, default=720, help="Output canvas height")
    parser.add_argument("--max-frame", type=int, default=103, help="Only replay draws up to this frame")
    args = parser.parse_args()

    output_path = args.out or (args.sample_root / "native_menu_replay.png")
    image, summaries = replay(args.sample_root, args.width, args.height, args.max_frame)
    output_path.parent.mkdir(parents=True, exist_ok=True)
    image.save(output_path)
    for line in summaries:
        print(line)
    print(f"Wrote {output_path}")
    return 0 if summaries else 1


if __name__ == "__main__":
    raise SystemExit(main())
