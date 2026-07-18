#!/usr/bin/env python3
"""Export a useful subset of SW2E/Koei G1M geometry to Wavefront OBJ.

This is intentionally focused on the G1MG layouts found in Samurai Warriors 2
Empires so far: big-endian G1M_0030/G1MG0040 chunks with position, normal, UV,
color, and joint-index vertex declarations. It is meant as a practical bridge
toward Blender map/model editing, not a complete G1M implementation.
"""

from __future__ import annotations

import argparse
import csv
import re
import struct
from dataclasses import dataclass, field
from pathlib import Path
from typing import Iterable


SEMANTIC_POSITION = 0x00
SEMANTIC_NORMAL = 0x03
SEMANTIC_UV = 0x05

TYPE_FLOAT_X2 = 0x01
TYPE_FLOAT_X3 = 0x02
TYPE_FLOAT_X4 = 0x03

PRIM_TRIANGLE_LIST = 3
PRIM_TRIANGLE_STRIP = 4


@dataclass
class VertexBuffer:
    index: int
    stride: int
    count: int
    data_offset: int


@dataclass
class VertexAttribute:
    buffer_id: int
    stream_offset: int
    data_type: int
    semantic: int
    layer: int


@dataclass
class VertexAttributeSet:
    index: int
    attributes: list[VertexAttribute] = field(default_factory=list)


@dataclass
class IndexBuffer:
    index: int
    count: int
    data_type: int
    bit_width: int
    data_offset: int


@dataclass
class MaterialTexture:
    index: int
    layer: int
    texture_type: int
    other_type: int
    tile_mode_x: int
    tile_mode_y: int


@dataclass
class Material:
    index: int
    textures: list[MaterialTexture] = field(default_factory=list)


@dataclass
class Submesh:
    index: int
    submesh_type: int
    vertex_attribute_set: int
    bone_palette_index: int
    material_index: int
    index_buffer_index: int
    primitive_type: int
    vertex_offset: int
    vertex_count: int
    index_offset: int
    index_count: int


@dataclass
class G1MGChunk:
    index: int
    offset: int
    version: str
    materials: list[Material] = field(default_factory=list)
    vertex_buffers: list[VertexBuffer] = field(default_factory=list)
    attribute_sets: list[VertexAttributeSet] = field(default_factory=list)
    index_buffers: list[IndexBuffer] = field(default_factory=list)
    submeshes: list[Submesh] = field(default_factory=list)
    warnings: list[str] = field(default_factory=list)


@dataclass
class ObjStream:
    vertex_base: int
    uv_base: int | None
    normal_base: int | None
    count: int


class ParseError(RuntimeError):
    pass


def read_ascii(data: bytes, offset: int, size: int) -> str:
    if offset + size > len(data):
        return ""
    return data[offset : offset + size].decode("ascii", errors="replace")


def read_u16be(data: bytes, offset: int) -> int:
    return int.from_bytes(data[offset : offset + 2], "big")


def read_u32be(data: bytes, offset: int) -> int:
    return int.from_bytes(data[offset : offset + 4], "big")


def read_f32be(data: bytes, offset: int) -> float:
    return struct.unpack(">f", data[offset : offset + 4])[0]


def version_number(version: str) -> int:
    match = re.search(r"\d+", version)
    return int(match.group(0)) if match else 0


def align(value: int, alignment: int) -> int:
    return (value + alignment - 1) & ~(alignment - 1)


def parse_g1m(path: Path) -> list[G1MGChunk]:
    data = path.read_bytes()
    if len(data) < 0x18 or read_ascii(data, 0, 4) != "G1M_":
        raise ParseError("not a G1M_ file")

    first_chunk = read_u32be(data, 12)
    cursor = first_chunk
    chunk_index = 0
    chunks: list[G1MGChunk] = []

    while cursor + 12 <= len(data):
        magic = read_ascii(data, cursor, 4)
        if not re.match(r"^G1M[A-Z_]$", magic):
            break

        version = read_ascii(data, cursor + 4, 4)
        size = read_u32be(data, cursor + 8)
        if size < 12 or cursor + size > len(data):
            raise ParseError(f"invalid chunk size at 0x{cursor:08X}")

        if magic == "G1MG":
            chunks.append(parse_g1mg(data, cursor, size, version, chunk_index))

        cursor += size
        chunk_index += 1

    return chunks


def parse_g1mg(data: bytes, offset: int, size: int, version: str, index: int) -> G1MGChunk:
    chunk = G1MGChunk(index=index, offset=offset, version=version)
    chunk_end = offset + size
    header_offset = offset + 12
    if header_offset + 36 > chunk_end:
        chunk.warnings.append("truncated G1MG header")
        return chunk

    section_count = read_u32be(data, header_offset + 32)
    section_offset = header_offset + 36

    for section_index in range(section_count):
        if section_offset + 12 > chunk_end:
            chunk.warnings.append(f"truncated section header {section_index}")
            break

        section_id = read_u32be(data, section_offset)
        section_size = read_u32be(data, section_offset + 4)
        item_count = read_u32be(data, section_offset + 8)
        section_end = section_offset + section_size
        if section_size < 12 or section_end > chunk_end:
            chunk.warnings.append(f"invalid section size for 0x{section_id:08X}")
            break

        if section_id == 0x00010002:
            chunk.materials = parse_materials(
                data, section_offset, section_end, item_count, chunk.warnings
            )
        elif section_id == 0x00010004:
            chunk.vertex_buffers = parse_vertex_buffers(
                data, section_offset, section_end, item_count, version, chunk.warnings
            )
        elif section_id == 0x00010005:
            chunk.attribute_sets = parse_attribute_sets(
                data, section_offset, section_end, item_count, chunk.warnings
            )
        elif section_id == 0x00010007:
            chunk.index_buffers = parse_index_buffers(
                data, section_offset, section_end, item_count, version, chunk.warnings
            )
        elif section_id == 0x00010008:
            chunk.submeshes = parse_submeshes(
                data, section_offset, section_end, item_count, chunk.warnings
            )

        section_offset = section_end

    return chunk


def parse_materials(
    data: bytes,
    section_offset: int,
    section_end: int,
    item_count: int,
    warnings: list[str],
) -> list[Material]:
    cursor = section_offset + 12
    materials: list[Material] = []

    for index in range(item_count):
        if cursor + 16 > section_end:
            warnings.append(f"truncated material {index}")
            break

        texture_count = read_u32be(data, cursor + 4)
        texture_offset = cursor + 16
        record_end = texture_offset + texture_count * 12
        if record_end > section_end:
            warnings.append(f"material {index} texture list overruns section")
            break

        material = Material(index=index)
        for texture_index in range(texture_count):
            item_offset = texture_offset + texture_index * 12
            material.textures.append(
                MaterialTexture(
                    index=read_u16be(data, item_offset),
                    layer=read_u16be(data, item_offset + 2),
                    texture_type=read_u16be(data, item_offset + 4),
                    other_type=read_u16be(data, item_offset + 6),
                    tile_mode_x=read_u16be(data, item_offset + 8),
                    tile_mode_y=read_u16be(data, item_offset + 10),
                )
            )
        materials.append(material)
        cursor = record_end

    return materials


def parse_vertex_buffers(
    data: bytes,
    section_offset: int,
    section_end: int,
    item_count: int,
    version: str,
    warnings: list[str],
) -> list[VertexBuffer]:
    cursor = section_offset + 12
    header_size = 16 if version_number(version) > 40 else 12
    buffers: list[VertexBuffer] = []

    for index in range(item_count):
        if cursor + header_size > section_end:
            warnings.append(f"truncated vertex buffer {index}")
            break

        stride = read_u32be(data, cursor + 4)
        count = read_u32be(data, cursor + 8)
        if stride <= 0 or count <= 0:
            warnings.append(f"invalid vertex buffer {index}: stride={stride} count={count}")
            break

        data_offset = cursor + header_size
        data_end = data_offset + stride * count
        if data_end > section_end:
            warnings.append(f"vertex buffer {index} overruns section")
            break

        buffers.append(VertexBuffer(index=index, stride=stride, count=count, data_offset=data_offset))
        cursor = data_end

    if cursor < section_end and len(buffers) < item_count:
        warnings.append("unparsed vertex buffer tail remains")

    return buffers


def parse_attribute_sets(
    data: bytes,
    section_offset: int,
    section_end: int,
    item_count: int,
    warnings: list[str],
) -> list[VertexAttributeSet]:
    cursor = section_offset + 12
    sets: list[VertexAttributeSet] = []

    for set_index in range(item_count):
        if cursor + 4 > section_end:
            warnings.append(f"truncated vertex attribute set {set_index}")
            break

        ref_count = read_u32be(data, cursor)
        cursor += 4
        refs: list[int] = []
        for _ in range(ref_count):
            if cursor + 4 > section_end:
                warnings.append(f"truncated vertex buffer refs in set {set_index}")
                break
            refs.append(read_u32be(data, cursor))
            cursor += 4

        if cursor + 4 > section_end:
            warnings.append(f"missing attribute count in set {set_index}")
            break

        attr_count = read_u32be(data, cursor)
        cursor += 4
        attr_set = VertexAttributeSet(index=set_index)

        for attr_index in range(attr_count):
            if cursor + 8 > section_end:
                warnings.append(f"truncated attribute {attr_index} in set {set_index}")
                break

            ref_index = read_u16be(data, cursor)
            stream_offset = read_u16be(data, cursor + 2)
            data_type = data[cursor + 4]
            semantic = data[cursor + 6]
            layer = data[cursor + 7]
            buffer_id = refs[ref_index] if ref_index < len(refs) else -1
            attr_set.attributes.append(
                VertexAttribute(
                    buffer_id=buffer_id,
                    stream_offset=stream_offset,
                    data_type=data_type,
                    semantic=semantic,
                    layer=layer,
                )
            )
            cursor += 8

        sets.append(attr_set)

    return sets


def parse_index_buffers(
    data: bytes,
    section_offset: int,
    section_end: int,
    item_count: int,
    version: str,
    warnings: list[str],
) -> list[IndexBuffer]:
    cursor = section_offset + 12
    header_size = 12 if version_number(version) > 40 else 8
    buffers: list[IndexBuffer] = []

    for index in range(item_count):
        if cursor + header_size > section_end:
            warnings.append(f"truncated index buffer {index}")
            break

        count = read_u32be(data, cursor)
        data_type = read_u32be(data, cursor + 4)
        bit_width = {0x08: 1, 0x10: 2, 0x20: 4}.get(data_type, 0)
        data_offset = cursor + header_size
        data_end = align(data_offset + count * bit_width, 4) if bit_width else section_end

        if not bit_width:
            warnings.append(f"unsupported index buffer type 0x{data_type:X} at {index}")
            break
        if data_end > section_end:
            warnings.append(f"index buffer {index} overruns section")
            break

        buffers.append(
            IndexBuffer(
                index=index,
                count=count,
                data_type=data_type,
                bit_width=bit_width,
                data_offset=data_offset,
            )
        )
        cursor = data_end

    return buffers


def parse_submeshes(
    data: bytes,
    section_offset: int,
    section_end: int,
    item_count: int,
    warnings: list[str],
) -> list[Submesh]:
    cursor = section_offset + 12
    record_size = 56
    submeshes: list[Submesh] = []

    for index in range(item_count):
        if cursor + record_size > section_end:
            warnings.append(f"truncated submesh {index}")
            break

        submeshes.append(
            Submesh(
                index=index,
                submesh_type=read_u32be(data, cursor),
                vertex_attribute_set=read_u32be(data, cursor + 4),
                bone_palette_index=read_u32be(data, cursor + 8),
                material_index=read_u32be(data, cursor + 24),
                index_buffer_index=read_u32be(data, cursor + 28),
                primitive_type=read_u32be(data, cursor + 36),
                vertex_offset=read_u32be(data, cursor + 40),
                vertex_count=read_u32be(data, cursor + 44),
                index_offset=read_u32be(data, cursor + 48),
                index_count=read_u32be(data, cursor + 52),
            )
        )
        cursor += record_size

    return submeshes


def find_attribute(attr_set: VertexAttributeSet, semantic: int) -> VertexAttribute | None:
    for attr in attr_set.attributes:
        if attr.semantic == semantic:
            return attr
    return None


def read_float_tuple(data: bytes, offset: int, data_type: int, width: int) -> tuple[float, ...]:
    if data_type == TYPE_FLOAT_X2:
        count = 2
    elif data_type == TYPE_FLOAT_X3:
        count = 3
    elif data_type == TYPE_FLOAT_X4:
        count = 4
    else:
        raise ParseError(f"unsupported float vertex data type 0x{data_type:X}")

    values = tuple(read_f32be(data, offset + i * 4) for i in range(count))
    if len(values) < width:
        values = values + tuple(0.0 for _ in range(width - len(values)))
    return values[:width]


def read_index(data: bytes, buffer: IndexBuffer, index: int) -> int:
    offset = buffer.data_offset + index * buffer.bit_width
    if buffer.bit_width == 1:
        return data[offset]
    if buffer.bit_width == 2:
        return read_u16be(data, offset)
    return read_u32be(data, offset)


def strip_triangles(indices: list[int]) -> Iterable[tuple[int, int, int]]:
    for i in range(0, len(indices) - 2):
        a, b, c = indices[i], indices[i + 1], indices[i + 2]
        if a == b or b == c or a == c:
            continue
        if i & 1:
            yield b, a, c
        else:
            yield a, b, c


def triangle_indices(
    indices: list[int],
    primitive_type: int,
    restart_index: int | None,
) -> Iterable[tuple[int, int, int]]:
    if primitive_type == PRIM_TRIANGLE_LIST:
        for i in range(0, len(indices) - 2, 3):
            a, b, c = indices[i], indices[i + 1], indices[i + 2]
            if restart_index is not None and restart_index in (a, b, c):
                continue
            if a != b and b != c and a != c:
                yield a, b, c
    elif primitive_type == PRIM_TRIANGLE_STRIP:
        strip: list[int] = []
        for index in indices:
            if restart_index is not None and index == restart_index:
                yield from strip_triangles(strip)
                strip = []
            else:
                strip.append(index)
        yield from strip_triangles(strip)


def format_face_token(index: int, stream: ObjStream) -> str:
    vertex = stream.vertex_base + index
    uv = stream.uv_base + index if stream.uv_base is not None else None
    normal = stream.normal_base + index if stream.normal_base is not None else None

    if uv is not None and normal is not None:
        return f"{vertex}/{uv}/{normal}"
    if uv is not None:
        return f"{vertex}/{uv}"
    if normal is not None:
        return f"{vertex}//{normal}"
    return str(vertex)


def material_name(chunk_index: int, material_index: int) -> str:
    return f"g1mg_{chunk_index}_mat_{material_index}"


def write_mtl(path: Path, chunks: list[G1MGChunk]) -> int:
    material_names: set[str] = set()
    for chunk in chunks:
        for material in chunk.materials:
            material_names.add(material_name(chunk.index, material.index))
        for submesh in chunk.submeshes:
            material_names.add(material_name(chunk.index, submesh.material_index))

    if not material_names:
        return 0

    with path.open("w", encoding="utf-8", newline="\n") as mtl:
        for chunk in chunks:
            material_count = max(
                len(chunk.materials),
                max((submesh.material_index for submesh in chunk.submeshes), default=-1) + 1,
            )
            for material_index in range(material_count):
                name = material_name(chunk.index, material_index)
                if name not in material_names:
                    continue

                mtl.write(f"newmtl {name}\n")
                mtl.write("Kd 0.8 0.8 0.8\n")
                if material_index < len(chunk.materials):
                    for texture_slot, texture in enumerate(chunk.materials[material_index].textures):
                        mtl.write(
                            "# texture_slot={} texture_index={} uv_layer={} type={} other={} "
                            "tile_x={} tile_y={}\n".format(
                                texture_slot,
                                texture.index,
                                texture.layer,
                                texture.texture_type,
                                texture.other_type,
                                texture.tile_mode_x,
                                texture.tile_mode_y,
                            )
                        )
                mtl.write("\n")

    return len(material_names)


def export_obj(path: Path, out_path: Path, scale: float, flip_v: bool) -> dict[str, object]:
    data = path.read_bytes()
    chunks = parse_g1m(path)
    if not chunks:
        raise ParseError("no G1MG geometry chunks found")

    out_path.parent.mkdir(parents=True, exist_ok=True)
    material_count = write_mtl(out_path.with_suffix(".mtl"), chunks)
    warnings: list[str] = []
    vertex_total = 0
    uv_total = 0
    normal_total = 0
    face_total = 0
    submesh_total = 0
    stream_map: dict[tuple[int, int], ObjStream] = {}

    with out_path.open("w", encoding="utf-8", newline="\n") as obj:
        obj.write(f"# Exported from {path.name}\n")
        obj.write("# Source format: G1M_/G1MG, big-endian SW2E subset\n")
        if material_count:
            obj.write(f"mtllib {out_path.with_suffix('.mtl').name}\n")

        for chunk in chunks:
            warnings.extend(chunk.warnings)
            obj.write(f"\no g1mg_{chunk.index}\n")

            for attr_set in chunk.attribute_sets:
                pos_attr = find_attribute(attr_set, SEMANTIC_POSITION)
                if pos_attr is None:
                    warnings.append(f"chunk {chunk.index} attr_set {attr_set.index}: missing position")
                    continue
                if pos_attr.buffer_id < 0 or pos_attr.buffer_id >= len(chunk.vertex_buffers):
                    warnings.append(f"chunk {chunk.index} attr_set {attr_set.index}: invalid position buffer")
                    continue

                pos_buffer = chunk.vertex_buffers[pos_attr.buffer_id]
                normal_attr = find_attribute(attr_set, SEMANTIC_NORMAL)
                uv_attr = find_attribute(attr_set, SEMANTIC_UV)

                vertex_base = vertex_total + 1
                uv_base = uv_total + 1 if uv_attr else None
                normal_base = normal_total + 1 if normal_attr else None
                stream_map[(chunk.index, attr_set.index)] = ObjStream(
                    vertex_base=vertex_base,
                    uv_base=uv_base,
                    normal_base=normal_base,
                    count=pos_buffer.count,
                )

                for vertex_index in range(pos_buffer.count):
                    vertex_offset = (
                        pos_buffer.data_offset
                        + vertex_index * pos_buffer.stride
                        + pos_attr.stream_offset
                    )
                    x, y, z = read_float_tuple(data, vertex_offset, pos_attr.data_type, 3)
                    obj.write(f"v {x * scale:.9g} {y * scale:.9g} {z * scale:.9g}\n")
                vertex_total += pos_buffer.count

                if uv_attr:
                    if uv_attr.buffer_id < 0 or uv_attr.buffer_id >= len(chunk.vertex_buffers):
                        warnings.append(f"chunk {chunk.index} attr_set {attr_set.index}: invalid uv buffer")
                    else:
                        uv_buffer = chunk.vertex_buffers[uv_attr.buffer_id]
                        for vertex_index in range(pos_buffer.count):
                            if vertex_index >= uv_buffer.count:
                                u, v = 0.0, 0.0
                            else:
                                uv_offset = (
                                    uv_buffer.data_offset
                                    + vertex_index * uv_buffer.stride
                                    + uv_attr.stream_offset
                                )
                                u, v = read_float_tuple(data, uv_offset, uv_attr.data_type, 2)
                                if flip_v:
                                    v = 1.0 - v
                            obj.write(f"vt {u:.9g} {v:.9g}\n")
                        uv_total += pos_buffer.count

                if normal_attr:
                    if normal_attr.buffer_id < 0 or normal_attr.buffer_id >= len(chunk.vertex_buffers):
                        warnings.append(f"chunk {chunk.index} attr_set {attr_set.index}: invalid normal buffer")
                    else:
                        normal_buffer = chunk.vertex_buffers[normal_attr.buffer_id]
                        for vertex_index in range(pos_buffer.count):
                            if vertex_index >= normal_buffer.count:
                                nx, ny, nz = 0.0, 1.0, 0.0
                            else:
                                normal_offset = (
                                    normal_buffer.data_offset
                                    + vertex_index * normal_buffer.stride
                                    + normal_attr.stream_offset
                                )
                                nx, ny, nz = read_float_tuple(data, normal_offset, normal_attr.data_type, 3)
                            obj.write(f"vn {nx:.9g} {ny:.9g} {nz:.9g}\n")
                        normal_total += pos_buffer.count

            for submesh in chunk.submeshes:
                submesh_total += 1
                stream = stream_map.get((chunk.index, submesh.vertex_attribute_set))
                if stream is None:
                    warnings.append(
                        f"chunk {chunk.index} submesh {submesh.index}: missing vertex stream"
                    )
                    continue
                if submesh.index_buffer_index >= len(chunk.index_buffers):
                    warnings.append(
                        f"chunk {chunk.index} submesh {submesh.index}: invalid index buffer"
                    )
                    continue
                if submesh.primitive_type not in (PRIM_TRIANGLE_LIST, PRIM_TRIANGLE_STRIP):
                    warnings.append(
                        f"chunk {chunk.index} submesh {submesh.index}: "
                        f"unsupported prim_type {submesh.primitive_type}"
                    )
                    continue

                index_buffer = chunk.index_buffers[submesh.index_buffer_index]
                if submesh.index_offset + submesh.index_count > index_buffer.count:
                    warnings.append(
                        f"chunk {chunk.index} submesh {submesh.index}: index range overruns buffer"
                    )
                    continue

                indices = [
                    read_index(data, index_buffer, submesh.index_offset + i)
                    for i in range(submesh.index_count)
                ]
                obj.write(
                    f"\ng chunk_{chunk.index}_submesh_{submesh.index}_mat_{submesh.material_index}\n"
                )
                obj.write(f"usemtl {material_name(chunk.index, submesh.material_index)}\n")
                restart_index = (1 << (index_buffer.bit_width * 8)) - 1
                for tri in triangle_indices(indices, submesh.primitive_type, restart_index):
                    if any(i >= stream.count for i in tri):
                        warnings.append(
                            f"chunk {chunk.index} submesh {submesh.index}: face index outside stream"
                        )
                        continue
                    a, b, c = (format_face_token(i, stream) for i in tri)
                    obj.write(f"f {a} {b} {c}\n")
                    face_total += 1

    return {
        "input": str(path),
        "output": str(out_path),
        "status": "ok",
        "g1mg_chunks": len(chunks),
        "material_slots": material_count,
        "submeshes": submesh_total,
        "vertices": vertex_total,
        "uvs": uv_total,
        "normals": normal_total,
        "faces": face_total,
        "warnings": "; ".join(dict.fromkeys(warnings)),
    }


def collect_inputs(input_path: Path) -> list[Path]:
    if input_path.is_file():
        return [input_path]
    return sorted(path for path in input_path.rglob("*") if path.suffix.lower() == ".g1m")


def output_name(input_root: Path, path: Path) -> str:
    if input_root.is_file():
        return f"{path.stem}.obj"
    rel = path.relative_to(input_root).with_suffix("")
    safe_parts = [re.sub(r"[^A-Za-z0-9_.-]+", "_", part) for part in rel.parts]
    return "__".join(safe_parts) + ".obj"


def main() -> int:
    parser = argparse.ArgumentParser(
        description="Export SW2E G1M/G1MG model files to OBJ."
    )
    parser.add_argument("input", type=Path, help="Input .g1m file or directory.")
    parser.add_argument("--out-dir", type=Path, required=True, help="OBJ output directory.")
    parser.add_argument("--scale", type=float, default=1.0, help="Scale exported positions.")
    parser.add_argument("--flip-v", action="store_true", help="Flip OBJ texture V coordinate.")
    parser.add_argument(
        "--report",
        type=Path,
        default=None,
        help="CSV report path. Defaults to <out-dir>/export_manifest.csv.",
    )
    args = parser.parse_args()

    input_path = args.input.resolve()
    out_dir = args.out_dir.resolve()
    report_path = args.report.resolve() if args.report else out_dir / "export_manifest.csv"
    out_dir.mkdir(parents=True, exist_ok=True)

    rows: list[dict[str, object]] = []
    for path in collect_inputs(input_path):
        out_path = out_dir / output_name(input_path, path)
        try:
            rows.append(export_obj(path, out_path, args.scale, args.flip_v))
        except Exception as exc:  # Keep batch exports going.
            rows.append(
                {
                    "input": str(path),
                    "output": str(out_path),
                    "status": "error",
                    "g1mg_chunks": 0,
                    "material_slots": 0,
                    "submeshes": 0,
                    "vertices": 0,
                    "uvs": 0,
                    "normals": 0,
                    "faces": 0,
                    "warnings": str(exc),
                }
            )

    with report_path.open("w", encoding="utf-8", newline="") as report:
        writer = csv.DictWriter(
            report,
            fieldnames=[
                "input",
                "output",
                "status",
                "g1mg_chunks",
                "material_slots",
                "submeshes",
                "vertices",
                "uvs",
                "normals",
                "faces",
                "warnings",
            ],
        )
        writer.writeheader()
        writer.writerows(rows)

    ok = sum(1 for row in rows if row["status"] == "ok")
    errors = len(rows) - ok
    print(f"exported={ok} errors={errors} report={report_path}")
    return 1 if errors else 0


if __name__ == "__main__":
    raise SystemExit(main())
