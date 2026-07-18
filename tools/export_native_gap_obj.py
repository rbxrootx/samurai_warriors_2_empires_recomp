"""Export sampled native-render gap draws to simple OBJ previews."""

from __future__ import annotations

import argparse
import csv
import itertools
import json
import math
import struct
from pathlib import Path
from typing import Any


FLOAT_COMPONENTS = {
    36: 1,  # k_32_FLOAT
    37: 2,  # k_32_32_FLOAT
    57: 3,  # k_32_32_32_FLOAT
    38: 4,  # k_32_32_32_32_FLOAT
}


def read_rows(sample_root: Path) -> list[dict[str, Any]]:
    metadata_path = sample_root / "samples.jsonl"
    if not metadata_path.exists():
        raise FileNotFoundError(metadata_path)
    rows: list[dict[str, Any]] = []
    with metadata_path.open("r", encoding="utf-8") as handle:
        for line in handle:
            line = line.strip()
            if line:
                rows.append(json.loads(line))
    return rows


def read_f32(data: bytes, offset: int, endian: int) -> float:
    if offset < 0 or offset + 4 > len(data):
        return 0.0
    fmt = ">f" if endian == 2 else "<f"
    value = struct.unpack_from(fmt, data, offset)[0]
    return value if math.isfinite(value) else 0.0


def read_index(data: bytes, offset: int, element_size: int, endian: int) -> int | None:
    if offset < 0 or offset + element_size > len(data):
        return None
    if element_size == 2:
        fmt = ">H" if endian == 1 else "<H"
        return struct.unpack_from(fmt, data, offset)[0]
    if element_size == 4:
        fmt = ">I" if endian == 1 else "<I"
        return struct.unpack_from(fmt, data, offset)[0] & 0x00FFFFFF
    return None


def float_component_count(attribute: dict[str, Any]) -> int:
    return FLOAT_COMPONENTS.get(int(attribute.get("data_format", -1)), 0)


def choose_position_attribute(attributes: list[dict[str, Any]]) -> dict[str, Any] | None:
    for attribute in attributes:
        if int(attribute.get("offset_words", -1)) == 0 and float_component_count(attribute) >= 3:
            return attribute
    for attribute in attributes:
        if float_component_count(attribute) >= 3:
            return attribute
    for attribute in attributes:
        if float_component_count(attribute) >= 1:
            return attribute
    return None


def choose_uv_attribute(attributes: list[dict[str, Any]]) -> dict[str, Any] | None:
    for attribute in attributes:
        if float_component_count(attribute) == 2:
            return attribute
    return None


def decode_attribute(
    data: bytes, vertex_offset: int, stride_bytes: int, endian: int, attribute: dict[str, Any]
) -> list[float]:
    component_count = float_component_count(attribute)
    if component_count == 0:
        return []
    offset_words = int(attribute.get("offset_words", -1))
    if offset_words < 0:
        return []
    byte_offset = vertex_offset + offset_words * 4
    if byte_offset + component_count * 4 > vertex_offset + stride_bytes:
        return []
    return [read_f32(data, byte_offset + component * 4, endian) for component in range(component_count)]


def expand_triangle_strip(indices: list[int]) -> list[tuple[int, int, int]]:
    faces: list[tuple[int, int, int]] = []
    for i in range(len(indices) - 2):
        a, b, c = indices[i], indices[i + 1], indices[i + 2]
        if a == b or a == c or b == c:
            continue
        faces.append((a, b, c) if (i & 1) == 0 else (b, a, c))
    return faces


def expand_triangle_list(indices: list[int]) -> list[tuple[int, int, int]]:
    faces: list[tuple[int, int, int]] = []
    for i in range(0, len(indices) - 2, 3):
        a, b, c = indices[i], indices[i + 1], indices[i + 2]
        if a == b or a == c or b == c:
            continue
        faces.append((a, b, c))
    return faces


def decode_indices(index_row: dict[str, Any], sample_root: Path) -> list[int]:
    index_info = index_row.get("index") or {}
    data = (sample_root / index_row["file"]).read_bytes()
    element_size = int(index_info.get("element_size", 2))
    endian = int(index_info.get("endian", 0))
    index_count = int(index_row.get("index_count", 0))
    indices: list[int] = []
    for i in range(index_count):
        index = read_index(data, i * element_size, element_size, endian)
        if index is None:
            break
        indices.append(index)
    return indices


def decode_vertices(vertex_row: dict[str, Any], sample_root: Path) -> tuple[list[tuple[float, float, float]], list[tuple[float, float]]]:
    vertex_info = vertex_row.get("vertex") or {}
    data = (sample_root / vertex_row["file"]).read_bytes()
    stride_words = int(vertex_info.get("stride_words", 0))
    stride_bytes = stride_words * 4
    if stride_bytes <= 0:
        return [], []
    endian = int(vertex_info.get("endian", 0))
    attributes = list(vertex_info.get("attributes") or [])
    position_attribute = choose_position_attribute(attributes)
    uv_attribute = choose_uv_attribute(attributes)
    if position_attribute is None:
        return [], []

    vertex_count = len(data) // stride_bytes
    if not bool(vertex_row.get("indexed")):
        submitted_vertex_count = int(vertex_row.get("index_count", 0))
        if submitted_vertex_count > 0:
            vertex_count = min(vertex_count, submitted_vertex_count)
    positions: list[tuple[float, float, float]] = []
    uvs: list[tuple[float, float]] = []
    for vertex_index in range(vertex_count):
        vertex_offset = vertex_index * stride_bytes
        position = decode_attribute(data, vertex_offset, stride_bytes, endian, position_attribute)
        while len(position) < 3:
            position.append(0.0)
        positions.append((position[0], position[1], position[2]))

        if uv_attribute is not None:
            uv = decode_attribute(data, vertex_offset, stride_bytes, endian, uv_attribute)
            if len(uv) >= 2:
                uvs.append((uv[0], 1.0 - uv[1]))
            else:
                uvs.append((0.0, 0.0))

    return positions, uvs


def position_bounds(
    positions: list[tuple[float, float, float]]
) -> tuple[tuple[float, float, float], tuple[float, float, float]]:
    mins = [min(position[axis] for position in positions) for axis in range(3)]
    maxs = [max(position[axis] for position in positions) for axis in range(3)]
    return (mins[0], mins[1], mins[2]), (maxs[0], maxs[1], maxs[2])


def compact_float(value: Any) -> str:
    if not isinstance(value, (int, float)) or not math.isfinite(float(value)):
        return "null"
    return f"{float(value):.9g}"


def constant_indices(row: dict[str, Any], key: str) -> str:
    constants = row.get(key) or []
    return ",".join(str(int(constant.get("index", 0))) for constant in constants)


def constants_for_comment(row: dict[str, Any], key: str, limit: int = 4) -> str:
    constants = row.get(key) or []
    parts: list[str] = []
    for constant in constants[:limit]:
        values = ",".join(compact_float(value) for value in (constant.get("values") or [])[:4])
        parts.append(f"c{int(constant.get('index', 0))}=({values})")
    return " ".join(parts)


def finite_constant_rows(row: dict[str, Any]) -> list[tuple[int, tuple[float, float, float, float]]]:
    rows: list[tuple[int, tuple[float, float, float, float]]] = []
    for constant in row.get("vertex_float_constant_values") or []:
        values = constant.get("values") or []
        if len(values) < 4:
            continue
        try:
            vec = tuple(float(values[i]) for i in range(4))
        except (TypeError, ValueError):
            continue
        if all(math.isfinite(value) for value in vec):
            rows.append((int(constant.get("index", 0)), vec))
    return rows


def transform_position(
    position: tuple[float, float, float],
    rows: list[tuple[float, float, float, float]],
    column_major: bool,
) -> tuple[float, float, float, float]:
    source = (position[0], position[1], position[2], 1.0)
    if column_major:
        return tuple(sum(rows[col][component] * source[col] for col in range(4)) for component in range(4))  # type: ignore[return-value]
    return tuple(sum(rows[row][component] * source[component] for component in range(4)) for row in range(4))  # type: ignore[return-value]


def analyze_projection_candidate(
    positions: list[tuple[float, float, float]],
    constant_rows: list[tuple[int, tuple[float, float, float, float]]],
) -> dict[str, Any]:
    if len(positions) < 3 or len(constant_rows) < 4:
        return {}

    stride = max(1, len(positions) // 256)
    sampled_positions = positions[::stride][:256]
    best: dict[str, Any] = {}
    best_score = -1.0

    for combo in itertools.combinations(range(len(constant_rows)), 4):
        rows = [constant_rows[index][1] for index in combo]
        indices = [constant_rows[index][0] for index in combo]
        for column_major in (False, True):
            ndc_points: list[tuple[float, float, float]] = []
            inside_count = 0
            valid_count = 0
            for position in sampled_positions:
                clip = transform_position(position, rows, column_major)
                if not all(math.isfinite(component) for component in clip) or abs(clip[3]) < 1e-6:
                    continue
                ndc = (clip[0] / clip[3], clip[1] / clip[3], clip[2] / clip[3])
                if not all(math.isfinite(component) for component in ndc):
                    continue
                valid_count += 1
                if max(abs(ndc[0]), abs(ndc[1]), abs(ndc[2])) < 1000.0:
                    ndc_points.append(ndc)
                    if abs(ndc[0]) <= 2.0 and abs(ndc[1]) <= 2.0 and -10.0 <= ndc[2] <= 10.0:
                        inside_count += 1

            if not ndc_points:
                continue

            mins = [min(point[axis] for point in ndc_points) for axis in range(3)]
            maxs = [max(point[axis] for point in ndc_points) for axis in range(3)]
            extent = sum(maxs[axis] - mins[axis] for axis in range(3))
            inside_ratio = inside_count / max(1, valid_count)
            finite_ratio = valid_count / len(sampled_positions)
            # Prefer candidates that keep samples finite and near clip space without collapsing to
            # one point. This is a hint for renderer research, not a correctness proof.
            score = finite_ratio + inside_ratio + min(extent, 10.0) * 0.01
            if score > best_score:
                best_score = score
                best = {
                    "indices": ",".join(f"c{index}" for index in indices),
                    "orientation": "column_major" if column_major else "row_major",
                    "finite_ratio": finite_ratio,
                    "inside_ratio": inside_ratio,
                    "ndc_min": (mins[0], mins[1], mins[2]),
                    "ndc_max": (maxs[0], maxs[1], maxs[2]),
                    "score": score,
                }

    return best


def write_obj(
    output_path: Path,
    vertex_row: dict[str, Any],
    positions: list[tuple[float, float, float]],
    uvs: list[tuple[float, float]],
    faces: list[tuple[int, int, int]],
) -> None:
    output_path.parent.mkdir(parents=True, exist_ok=True)
    with output_path.open("w", encoding="utf-8", newline="\n") as handle:
        handle.write("# SW2E native gap sample OBJ\n")
        handle.write(
            f"# frame={vertex_row['frame']} draw={vertex_row['draw']} "
            f"support={vertex_row.get('native_replay_support')} primitive={vertex_row.get('primitive')} "
            f"indexed={vertex_row.get('indexed')} index_count={vertex_row.get('index_count')}\n"
        )
        handle.write(
            f"# VS={vertex_row.get('vertex_shader')} PS={vertex_row.get('pixel_shader')} "
            f"sample={vertex_row.get('file')}\n"
        )
        vertex_constants = constants_for_comment(vertex_row, "vertex_float_constant_values")
        pixel_constants = constants_for_comment(vertex_row, "pixel_float_constant_values")
        if vertex_constants:
            handle.write(f"# vertex_constants {vertex_constants}\n")
        if pixel_constants:
            handle.write(f"# pixel_constants {pixel_constants}\n")
        for x, y, z in positions:
            handle.write(f"v {x:.9g} {y:.9g} {z:.9g}\n")
        has_uvs = len(uvs) == len(positions)
        if has_uvs:
            for u, v in uvs:
                handle.write(f"vt {u:.9g} {v:.9g}\n")
        for a, b, c in faces:
            if has_uvs:
                handle.write(f"f {a + 1}/{a + 1} {b + 1}/{b + 1} {c + 1}/{c + 1}\n")
            else:
                handle.write(f"f {a + 1} {b + 1} {c + 1}\n")


def export_draws(
    sample_root: Path,
    output_root: Path,
    support: str,
    max_draws: int,
    shader: str | None,
) -> list[dict[str, Any]]:
    rows = read_rows(sample_root)
    grouped: dict[tuple[int, int], dict[str, list[dict[str, Any]]]] = {}
    for row in rows:
        if row.get("native_replay_support") != support:
            continue
        if shader and row.get("vertex_shader", "").lower() != shader.lower():
            continue
        key = (int(row["frame"]), int(row["draw"]))
        grouped.setdefault(key, {}).setdefault(row["kind"], []).append(row)

    exported: list[dict[str, Any]] = []
    for (frame, draw), group in sorted(grouped.items()):
        vertex_rows = group.get("vertex") or []
        if not vertex_rows:
            continue
        vertex_row = vertex_rows[0]
        positions, uvs = decode_vertices(vertex_row, sample_root)
        if len(positions) < 3:
            continue

        index_rows = group.get("index") or []
        if index_rows:
            indices = decode_indices(index_rows[0], sample_root)
        else:
            indices = list(
                range(min(len(positions), int(vertex_row.get("index_count", len(positions)))))
            )

        primitive = int(vertex_row.get("primitive", 0))
        if primitive == 6:
            faces = expand_triangle_strip(indices)
        elif primitive == 4:
            faces = expand_triangle_list(indices)
        else:
            faces = []

        clipped_faces = [face for face in faces if max(face) < len(positions)]
        if not clipped_faces:
            continue

        stride_words = int((vertex_row.get("vertex") or {}).get("stride_words", 0))
        output_path = output_root / f"gap_f{frame:06d}_d{draw:04d}_{support}_sw{stride_words}.obj"
        write_obj(output_path, vertex_row, positions, uvs, clipped_faces)
        bounds_min, bounds_max = position_bounds(positions)
        projection = analyze_projection_candidate(positions, finite_constant_rows(vertex_row))
        exported.append(
            {
                "file": str(output_path),
                "frame": frame,
                "draw": draw,
                "support": support,
                "stride_words": stride_words,
                "vertices": len(positions),
                "faces": len(clipped_faces),
                "indexed": bool(vertex_row.get("indexed")),
                "vertex_shader": vertex_row.get("vertex_shader"),
                "pixel_shader": vertex_row.get("pixel_shader"),
                "bounds_min": bounds_min,
                "bounds_max": bounds_max,
                "vertex_constant_indices": constant_indices(vertex_row, "vertex_float_constant_values"),
                "pixel_constant_indices": constant_indices(vertex_row, "pixel_float_constant_values"),
                "projection": projection,
            }
        )
        if max_draws > 0 and len(exported) >= max_draws:
            break
    return exported


def write_export_manifest(output_path: Path, exported: list[dict[str, Any]]) -> None:
    output_path.parent.mkdir(parents=True, exist_ok=True)
    with output_path.open("w", encoding="utf-8", newline="") as handle:
        fieldnames = [
            "file",
            "frame",
            "draw",
            "support",
            "stride_words",
            "vertices",
            "faces",
            "indexed",
            "vertex_shader",
            "pixel_shader",
            "bounds_min_x",
            "bounds_min_y",
            "bounds_min_z",
            "bounds_max_x",
            "bounds_max_y",
            "bounds_max_z",
            "vertex_constant_indices",
            "pixel_constant_indices",
            "projection_candidate",
            "projection_orientation",
            "projection_finite_ratio",
            "projection_inside_ratio",
            "projection_ndc_min_x",
            "projection_ndc_min_y",
            "projection_ndc_min_z",
            "projection_ndc_max_x",
            "projection_ndc_max_y",
            "projection_ndc_max_z",
            "projection_score",
        ]
        writer = csv.DictWriter(handle, fieldnames=fieldnames)
        writer.writeheader()
        for row in exported:
            bounds_min = row["bounds_min"]
            bounds_max = row["bounds_max"]
            projection = row.get("projection") or {}
            ndc_min = projection.get("ndc_min") or ("", "", "")
            ndc_max = projection.get("ndc_max") or ("", "", "")
            writer.writerow(
                {
                    "file": row["file"],
                    "frame": row["frame"],
                    "draw": row["draw"],
                    "support": row["support"],
                    "stride_words": row["stride_words"],
                    "vertices": row["vertices"],
                    "faces": row["faces"],
                    "indexed": row["indexed"],
                    "vertex_shader": row["vertex_shader"],
                    "pixel_shader": row["pixel_shader"],
                    "bounds_min_x": compact_float(bounds_min[0]),
                    "bounds_min_y": compact_float(bounds_min[1]),
                    "bounds_min_z": compact_float(bounds_min[2]),
                    "bounds_max_x": compact_float(bounds_max[0]),
                    "bounds_max_y": compact_float(bounds_max[1]),
                    "bounds_max_z": compact_float(bounds_max[2]),
                    "vertex_constant_indices": row["vertex_constant_indices"],
                    "pixel_constant_indices": row["pixel_constant_indices"],
                    "projection_candidate": projection.get("indices", ""),
                    "projection_orientation": projection.get("orientation", ""),
                    "projection_finite_ratio": compact_float(projection.get("finite_ratio", "")),
                    "projection_inside_ratio": compact_float(projection.get("inside_ratio", "")),
                    "projection_ndc_min_x": compact_float(ndc_min[0]),
                    "projection_ndc_min_y": compact_float(ndc_min[1]),
                    "projection_ndc_min_z": compact_float(ndc_min[2]),
                    "projection_ndc_max_x": compact_float(ndc_max[0]),
                    "projection_ndc_max_y": compact_float(ndc_max[1]),
                    "projection_ndc_max_z": compact_float(ndc_max[2]),
                    "projection_score": compact_float(projection.get("score", "")),
                }
            )


def main() -> None:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("sample_root", type=Path, help="Folder containing samples.jsonl")
    parser.add_argument("--out-dir", type=Path, help="OBJ output folder")
    parser.add_argument("--manifest", type=Path, help="CSV manifest path")
    parser.add_argument("--support", default="unsupported_transform")
    parser.add_argument("--shader", help="Filter to one vertex shader hash")
    parser.add_argument("--max-draws", type=int, default=12)
    args = parser.parse_args()

    sample_root = args.sample_root
    output_root = args.out_dir or (sample_root / "obj")
    exported = export_draws(sample_root, output_root, args.support, args.max_draws, args.shader)
    manifest_path = args.manifest or (output_root / "gap_obj_manifest.csv")
    write_export_manifest(manifest_path, exported)
    print(f"Exported {len(exported)} OBJ previews to {output_root}")
    print(f"Wrote manifest to {manifest_path}")
    for row in exported:
        print(
            f"{Path(row['file']).name}: vertices={row['vertices']} faces={row['faces']} "
            f"stride_words={row['stride_words']} bounds={row['bounds_min']}..{row['bounds_max']} "
            f"project={row.get('projection', {}).get('indices', 'n/a')} "
            f"inside={compact_float(row.get('projection', {}).get('inside_ratio', ''))} "
            f"VS={row['vertex_shader']} PS={row['pixel_shader']}"
        )


if __name__ == "__main__":
    main()
