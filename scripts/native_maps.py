"""Bounded parsing of one process's NUL-delimited lsof file inventory."""

import re

MAX_OUTPUT = 8 * 1024 * 1024


def mapping_records(encoded, pid):
    if len(encoded) > MAX_OUTPUT:
        raise RuntimeError("Mapped-image inventory exceeds its size limit")
    records, row, process = [], None, False
    for raw in encoded.split(b"\0"):
        raw = raw.lstrip(b"\n")
        if not raw:
            continue
        key, value = chr(raw[0]), raw[1:].decode("utf-8", errors="strict")
        if key == "p":
            if process or value != str(pid):
                raise RuntimeError("Mapping inventory contains an ambiguous process")
            process = True
        elif key == "f":
            if not process:
                raise RuntimeError("Mapping record lacks its process header")
            row = {"f": value}
            records.append(row)
        elif row is not None:
            if key in row:
                raise RuntimeError("Mapping record repeats a field")
            row[key] = value
    if not process or not records:
        raise RuntimeError("Mapped-image inventory is incomplete")
    return records


def mapped_identity(row):
    device, inode = row.get("D", ""), row.get("i", "")
    if not re.fullmatch(r"(?:0x)?[0-9a-fA-F]{1,16}", device) or not re.fullmatch(
        r"[0-9]{1,20}", inode
    ):
        raise RuntimeError("Executable mapping lacks a device/inode identity")
    return int(device, 16), int(inode)
