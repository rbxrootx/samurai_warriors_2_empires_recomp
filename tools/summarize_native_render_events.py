#!/usr/bin/env python3
"""Summarize native-render JSONL captures into renderer replacement clues."""

from __future__ import annotations

import argparse
import collections
import csv
import json
import sys
from pathlib import Path
from typing import Any, Iterable


PRIMITIVE_NAMES = {
    0x00: "none",
    0x01: "point_list",
    0x02: "line_list",
    0x03: "line_strip",
    0x04: "triangle_list",
    0x05: "triangle_fan",
    0x06: "triangle_strip",
    0x07: "triangle_with_w_flags",
    0x08: "rectangle_list",
    0x0C: "line_loop",
    0x0D: "quad_list",
    0x0E: "quad_strip",
    0x0F: "polygon",
    0x10: "2d_copy_rect_or_line_patch",
    0x11: "2d_copy_rect_or_triangle_patch",
    0x12: "2d_copy_rect_or_quad_patch",
    0x13: "2d_copy_rect_v3",
    0x14: "2d_fill_rect",
    0x15: "2d_line_strip",
    0x16: "2d_tri_strip",
}

FLOAT_FORMAT_COMPONENTS = {
    36: 1,  # 32_FLOAT
    37: 2,  # 32_32_FLOAT
    38: 4,  # 32_32_32_32_FLOAT
    57: 3,  # 32_32_32_FLOAT
}


def primitive_name(value: int) -> str:
    return PRIMITIVE_NAMES.get(value, f"unknown_0x{value:X}")


def shader_pair_key(event: dict[str, Any]) -> tuple[str, str]:
    shaders = event.get("shaders", {})
    return shaders.get("vertex", "0x0000000000000000"), shaders.get("pixel", "0x0000000000000000")


def register_key(event: dict[str, Any], name: str) -> str:
    registers = event.get("registers", {})
    return registers.get(name, "0x00000000")


def render_target_key(event: dict[str, Any]) -> tuple[str, ...]:
    targets = tuple(event.get("render_targets", []))
    return (
        register_key(event, "rb_modecontrol"),
        register_key(event, "rb_surface_info"),
        register_key(event, "rb_depth_info"),
        *targets,
    )


def render_state_key(event: dict[str, Any]) -> tuple[str, str, str, str]:
    state = event.get("render_state", {})
    blend_controls = state.get("rt_blendcontrol", [])
    rt0_blend = blend_controls[0] if blend_controls else "0x00000000"
    return (
        state.get("normalized_color_mask", "0x00000000"),
        state.get("normalized_depthcontrol", "0x00000000"),
        state.get("pixel_color_target_mask", "0x00000000"),
        rt0_blend,
    )


def draw_effect_key(event: dict[str, Any]) -> tuple[bool, bool, bool, bool]:
    effects = event.get("draw_effects", {})
    return (
        bool(effects.get("primitive_polygonal", False)),
        bool(effects.get("rasterization_potentially_done", False)),
        bool(effects.get("pixel_shader_needed_with_rasterization", False)),
        bool(effects.get("output_merger_writes", False)),
    )


def float_attribute_components(attribute: dict[str, Any]) -> int:
    return FLOAT_FORMAT_COMPONENTS.get(int(attribute.get("data_format", 0)), 0)


def find_attribute_by_result(
    fetch: dict[str, Any], result_storage_index: int, min_components: int
) -> dict[str, Any] | None:
    for attribute in fetch.get("attributes", []):
        if not isinstance(attribute, dict):
            continue
        if (
            int(attribute.get("result_storage_target", 0)) == 1
            and int(attribute.get("result_storage_index", -1)) == result_storage_index
            and float_attribute_components(attribute) >= min_components
        ):
            return attribute
    return None


def has_position_attribute(fetch: dict[str, Any]) -> bool:
    if find_attribute_by_result(fetch, 1, 3):
        return True
    return any(
        isinstance(attribute, dict)
        and int(attribute.get("offset_words", -1)) == 0
        and float_attribute_components(attribute) >= 3
        for attribute in fetch.get("attributes", [])
    )


def has_texcoord_attribute(fetch: dict[str, Any]) -> bool:
    attributes = [
        attribute for attribute in fetch.get("attributes", []) if isinstance(attribute, dict)
    ]
    if any(
        int(attribute.get("offset_words", 0)) > 0
        and float_attribute_components(attribute) == 2
        for attribute in attributes
    ):
        return True
    if find_attribute_by_result(fetch, 0, 2):
        return True
    return any(
        int(attribute.get("offset_words", 0)) != 0
        and float_attribute_components(attribute) >= 2
        for attribute in attributes
    )


def can_decode_textured_vertex_fetch(fetch: dict[str, Any]) -> bool:
    if has_position_attribute(fetch) and has_texcoord_attribute(fetch):
        return True
    return int(fetch.get("stride_words", 0)) == 6 and int(fetch.get("attribute_count", 0)) == 3


def is_screen_space_textured_vertex_fetch(fetch: dict[str, Any]) -> bool:
    return int(fetch.get("stride_words", 0)) == 6 and int(fetch.get("attribute_count", 0)) == 3


def vertex_fetch_covers_draw(event: dict[str, Any], fetch: dict[str, Any]) -> bool:
    if bool(event.get("indexed", False)):
        return True
    index_count = int(event.get("index_count", 0))
    stride_words = int(fetch.get("stride_words", 0))
    return int(fetch.get("size_bytes", 0)) >= index_count * stride_words * 4


def is_textured_triangle_shape(event: dict[str, Any]) -> bool:
    primitive = int(event.get("primitive", 0))
    index_count = int(event.get("index_count", 0))
    indexed = bool(event.get("indexed", False))
    if not event.get("vertex_fetches") or not event.get("texture_fetches"):
        return False
    if indexed and int(event.get("index_length", 0)) <= 0:
        return False
    if primitive == 4:
        return index_count >= 3 and (index_count % 3) == 0
    if primitive == 6:
        return index_count >= 3
    return False


def native_replay_support_key(event: dict[str, Any]) -> str:
    info = event.get("shader_info", {})
    effects = event.get("draw_effects", {})
    color_mask, _depth_control, _pixel_targets, _rt0_blend = render_state_key(event)
    color_mask_value = int(color_mask, 16)
    output_merger_writes = bool(effects.get("output_merger_writes", False))
    memexport_used = int(info.get("vertex_memexport_mask", 0)) != 0 or int(
        info.get("pixel_memexport_mask", 0)
    ) != 0
    viz_query_used = register_key(event, "pa_sc_viz_query") != "0x00000000"
    if not output_merger_writes and not memexport_used and not viz_query_used:
        return "skip_no_output"
    if not output_merger_writes or (color_mask_value & 0x0F) == 0:
        return "depth_or_noncolor_output"
    index_count = int(event.get("index_count", 0))
    indexed = bool(event.get("indexed", False))
    vertex_fetches = event.get("vertex_fetches", [])
    texture_fetches = event.get("texture_fetches", [])

    if is_textured_triangle_shape(event):
        has_vertex_layout = any(
            is_screen_space_textured_vertex_fetch(fetch) and vertex_fetch_covers_draw(event, fetch)
            for fetch in vertex_fetches
        )
        if not has_vertex_layout:
            if any(
                can_decode_textured_vertex_fetch(fetch) and vertex_fetch_covers_draw(event, fetch)
                for fetch in vertex_fetches
            ):
                return "unsupported_textured_transform"
            return "unsupported_textured_layout"
        has_supported_texture = any(
            int(fetch.get("format", 0)) == 20
            and int(fetch.get("dimension", 0)) == 1
            and int(fetch.get("tiled", 0)) == 0
            and int(fetch.get("width", 0)) > 0
            and int(fetch.get("height", 0)) > 0
            for fetch in texture_fetches
        )
        if not has_supported_texture:
            return "unsupported_texture"
        return "supported_indexed_textured" if indexed else "supported_textured"

    if indexed:
        return "unsupported_indexed"

    primitive = int(event.get("primitive", 0))
    solid_shape = (primitive == 8 and index_count == 3) or (primitive == 6 and index_count == 4)
    if solid_shape and vertex_fetches and not texture_fetches:
        has_solid_layout = any(
            int(fetch.get("stride_words", 0)) in (2, 7)
            and int(fetch.get("size_bytes", 0))
            >= index_count * int(fetch.get("stride_words", 0)) * 4
            for fetch in vertex_fetches
        )
        return "supported_solid" if has_solid_layout else "unsupported_solid_layout"

    return "unsupported_shape"


def pass_family_key(event: dict[str, Any]) -> tuple[Any, ...]:
    primitive, index_count, indexed, _index_format, _index_length = draw_shape_key(event)
    vs, ps = shader_pair_key(event)
    color_mask, depth_control, pixel_targets, rt0_blend = render_state_key(event)
    primitive_polygonal, rasterization, pixel_needed, om_writes = draw_effect_key(event)
    info = event.get("shader_info", {})
    return (
        primitive,
        index_count,
        indexed,
        vs,
        ps,
        color_mask,
        depth_control,
        pixel_targets,
        rt0_blend,
        primitive_polygonal,
        rasterization,
        pixel_needed,
        om_writes,
        int(info.get("vertex_memexport_mask", 0)),
        int(info.get("pixel_memexport_mask", 0)),
        register_key(event, "pa_sc_viz_query"),
        len(event.get("vertex_fetches", [])),
        len(event.get("texture_fetches", [])),
    )


def constant_value_key(constant: dict[str, Any]) -> tuple[int, tuple[str, ...], tuple[str, ...]]:
    values = tuple(str(value) for value in constant.get("values", []))
    bits = tuple(str(value) for value in constant.get("bits", []))
    return int(constant.get("index", 0)), values, bits


def draw_shape_key(event: dict[str, Any]) -> tuple[str, int, bool, int, int]:
    primitive = int(event.get("primitive", 0))
    return (
        primitive_name(primitive),
        int(event.get("index_count", 0)),
        bool(event.get("indexed", False)),
        int(event.get("index_format", 0)),
        int(event.get("index_length", 0)),
    )


def index_element_size(index_format: int) -> int:
    return 2 if index_format == 0 else 4


def index_buffer_key(event: dict[str, Any]) -> tuple[str, int, int, int, int, int]:
    index_count = int(event.get("index_count", 0))
    index_format = int(event.get("index_format", 0))
    index_length = int(event.get("index_length", 0))
    needed_bytes = min(index_count * index_element_size(index_format), index_length)
    return (
        event.get("index_guest_base", "0x00000000"),
        index_length,
        needed_bytes,
        index_format,
        int(event.get("index_endianness", 0)),
        index_count,
    )


def int_value(value: Any, default: int = 0) -> int:
    if value is None:
        return default
    if isinstance(value, bool):
        return int(value)
    if isinstance(value, int):
        return value
    if isinstance(value, str):
        try:
            return int(value, 0)
        except ValueError:
            return default
    return default


def vertex_attribute_signature(fetch: dict[str, Any]) -> tuple[tuple[int, ...], ...]:
    signature: list[tuple[int, ...]] = []
    for attribute in fetch.get("attributes", []):
        if not isinstance(attribute, dict):
            continue
        signature.append(
            (
                int_value(attribute.get("data_format")),
                int_value(attribute.get("offset_words")),
                int_value(attribute.get("exp_adjust")),
                int_value(attribute.get("is_signed")),
                int_value(attribute.get("is_integer")),
                int_value(attribute.get("result_storage_target")),
                int_value(attribute.get("result_storage_index")),
                int_value(attribute.get("result_write_mask")),
                int_value(attribute.get("result_used_components")),
                int_value(attribute.get("result_swizzle")),
            )
        )
    return tuple(signature)


def format_vertex_attribute_signature(signature: tuple[tuple[int, ...], ...]) -> str:
    if not signature:
        return "none"
    parts = []
    for (
        data_format,
        offset_words,
        exp_adjust,
        is_signed,
        is_integer,
        result_storage_target,
        result_storage_index,
        result_write_mask,
        result_used_components,
        result_swizzle,
    ) in signature:
        kind = ("s" if is_signed else "u") + ("i" if is_integer else "f")
        parts.append(
            f"fmt={data_format}@{offset_words}:{kind}:exp={exp_adjust}"
            f"->t{result_storage_target}[{result_storage_index}]"
            f" mask=0x{result_write_mask:X}/used=0x{result_used_components:X}"
            f"/swz=0x{result_swizzle:X}"
        )
    return " | ".join(parts)


def vertex_layout_key(
    event: dict[str, Any], fetch: dict[str, Any], support: str
) -> tuple[str, bool, str, str, int, int, int, int, str]:
    primitive = primitive_name(int(event.get("primitive", 0)))
    vs, ps = shader_pair_key(event)
    return (
        primitive,
        bool(event.get("indexed", False)),
        vs,
        ps,
        int(fetch.get("fetch_constant", 0)),
        int(fetch.get("stride_words", 0)),
        int(fetch.get("attribute_count", 0)),
        len(event.get("texture_fetches", [])),
        support,
    )


def shader_info_key(event: dict[str, Any]) -> tuple[int, int, int, int, int, int, int, int, int, int]:
    info = event.get("shader_info", {})
    return (
        int(info.get("vertex_dwords", 0)),
        int(info.get("pixel_dwords", 0)),
        int(info.get("vertex_bindings", 0)),
        int(info.get("vertex_attributes", 0)),
        int(info.get("vertex_texture_bindings", 0)),
        int(info.get("pixel_texture_bindings", 0)),
        int(info.get("vertex_float_constants", 0)),
        int(info.get("pixel_float_constants", 0)),
        int(info.get("vertex_memexport_mask", 0)),
        int(info.get("pixel_memexport_mask", 0)),
    )


def load_events(path: Path, strict: bool) -> tuple[list[dict[str, Any]], list[str]]:
    events: list[dict[str, Any]] = []
    warnings: list[str] = []
    with path.open("r", encoding="utf-8") as file:
        lines = file.readlines()
        for line_number, line in enumerate(lines, start=1):
            line = line.strip()
            if not line:
                continue
            try:
                event = json.loads(line)
            except json.JSONDecodeError as exc:
                if not strict and line_number == len(lines):
                    warnings.append(f"ignored truncated final line {line_number}: {exc}")
                    break
                raise ValueError(f"{path}:{line_number}: {exc}") from exc
            if not isinstance(event, dict):
                raise ValueError(f"{path}:{line_number}: event is not a JSON object")
            events.append(event)
    return events, warnings


def make_summary() -> dict[str, Any]:
    return {
        "event_types": collections.Counter(),
        "shader_pairs": collections.Counter(),
        "primitive_counts": collections.Counter(),
        "render_targets": collections.Counter(),
        "render_states": collections.Counter(),
        "draw_effects": collections.Counter(),
        "native_replay_support": collections.Counter(),
        "pass_families": collections.Counter(),
        "draw_shapes": collections.Counter(),
        "shader_infos": collections.Counter(),
        "index_buffers": collections.Counter(),
        "vertex_layouts": collections.Counter(),
        "vertex_attribute_layouts": collections.Counter(),
        "vertex_fetches": collections.Counter(),
        "texture_fetches": collections.Counter(),
        "vertex_constants": collections.Counter(),
        "pixel_constants": collections.Counter(),
        "frame_draws": collections.Counter(),
        "swap_draw_counts": [],
    }


def add_event_to_summary(summary: dict[str, Any], event: dict[str, Any]) -> None:
    event_type = event.get("event")
    summary["event_types"][event_type or "unknown"] += 1
    if event_type == "draw":
        frame = int(event.get("frame", 0))
        primitive = int(event.get("primitive", 0))
        summary["frame_draws"][frame] += 1
        summary["shader_pairs"][shader_pair_key(event)] += 1
        summary["primitive_counts"][(primitive, primitive_name(primitive))] += 1
        summary["render_targets"][render_target_key(event)] += 1
        summary["render_states"][render_state_key(event)] += 1
        summary["draw_effects"][draw_effect_key(event)] += 1
        support = native_replay_support_key(event)
        summary["native_replay_support"][support] += 1
        summary["pass_families"][pass_family_key(event)] += 1
        summary["draw_shapes"][draw_shape_key(event)] += 1
        summary["shader_infos"][(shader_pair_key(event), shader_info_key(event))] += 1
        if bool(event.get("indexed", False)):
            summary["index_buffers"][index_buffer_key(event)] += 1
        for fetch in event.get("vertex_fetches", []):
            attribute_signature = vertex_attribute_signature(fetch)
            summary["vertex_layouts"][vertex_layout_key(event, fetch, support)] += 1
            if attribute_signature:
                primitive_name_value = primitive_name(int(event.get("primitive", 0)))
                vs, ps = shader_pair_key(event)
                summary["vertex_attribute_layouts"][
                    (
                        primitive_name_value,
                        bool(event.get("indexed", False)),
                        vs,
                        ps,
                        int(fetch.get("fetch_constant", 0)),
                        int(fetch.get("stride_words", 0)),
                        int(fetch.get("attribute_count", 0)),
                        attribute_signature,
                        support,
                    )
                ] += 1
            summary["vertex_fetches"][
                (
                    int(fetch.get("fetch_constant", 0)),
                    fetch.get("address_bytes", "0x00000000"),
                    int(fetch.get("size_bytes", 0)),
                    int(fetch.get("stride_words", 0)),
                    int(fetch.get("attribute_count", 0)),
                )
            ] += 1
        for fetch in event.get("texture_fetches", []):
            summary["texture_fetches"][
                (
                    int(fetch.get("fetch_constant", 0)),
                    fetch.get("base_address_bytes", "0x00000000"),
                    fetch.get("mip_address_bytes", "0x00000000"),
                    int(fetch.get("format", 0)),
                    int(fetch.get("dimension", 0)),
                    int(fetch.get("width", 0)),
                    int(fetch.get("height", 0)),
                    int(fetch.get("depth", 0)),
                    int(fetch.get("tiled", 0)),
                )
            ] += 1
        for constant in event.get("vertex_float_constant_values", []):
            summary["vertex_constants"][constant_value_key(constant)] += 1
        for constant in event.get("pixel_float_constant_values", []):
            summary["pixel_constants"][constant_value_key(constant)] += 1
    elif event_type == "swap":
        summary["swap_draw_counts"].append(
            (
                int(event.get("frame", 0)),
                int(event.get("frame_draw_count", 0)),
                event.get("frontbuffer_ptr", "0x00000000"),
                int(event.get("frontbuffer_width", 0)),
                int(event.get("frontbuffer_height", 0)),
            )
        )


def summarize(events: Iterable[dict[str, Any]]) -> dict[str, Any]:
    summary = make_summary()

    for event in events:
        add_event_to_summary(summary, event)

    return summary


def summarize_file(path: Path, strict: bool) -> tuple[dict[str, Any], list[str]]:
    summary = make_summary()
    warnings: list[str] = []
    with path.open("r", encoding="utf-8") as file:
        for line_number, line in enumerate(file, start=1):
            line = line.strip()
            if not line:
                continue
            try:
                event = json.loads(line)
            except json.JSONDecodeError as exc:
                if not strict and not file.read(1):
                    warnings.append(f"ignored truncated final line {line_number}: {exc}")
                    break
                raise ValueError(f"{path}:{line_number}: {exc}") from exc
            if not isinstance(event, dict):
                raise ValueError(f"{path}:{line_number}: event is not a JSON object")
            add_event_to_summary(summary, event)
    return summary, warnings


def print_summary(path: Path, summary: dict[str, Any], top: int) -> None:
    print(f"Native event capture: {path}")
    print()

    print("Events:")
    for event_type, count in summary["event_types"].most_common():
        print(f"  {event_type}: {count}")
    print()

    frame_draws = summary["frame_draws"]
    if frame_draws:
        counts = list(frame_draws.values())
        print(
            "Frames with draws: "
            f"{len(frame_draws)}  min={min(counts)}  max={max(counts)}  "
            f"avg={sum(counts) / len(counts):.1f}"
        )
        print()

    print("Top shader pairs:")
    for (vs, ps), count in summary["shader_pairs"].most_common(top):
        print(f"  {count:6d}  VS={vs}  PS={ps}")
    print()

    print("Primitive use:")
    for (primitive, name), count in summary["primitive_counts"].most_common(top):
        print(f"  {count:6d}  0x{primitive:02X} {name}")
    print()

    print("Top draw shapes:")
    for (primitive, index_count, indexed, index_format, index_length), count in summary[
        "draw_shapes"
    ].most_common(top):
        print(
            f"  {count:6d}  prim={primitive} indices={index_count} indexed={indexed} "
            f"index_format={index_format} index_length={index_length}"
        )
    print()

    print("Top shader data requirements:")
    for ((vs, ps), info), count in summary["shader_infos"].most_common(top):
        (
            vs_dwords,
            ps_dwords,
            vertex_bindings,
            vertex_attributes,
            vertex_texture_bindings,
            pixel_texture_bindings,
            vertex_float_constants,
            pixel_float_constants,
            vertex_memexport_mask,
            pixel_memexport_mask,
        ) = info
        print(
            f"  {count:6d}  VS={vs} PS={ps} "
            f"vfetch={vertex_bindings}/{vertex_attributes} "
            f"tfetch_vs={vertex_texture_bindings} tfetch_ps={pixel_texture_bindings} "
            f"consts={vertex_float_constants}/{pixel_float_constants} "
            f"ucode={vs_dwords}/{ps_dwords} memexport={vertex_memexport_mask}/{pixel_memexport_mask}"
        )
    print()

    if summary["vertex_layouts"]:
        print("Top vertex layouts:")
        for (
            primitive,
            indexed,
            vs,
            ps,
            fetch_constant,
            stride_words,
            attribute_count,
            texture_fetch_count,
            support,
        ), count in summary["vertex_layouts"].most_common(top):
            print(
                f"  {count:6d}  prim={primitive} indexed={indexed} fc={fetch_constant} "
                f"stride_words={stride_words} attrs={attribute_count} "
                f"tfetch={texture_fetch_count} support={support} VS={vs} PS={ps}"
            )
        print()

    if summary["vertex_attribute_layouts"]:
        print("Top vertex attribute signatures:")
        for (
            primitive,
            indexed,
            vs,
            ps,
            fetch_constant,
            stride_words,
            attribute_count,
            attribute_signature,
            support,
        ), count in summary["vertex_attribute_layouts"].most_common(top):
            print(
                f"  {count:6d}  prim={primitive} indexed={indexed} fc={fetch_constant} "
                f"stride_words={stride_words} attrs={attribute_count} support={support} "
                f"signature={format_vertex_attribute_signature(attribute_signature)} "
                f"VS={vs} PS={ps}"
            )
        print()

    if summary["index_buffers"]:
        print("Top index buffers:")
        for (base_address, index_length, needed_bytes, index_format, endian, index_count), count in (
            summary["index_buffers"].most_common(top)
        ):
            print(
                f"  {count:6d}  base={base_address} length={index_length} "
                f"needed={needed_bytes} format={index_format} endian={endian} "
                f"indices={index_count}"
            )
        print()

    print("Top render-target signatures:")
    for key, count in summary["render_targets"].most_common(top):
        rb_mode, rb_surface, rb_depth, *targets = key
        print(
            f"  {count:6d}  rb_mode={rb_mode} rb_surface={rb_surface} "
            f"rb_depth={rb_depth} targets={','.join(targets)}"
        )

    print()
    print("Top render-state signatures:")
    for (color_mask, depth_control, pixel_targets, rt0_blend), count in summary[
        "render_states"
    ].most_common(top):
        print(
            f"  {count:6d}  color_mask={color_mask} depth={depth_control} "
            f"pixel_targets={pixel_targets} rt0_blend={rt0_blend}"
        )

    print()
    print("Top draw-effect signatures:")
    for (primitive_polygonal, rasterization, pixel_needed, om_writes), count in summary[
        "draw_effects"
    ].most_common(top):
        print(
            f"  {count:6d}  polygonal={primitive_polygonal} raster={rasterization} "
            f"pixel_needed={pixel_needed} om_writes={om_writes}"
        )

    print()
    print("Native replay support:")
    for support, count in summary["native_replay_support"].most_common(top):
        print(f"  {count:6d}  {support}")

    print()
    print("Top pass families:")
    for key, count in summary["pass_families"].most_common(top):
        (
            primitive,
            index_count,
            indexed,
            vs,
            ps,
            color_mask,
            depth_control,
            pixel_targets,
            rt0_blend,
            primitive_polygonal,
            rasterization,
            pixel_needed,
            om_writes,
            vertex_memexport_mask,
            pixel_memexport_mask,
            viz_query,
            vertex_fetch_count,
            texture_fetch_count,
        ) = key
        print(
            f"  {count:6d}  prim={primitive} indices={index_count} indexed={indexed} "
            f"vfetch={vertex_fetch_count} tfetch={texture_fetch_count} "
            f"color={color_mask} depth={depth_control} rt0_blend={rt0_blend} "
            f"raster={rasterization} pixel_needed={pixel_needed} om_writes={om_writes} "
            f"memexport={vertex_memexport_mask}/{pixel_memexport_mask} viz={viz_query} "
            f"VS={vs} PS={ps} pixel_targets={pixel_targets} polygonal={primitive_polygonal}"
        )

    if summary["vertex_fetches"]:
        print()
        print("Top vertex fetches:")
        for (fetch_constant, address, size, stride, attributes), count in summary[
            "vertex_fetches"
        ].most_common(top):
            print(
                f"  {count:6d}  fc={fetch_constant} address={address} size={size} "
                f"stride_words={stride} attrs={attributes}"
            )

    if summary["texture_fetches"]:
        print()
        print("Top texture fetches:")
        for (
            fetch_constant,
            base_address,
            mip_address,
            texture_format,
            dimension,
            width,
            height,
            depth,
            tiled,
        ), count in summary["texture_fetches"].most_common(top):
            print(
                f"  {count:6d}  fc={fetch_constant} base={base_address} mip={mip_address} "
                f"format={texture_format} dim={dimension} size={width}x{height}x{depth} "
                f"tiled={tiled}"
            )

    if summary["vertex_constants"]:
        print()
        print("Top vertex float constants:")
        for (constant_index, values, bits), count in summary["vertex_constants"].most_common(top):
            print(
                f"  {count:6d}  c{constant_index} values={','.join(values)} "
                f"bits={','.join(bits)}"
            )

    if summary["pixel_constants"]:
        print()
        print("Top pixel float constants:")
        for (constant_index, values, bits), count in summary["pixel_constants"].most_common(top):
            print(
                f"  {count:6d}  c{constant_index} values={','.join(values)} "
                f"bits={','.join(bits)}"
            )

    if summary["swap_draw_counts"]:
        print()
        print("First swaps:")
        for frame, draw_count, frontbuffer, width, height in summary["swap_draw_counts"][:top]:
            print(
                f"  frame={frame} draws={draw_count} frontbuffer={frontbuffer} "
                f"size={width}x{height}"
            )


def write_csv(summary: dict[str, Any], csv_path: Path) -> None:
    with csv_path.open("w", newline="", encoding="utf-8") as file:
        writer = csv.writer(file)
        writer.writerow(["kind", "key0", "key1", "key2", "count", "details"])

        for event_type, count in summary["event_types"].most_common():
            writer.writerow(["event", event_type, "", "", count, ""])

        for (vs, ps), count in summary["shader_pairs"].most_common():
            writer.writerow(["shader_pair", vs, ps, "", count, ""])

        for (primitive, name), count in summary["primitive_counts"].most_common():
            writer.writerow(["primitive", f"0x{primitive:02X}", name, "", count, ""])

        for (primitive, index_count, indexed, index_format, index_length), count in summary[
            "draw_shapes"
        ].most_common():
            writer.writerow(
                [
                    "draw_shape",
                    primitive,
                    str(index_count),
                    str(indexed),
                    count,
                    f"index_format={index_format};index_length={index_length}",
                ]
            )

        for ((vs, ps), info), count in summary["shader_infos"].most_common():
            (
                vs_dwords,
                ps_dwords,
                vertex_bindings,
                vertex_attributes,
                vertex_texture_bindings,
                pixel_texture_bindings,
                vertex_float_constants,
                pixel_float_constants,
                vertex_memexport_mask,
                pixel_memexport_mask,
            ) = info
            writer.writerow(
                [
                    "shader_requirements",
                    vs,
                    ps,
                    f"vfetch={vertex_bindings}/{vertex_attributes}",
                    count,
                    (
                        f"tfetch_vs={vertex_texture_bindings};tfetch_ps={pixel_texture_bindings};"
                        f"consts={vertex_float_constants}/{pixel_float_constants};"
                        f"ucode={vs_dwords}/{ps_dwords};"
                        f"memexport={vertex_memexport_mask}/{pixel_memexport_mask}"
                    ),
                ]
            )

        for (
            primitive,
            indexed,
            vs,
            ps,
            fetch_constant,
            stride_words,
            attribute_count,
            texture_fetch_count,
            support,
        ), count in summary["vertex_layouts"].most_common():
            writer.writerow(
                [
                    "vertex_layout",
                    primitive,
                    vs,
                    ps,
                    count,
                    (
                        f"indexed={indexed};fetch_constant={fetch_constant};"
                        f"stride_words={stride_words};attributes={attribute_count};"
                        f"texture_fetches={texture_fetch_count};support={support}"
                    ),
                ]
            )

        for (
            primitive,
            indexed,
            vs,
            ps,
            fetch_constant,
            stride_words,
            attribute_count,
            attribute_signature,
            support,
        ), count in summary["vertex_attribute_layouts"].most_common():
            writer.writerow(
                [
                    "vertex_attribute_signature",
                    primitive,
                    vs,
                    ps,
                    count,
                    (
                        f"indexed={indexed};fetch_constant={fetch_constant};"
                        f"stride_words={stride_words};attributes={attribute_count};"
                        f"support={support};signature="
                        f"{format_vertex_attribute_signature(attribute_signature)}"
                    ),
                ]
            )

        for key, count in summary["render_targets"].most_common():
            rb_mode, rb_surface, rb_depth, *targets = key
            writer.writerow(
                [
                    "render_target_signature",
                    rb_mode,
                    rb_surface,
                    rb_depth,
                    count,
                    "targets=" + ",".join(targets),
                ]
            )

        for (color_mask, depth_control, pixel_targets, rt0_blend), count in summary[
            "render_states"
        ].most_common():
            writer.writerow(
                [
                    "render_state_signature",
                    color_mask,
                    depth_control,
                    pixel_targets,
                    count,
                    f"rt0_blend={rt0_blend}",
                ]
            )

        for (primitive_polygonal, rasterization, pixel_needed, om_writes), count in summary[
            "draw_effects"
        ].most_common():
            writer.writerow(
                [
                    "draw_effect_signature",
                    f"polygonal={primitive_polygonal}",
                    f"raster={rasterization}",
                    f"pixel_needed={pixel_needed}",
                    count,
                    f"om_writes={om_writes}",
                ]
            )

        for support, count in summary["native_replay_support"].most_common():
            writer.writerow(["native_replay_support", support, "", "", count, ""])

        for key, count in summary["pass_families"].most_common():
            (
                primitive,
                index_count,
                indexed,
                vs,
                ps,
                color_mask,
                depth_control,
                pixel_targets,
                rt0_blend,
                primitive_polygonal,
                rasterization,
                pixel_needed,
                om_writes,
                vertex_memexport_mask,
                pixel_memexport_mask,
                viz_query,
                vertex_fetch_count,
                texture_fetch_count,
            ) = key
            writer.writerow(
                [
                    "pass_family",
                    primitive,
                    vs,
                    ps,
                    count,
                    (
                        f"indices={index_count};indexed={indexed};"
                        f"vfetch={vertex_fetch_count};tfetch={texture_fetch_count};"
                        f"color_mask={color_mask};depth={depth_control};"
                        f"pixel_targets={pixel_targets};rt0_blend={rt0_blend};"
                        f"polygonal={primitive_polygonal};raster={rasterization};"
                        f"pixel_needed={pixel_needed};om_writes={om_writes};"
                        f"memexport={vertex_memexport_mask}/{pixel_memexport_mask};"
                        f"viz_query={viz_query}"
                    ),
                ]
            )

        for (base_address, index_length, needed_bytes, index_format, endian, index_count), count in (
            summary["index_buffers"].most_common()
        ):
            writer.writerow(
                [
                    "index_buffer",
                    base_address,
                    str(index_count),
                    str(index_format),
                    count,
                    f"length={index_length};needed={needed_bytes};endian={endian}",
                ]
            )

        for (fetch_constant, address, size, stride, attributes), count in summary[
            "vertex_fetches"
        ].most_common():
            writer.writerow(
                [
                    "vertex_fetch",
                    str(fetch_constant),
                    address,
                    str(size),
                    count,
                    f"stride_words={stride};attributes={attributes}",
                ]
            )

        for (
            fetch_constant,
            base_address,
            mip_address,
            texture_format,
            dimension,
            width,
            height,
            depth,
            tiled,
        ), count in summary["texture_fetches"].most_common():
            writer.writerow(
                [
                    "texture_fetch",
                    str(fetch_constant),
                    base_address,
                    mip_address,
                    count,
                    (
                        f"format={texture_format};dimension={dimension};"
                        f"size={width}x{height}x{depth};tiled={tiled}"
                    ),
                ]
            )

        for (constant_index, values, bits), count in summary["vertex_constants"].most_common():
            writer.writerow(
                [
                    "vertex_float_constant",
                    f"c{constant_index}",
                    ",".join(values),
                    ",".join(bits),
                    count,
                    "",
                ]
            )

        for (constant_index, values, bits), count in summary["pixel_constants"].most_common():
            writer.writerow(
                [
                    "pixel_float_constant",
                    f"c{constant_index}",
                    ",".join(values),
                    ",".join(bits),
                    count,
                    "",
                ]
            )

        for frame, draw_count in summary["frame_draws"].most_common():
            writer.writerow(["frame_draws", str(frame), "", "", draw_count, ""])

        for frame, draw_count, frontbuffer, width, height in summary["swap_draw_counts"]:
            writer.writerow(
                [
                    "swap",
                    str(frame),
                    frontbuffer,
                    f"{width}x{height}",
                    draw_count,
                    "",
                ]
            )


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("events", type=Path, help="Path to native_render_events.jsonl")
    parser.add_argument("--top", type=int, default=24, help="Number of top rows to print")
    parser.add_argument("--csv", type=Path, help="Optional CSV output path")
    parser.add_argument("--strict", action="store_true", help="Fail on a truncated final JSONL line")
    args = parser.parse_args()

    try:
        summary, warnings = summarize_file(args.events, args.strict)
    except (OSError, ValueError) as exc:
        print(f"error: {exc}", file=sys.stderr)
        return 1

    for warning in warnings:
        print(f"warning: {warning}", file=sys.stderr)

    print_summary(args.events, summary, args.top)
    if args.csv:
        args.csv.parent.mkdir(parents=True, exist_ok=True)
        write_csv(summary, args.csv)
        print(f"\nWrote CSV: {args.csv}")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
