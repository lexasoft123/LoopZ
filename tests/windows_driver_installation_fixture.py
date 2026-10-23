"""Compile the actual driver installation methods against fault-injected PortCls doubles."""

from pathlib import Path
import re
import sys


def extract(path, name):
    source = path.read_text()
    starts = list(re.finditer(r"^" + re.escape(name) + r"\(", source, re.MULTILINE))
    if len(starts) != 1:
        raise RuntimeError(f"Ambiguous driver method {name}")
    start = starts[0].start()
    end = re.search(r"^}\s*$", source[start:], re.MULTILINE)
    if end is None:
        raise RuntimeError(f"Missing driver method end {name}")
    return "NTSTATUS\n" + source[start : start + end.end()] + "\n"


common, adapter, output = map(Path, sys.argv[1:])
output.write_text(
    extract(common, "CAdapterCommon::InstallEndpointFilters")
    + extract(adapter, "InstallEndpointRenderFilters")
)
