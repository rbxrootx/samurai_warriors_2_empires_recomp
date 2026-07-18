#!/usr/bin/env python3
"""Patch SW2E G1M vertex streams from an OBJ exported by export_g1m_obj.py.

This is a conservative edit path for Blender experiments. It preserves the
original G1M chunk layout, topology, materials, index buffers, skeleton/joint
data, and all unknown fields. Only existing position/normal/UV float streams
are replaced in-place, and the OBJ must keep the exported vertex order.
"""

from __future__ import annotations

import argparse
import csv
import struct
from dataclasses import dataclass
from pathlib import Path

from export_g1m_obj import (
    SEMANTIC_NORMAL,
    SEMANTIC_POSITION,
    SEMANTIC_UV,
    TYPE_FLOAT_X2,
    TYPE_FLOAT_X3,
    TYPE_FLOAT_X4,
    VertexAttribute,
    VertexAttributeSet,
    VertexBuffer,
    collect_inputs,
    find_attribute,
    output_name,
    parse_g1m,
)


@dataclass
class ObjData:
    positions: list[tuple[float, float, float]]
    uvs: list[tuple[float, float]]
    normals: list[tuple[float, float, float]]


class PatchError(RuntimeError):
    pass


def parse_obj(path: Path) -> ObjData:
    positions: list[tuple[float, float, float]] = []
    uvs: list[tuple[float, float]] = []
    normals: list[tuple[float, float, float]] = []

    with path.open("r", encoding="utf-8", errors="replace") as obj:
        for line_number, line in enumerate(obj, 1):
            line = line.strip()
            if not line or line.startswith("#"):
                continue

            parts = line.split()
            if parts[0] == "v":
                if len(parts) < 4:
                    raise PatchError(f"{path}: malformed v line {line_number}")
                positions.append((float(parts[1]), float(parts[2]), float(parts[3])))
            elif parts[0] == "vt":
                if len(parts) < 3:
                    raise PatchError(f"{path}: malformed vt line {line_number}")
                uvs.append((float(parts[1]), float(parts[2])))
            elif parts[0] == "vn":
                if len(parts) < 4:
                    raise PatchError(f"{path}: malformed vn line {line_number}")
                normals.append((float(parts[1]), float(parts[2]), float(parts[3])))

    return ObjData(positions=positions, uvs=uvs, normals=normals)


def write_f32be(data: bytearray, offset: int, value: float) -> None:
    data[offset : offset + 4] = struct.pack(">f", float(value))


def component_count(data_type: int) -> int:
    if data_type == TYPE_FLOAT_X2:
        return 2
    if data_type == TYPE_FLOAT_X3:
        return 3
    if data_type == TYPE_FLOAT_X4:
        return 4
    return 0


def patch_float_stream(
    data: bytearray,
    vertex_buffer: VertexBuffer,
    attribute: VertexAttribute,
    values: list[tuple[float, ...]],
    start_index: int,
    value_width: int,
) -> None:
    count = component_count(attribute.data_type)
    if count < value_width:
        raise PatchError(
            f"unsupported attribute type 0x{attribute.data_type:X} for {value_width} components"
        )

    for vertex_index in range(vertex_buffer.count):
        value = values[start_index + vertex_index]
        offset = (
            vertex_buffer.data_offset
            + vertex_index * vertex_buffer.stride
            + attribute.stream_offset
        )
        for component_index in range(value_width):
            write_f32be(data, offset + component_index * 4, value[component_index])


def stream_vertex_count(chunks) -> int:
    total = 0
    for chunk in chunks:
        for attr_set in chunk.attribute_sets:
            pos_attr = find_attribute(attr_set, SEMANTIC_POSITION)
            if pos_attr and 0 <= pos_attr.buffer_id < len(chunk.vertex_buffers):
                total += chunk.vertex_buffers[pos_attr.buffer_id].count
    return total


def optional_stream_count(chunks, semantic: int) -> int:
    total = 0
    for chunk in chunks:
        for attr_set in chunk.attribute_sets:
            pos_attr = find_attribute(attr_set, SEMANTIC_POSITION)
            attr = find_attribute(attr_set, semantic)
            if (
                pos_attr
                and attr
                and 0 <= pos_attr.buffer_id < len(chunk.vertex_buffers)
                and 0 <= attr.buffer_id < len(chunk.vertex_buffers)
            ):
                total += chunk.vertex_buffers[pos_attr.buffer_id].count
    return total


def scaled_positions(obj: ObjData, scale: float) -> list[tuple[float, float, float]]:
    if scale == 0:
        raise PatchError("scale must not be zero")
    return [(x / scale, y / scale, z / scale) for x, y, z in obj.positions]


def patched_uvs(obj: ObjData, flip_v: bool) -> list[tuple[float, float]]:
    if not flip_v:
        return obj.uvs
    return [(u, 1.0 - v) for u, v in obj.uvs]


def patch_one(
    input_path: Path,
    obj_path: Path,
    output_path: Path,
    scale: float,
    flip_v: bool,
    patch_positions: bool,
    patch_normals: bool,
    patch_uvs: bool,
) -> dict[str, object]:
    original = bytearray(input_path.read_bytes())
    chunks = parse_g1m(input_path)
    obj = parse_obj(obj_path)

    expected_positions = stream_vertex_count(chunks)
    expected_normals = optional_stream_count(chunks, SEMANTIC_NORMAL)
    expected_uvs = optional_stream_count(chunks, SEMANTIC_UV)

    if patch_positions and len(obj.positions) != expected_positions:
        raise PatchError(
            f"position count mismatch: obj={len(obj.positions)} expected={expected_positions}"
        )
    if patch_normals and expected_normals and len(obj.normals) != expected_normals:
        raise PatchError(f"normal count mismatch: obj={len(obj.normals)} expected={expected_normals}")
    if patch_uvs and expected_uvs and len(obj.uvs) != expected_uvs:
        raise PatchError(f"uv count mismatch: obj={len(obj.uvs)} expected={expected_uvs}")

    positions = scaled_positions(obj, scale)
    normals = obj.normals
    uvs = patched_uvs(obj, flip_v)
    position_cursor = 0
    normal_cursor = 0
    uv_cursor = 0
    patched_vertex_streams = 0
    patched_normal_streams = 0
    patched_uv_streams = 0

    for chunk in chunks:
        for attr_set in chunk.attribute_sets:
            pos_attr = find_attribute(attr_set, SEMANTIC_POSITION)
            if pos_attr is None or not (0 <= pos_attr.buffer_id < len(chunk.vertex_buffers)):
                continue

            pos_buffer = chunk.vertex_buffers[pos_attr.buffer_id]
            if patch_positions:
                patch_float_stream(original, pos_buffer, pos_attr, positions, position_cursor, 3)
                patched_vertex_streams += 1
            position_cursor += pos_buffer.count

            normal_attr = find_attribute(attr_set, SEMANTIC_NORMAL)
            if (
                patch_normals
                and normal_attr is not None
                and 0 <= normal_attr.buffer_id < len(chunk.vertex_buffers)
            ):
                normal_buffer = chunk.vertex_buffers[normal_attr.buffer_id]
                patch_float_stream(original, normal_buffer, normal_attr, normals, normal_cursor, 3)
                patched_normal_streams += 1
                normal_cursor += pos_buffer.count
            elif normal_attr is not None:
                normal_cursor += pos_buffer.count

            uv_attr = find_attribute(attr_set, SEMANTIC_UV)
            if patch_uvs and uv_attr is not None and 0 <= uv_attr.buffer_id < len(chunk.vertex_buffers):
                uv_buffer = chunk.vertex_buffers[uv_attr.buffer_id]
                patch_float_stream(original, uv_buffer, uv_attr, uvs, uv_cursor, 2)
                patched_uv_streams += 1
                uv_cursor += pos_buffer.count
            elif uv_attr is not None:
                uv_cursor += pos_buffer.count

    output_path.parent.mkdir(parents=True, exist_ok=True)
    output_path.write_bytes(original)

    return {
        "input": str(input_path),
        "obj": str(obj_path),
        "output": str(output_path),
        "status": "ok",
        "g1mg_chunks": len(chunks),
        "vertices": expected_positions,
        "normals": expected_normals,
        "uvs": expected_uvs,
        "patched_position_streams": patched_vertex_streams,
        "patched_normal_streams": patched_normal_streams,
        "patched_uv_streams": patched_uv_streams,
        "message": "",
    }


def paired_paths(input_root: Path, obj_root: Path, out_root: Path) -> list[tuple[Path, Path, Path]]:
    if input_root.is_file():
        if obj_root.is_dir():
            obj_path = obj_root / output_name(input_root, input_root)
        else:
            obj_path = obj_root
        if out_root.suffix:
            output_path = out_root
        else:
            output_path = out_root / input_root.name
        return [(input_root, obj_path, output_path)]

    pairs: list[tuple[Path, Path, Path]] = []
    for input_path in collect_inputs(input_root):
        obj_path = obj_root / output_name(input_root, input_path)
        output_path = out_root / input_path.relative_to(input_root)
        pairs.append((input_path, obj_path, output_path))
    return pairs


def main() -> int:
    parser = argparse.ArgumentParser(
        description="Patch SW2E G1M vertex streams from matching exported OBJ files."
    )
    parser.add_argument("input", type=Path, help="Input .g1m file or directory.")
    parser.add_argument("obj", type=Path, help="Matching OBJ file or OBJ directory.")
    parser.add_argument("--out", type=Path, required=True, help="Output .g1m file or directory.")
    parser.add_argument("--scale", type=float, default=1.0, help="Scale used during OBJ export.")
    parser.add_argument("--flip-v", action="store_true", help="OBJ V was flipped during export.")
    parser.add_argument("--no-positions", action="store_true", help="Do not patch positions.")
    parser.add_argument("--no-normals", action="store_true", help="Do not patch normals.")
    parser.add_argument("--no-uvs", action="store_true", help="Do not patch UVs.")
    parser.add_argument(
        "--report",
        type=Path,
        default=None,
        help="CSV report path. Defaults to <out>/patch_manifest.csv for directories.",
    )
    args = parser.parse_args()

    input_root = args.input.resolve()
    obj_root = args.obj.resolve()
    out_root = args.out.resolve()
    report_path = args.report.resolve() if args.report else None
    if report_path is None:
        report_path = (out_root if out_root.suffix == "" else out_root.parent) / "patch_manifest.csv"

    rows: list[dict[str, object]] = []
    for input_path, obj_path, output_path in paired_paths(input_root, obj_root, out_root):
        try:
            if not obj_path.exists():
                raise PatchError(f"missing OBJ: {obj_path}")
            rows.append(
                patch_one(
                    input_path=input_path,
                    obj_path=obj_path,
                    output_path=output_path,
                    scale=args.scale,
                    flip_v=args.flip_v,
                    patch_positions=not args.no_positions,
                    patch_normals=not args.no_normals,
                    patch_uvs=not args.no_uvs,
                )
            )
        except Exception as exc:
            rows.append(
                {
                    "input": str(input_path),
                    "obj": str(obj_path),
                    "output": str(output_path),
                    "status": "error",
                    "g1mg_chunks": 0,
                    "vertices": 0,
                    "normals": 0,
                    "uvs": 0,
                    "patched_position_streams": 0,
                    "patched_normal_streams": 0,
                    "patched_uv_streams": 0,
                    "message": str(exc),
                }
            )

    report_path.parent.mkdir(parents=True, exist_ok=True)
    with report_path.open("w", encoding="utf-8", newline="") as report:
        writer = csv.DictWriter(
            report,
            fieldnames=[
                "input",
                "obj",
                "output",
                "status",
                "g1mg_chunks",
                "vertices",
                "normals",
                "uvs",
                "patched_position_streams",
                "patched_normal_streams",
                "patched_uv_streams",
                "message",
            ],
        )
        writer.writeheader()
        writer.writerows(rows)

    ok = sum(1 for row in rows if row["status"] == "ok")
    errors = len(rows) - ok
    print(f"patched={ok} errors={errors} report={report_path}")
    return 1 if errors else 0


if __name__ == "__main__":
    raise SystemExit(main())
