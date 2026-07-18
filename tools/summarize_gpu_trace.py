#!/usr/bin/env python3
"""Summarize ReXGlue/Xenia-style GPU trace files for renderer replacement work."""

from __future__ import annotations

import argparse
import collections
import csv
import os
import struct
import sys
from pathlib import Path


TRACE_COMMAND_NAMES = {
    0: "PrimaryBufferStart",
    1: "PrimaryBufferEnd",
    2: "IndirectBufferStart",
    3: "IndirectBufferEnd",
    4: "PacketStart",
    5: "PacketEnd",
    6: "MemoryRead",
    7: "MemoryWrite",
    8: "EdramSnapshot",
    9: "Event",
    10: "Registers",
    11: "GammaRamp",
}

PM4_NAMES = {
    0x10: "PM4_NOP",
    0x21: "PM4_REG_RMW",
    0x22: "PM4_DRAW_INDX",
    0x23: "PM4_VIZ_QUERY",
    0x25: "PM4_SET_STATE",
    0x26: "PM4_WAIT_FOR_IDLE",
    0x27: "PM4_IM_LOAD",
    0x2B: "PM4_IM_LOAD_IMMEDIATE",
    0x2C: "PM4_IM_STORE",
    0x2D: "PM4_SET_CONSTANT",
    0x2E: "PM4_LOAD_CONSTANT_CONTEXT",
    0x2F: "PM4_LOAD_ALU_CONSTANT",
    0x34: "PM4_DRAW_INDX_BIN",
    0x35: "PM4_DRAW_INDX_2_BIN",
    0x36: "PM4_DRAW_INDX_2",
    0x37: "PM4_INDIRECT_BUFFER_PFD",
    0x3B: "PM4_INVALIDATE_STATE",
    0x3C: "PM4_WAIT_REG_MEM",
    0x3D: "PM4_MEM_WRITE",
    0x3E: "PM4_REG_TO_MEM",
    0x3F: "PM4_INDIRECT_BUFFER",
    0x44: "PM4_COND_EXEC",
    0x45: "PM4_COND_WRITE",
    0x46: "PM4_EVENT_WRITE",
    0x48: "PM4_ME_INIT",
    0x4A: "PM4_SET_SHADER_BASES",
    0x4B: "PM4_SET_BIN_BASE_OFFSET",
    0x4F: "PM4_MEM_WRITE_CNTR",
    0x50: "PM4_SET_BIN_MASK",
    0x51: "PM4_SET_BIN_SELECT",
    0x52: "PM4_WAIT_REG_EQ",
    0x53: "PM4_WAIT_REG_GTE",
    0x54: "PM4_INTERRUPT",
    0x55: "PM4_SET_CONSTANT2",
    0x56: "PM4_SET_SHADER_CONSTANTS",
    0x58: "PM4_EVENT_WRITE_SHD",
    0x59: "PM4_EVENT_WRITE_CFL",
    0x5A: "PM4_EVENT_WRITE_EXT",
    0x5B: "PM4_EVENT_WRITE_ZPD",
    0x5C: "PM4_WAIT_UNTIL_READ",
    0x5D: "PM4_WAIT_IB_PFD_COMPLETE",
    0x5E: "PM4_CONTEXT_UPDATE",
    0x60: "PM4_SET_BIN_MASK_LO",
    0x61: "PM4_SET_BIN_MASK_HI",
    0x62: "PM4_SET_BIN_SELECT_LO",
    0x63: "PM4_SET_BIN_SELECT_HI",
    0x64: "PM4_XE_SWAP",
}

DRAW_OPCODES = {0x22, 0x34, 0x35, 0x36}


class TraceFormatError(RuntimeError):
    pass


def read_exact(file, length: int, label: str) -> bytes:
    data = file.read(length)
    if len(data) != length:
        raise TraceFormatError(f"unexpected EOF while reading {label}")
    return data


def skip_exact(file, length: int, label: str) -> None:
    if length <= 0:
        return
    current = file.tell()
    file.seek(length, os.SEEK_CUR)
    if file.tell() != current + length:
        raise TraceFormatError(f"unable to skip {length} bytes for {label}")


def parse_packet(packet_bytes: bytes) -> dict[str, int | str]:
    if len(packet_bytes) < 4:
        return {"packet_type": -1, "opcode": -1, "name": "truncated"}

    packet = struct.unpack_from(">I", packet_bytes, 0)[0]
    if packet == 0:
        return {"packet_type": 2, "opcode": -1, "name": "TYPE2_NOP"}

    packet_type = packet >> 30
    if packet_type == 3:
        opcode = (packet >> 8) & 0x7F
        return {
            "packet_type": packet_type,
            "opcode": opcode,
            "name": PM4_NAMES.get(opcode, f"PM4_UNKNOWN_0x{opcode:02X}"),
        }
    return {"packet_type": packet_type, "opcode": -1, "name": f"TYPE{packet_type}"}


def summarize_trace(path: Path) -> dict:
    counters = collections.Counter()
    packet_types = collections.Counter()
    opcodes = collections.Counter()
    memory_encoded = collections.Counter()
    memory_decoded = collections.Counter()
    register_ranges = collections.Counter()
    first_packets: list[tuple[int, str, int, int]] = []

    with path.open("rb") as file:
        header = read_exact(file, 48, "trace header")
        version = struct.unpack_from("<I", header, 0)[0]
        build = header[4:44].split(b"\0", 1)[0].decode("ascii", errors="replace")
        title_id = struct.unpack_from("<I", header, 44)[0]

        while True:
            type_bytes = file.read(4)
            if not type_bytes:
                break
            if len(type_bytes) != 4:
                raise TraceFormatError("truncated command type")
            command_type = struct.unpack("<I", type_bytes)[0]
            counters[command_type] += 1

            if command_type == 0:  # PrimaryBufferStart
                read_exact(file, 8, "primary buffer start")
            elif command_type == 1:  # PrimaryBufferEnd
                pass
            elif command_type == 2:  # IndirectBufferStart
                read_exact(file, 8, "indirect buffer start")
            elif command_type == 3:  # IndirectBufferEnd
                pass
            elif command_type == 4:  # PacketStart
                rest = read_exact(file, 8, "packet start")
                base_ptr, count = struct.unpack("<II", rest)
                packet_bytes = read_exact(file, count * 4, "packet payload")
                info = parse_packet(packet_bytes)
                packet_types[info["packet_type"]] += 1
                if info["opcode"] >= 0:
                    opcodes[(info["opcode"], info["name"])] += 1
                    if info["opcode"] in DRAW_OPCODES:
                        counters["draw_packets"] += 1
                    elif info["opcode"] == 0x64:
                        counters["swap_packets"] += 1
                if len(first_packets) < 64:
                    first_packets.append((base_ptr, str(info["name"]), int(info["packet_type"]), count))
            elif command_type == 5:  # PacketEnd
                pass
            elif command_type in (6, 7):  # MemoryRead / MemoryWrite
                rest = read_exact(file, 16, "memory command")
                base_ptr, encoding, encoded_len, decoded_len = struct.unpack("<IIII", rest)
                key = TRACE_COMMAND_NAMES[command_type]
                memory_encoded[key] += encoded_len
                memory_decoded[key] += decoded_len
                skip_exact(file, encoded_len, "memory payload")
            elif command_type == 8:  # EdramSnapshot
                rest = read_exact(file, 8, "edram snapshot")
                encoding, encoded_len = struct.unpack("<II", rest)
                memory_encoded["EdramSnapshot"] += encoded_len
                skip_exact(file, encoded_len, "edram snapshot payload")
            elif command_type == 9:  # Event
                event_type = struct.unpack("<I", read_exact(file, 4, "event"))[0]
                if event_type == 0:
                    counters["swap_events"] += 1
            elif command_type == 10:  # RegistersCommand, padded to 24 bytes total.
                rest = read_exact(file, 20, "registers command")
                first_register, register_count = struct.unpack_from("<II", rest, 0)
                encoded_len = struct.unpack_from("<I", rest, 16)[0]
                register_ranges[(first_register, register_count)] += 1
                memory_encoded["Registers"] += encoded_len
                skip_exact(file, encoded_len, "register payload")
            elif command_type == 11:  # GammaRampCommand, padded to 16 bytes total.
                rest = read_exact(file, 12, "gamma ramp command")
                encoded_len = struct.unpack_from("<I", rest, 8)[0]
                memory_encoded["GammaRamp"] += encoded_len
                skip_exact(file, encoded_len, "gamma ramp payload")
            else:
                raise TraceFormatError(f"unknown command type {command_type} at offset 0x{file.tell() - 4:X}")

    return {
        "path": path,
        "version": version,
        "build": build,
        "title_id": title_id,
        "commands": counters,
        "packet_types": packet_types,
        "opcodes": opcodes,
        "memory_encoded": memory_encoded,
        "memory_decoded": memory_decoded,
        "register_ranges": register_ranges,
        "first_packets": first_packets,
    }


def print_summary(summary: dict, top: int) -> None:
    print(f"Trace: {summary['path']}")
    print(f"Version: {summary['version']}  Build: {summary['build']}  Title: 0x{summary['title_id']:08X}")
    print()

    commands = summary["commands"]
    print("Commands:")
    for key, count in commands.most_common():
        name = TRACE_COMMAND_NAMES.get(key, str(key)) if isinstance(key, int) else key
        print(f"  {name}: {count}")
    print()

    print("Packet types:")
    for key, count in summary["packet_types"].most_common():
        print(f"  TYPE{key}: {count}")
    print()

    print(f"Top PM4 opcodes:")
    for (opcode, name), count in summary["opcodes"].most_common(top):
        print(f"  0x{opcode:02X} {name}: {count}")
    print()

    print("Memory traffic:")
    for key in sorted(set(summary["memory_encoded"]) | set(summary["memory_decoded"])):
        encoded = summary["memory_encoded"][key]
        decoded = summary["memory_decoded"][key]
        if decoded:
            print(f"  {key}: encoded={encoded} decoded={decoded}")
        else:
            print(f"  {key}: encoded={encoded}")
    print()

    print("Top register ranges:")
    for (first, count), hits in summary["register_ranges"].most_common(top):
        print(f"  first=0x{first:04X} count={count}: {hits}")


def write_csv(summary: dict, csv_path: Path) -> None:
    with csv_path.open("w", newline="", encoding="utf-8") as file:
        writer = csv.writer(file)
        writer.writerow(["kind", "id", "name", "count"])
        for key, count in summary["commands"].most_common():
            name = TRACE_COMMAND_NAMES.get(key, str(key)) if isinstance(key, int) else key
            writer.writerow(["command", key, name, count])
        for (opcode, name), count in summary["opcodes"].most_common():
            writer.writerow(["pm4", f"0x{opcode:02X}", name, count])
        for (first, register_count), hits in summary["register_ranges"].most_common():
            writer.writerow(["register_range", f"0x{first:04X}", register_count, hits])


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("trace", type=Path, help="Path to .xtr GPU trace")
    parser.add_argument("--top", type=int, default=24, help="Number of top rows to print")
    parser.add_argument("--csv", type=Path, help="Optional CSV output path")
    args = parser.parse_args()

    try:
        summary = summarize_trace(args.trace)
    except (OSError, TraceFormatError, struct.error) as exc:
        print(f"error: {exc}", file=sys.stderr)
        return 1

    print_summary(summary, args.top)
    if args.csv:
        write_csv(summary, args.csv)
        print(f"\nWrote CSV: {args.csv}")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
