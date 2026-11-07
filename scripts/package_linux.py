#!/usr/bin/env python3
"""Package an existing Linux build; never build, install services or open audio."""

import argparse
import hashlib
import json
import os
from pathlib import Path
import re
import shutil
import struct
import subprocess
import tarfile
import tempfile


def run(*args, cwd=None):
    return subprocess.check_output(args, cwd=cwd).decode().strip()


def sha(path):
    return hashlib.sha256(path.read_bytes()).hexdigest()


def snapshot(root):
    names = subprocess.check_output(
        ["git", "ls-files", "-z", "--cached", "--others", "--exclude-standard"], cwd=root
    ).split(b"\0")
    files = {}
    for raw in sorted(set(names)):
        if not raw:
            continue
        name = os.fsdecode(raw)
        path = root / name
        if path.is_symlink():
            files[name] = {"link": os.readlink(path)}
        elif path.is_file():
            files[name] = {"sha256": sha(path), "executable": bool(path.stat().st_mode & 0o111)}
        elif not path.exists():
            files[name] = {"deleted": True}
    child = root / "vendor/zcore"
    if run("git", "status", "--porcelain", cwd=child):
        raise RuntimeError("zCore checkout must be clean")
    result = {
        "head": run("git", "rev-parse", "HEAD", cwd=root),
        "zcore": run("git", "rev-parse", "HEAD", cwd=child),
        "files": files,
    }
    result["snapshotSha256"] = hashlib.sha256(
        json.dumps(result, sort_keys=True, separators=(",", ":")).encode()
    ).hexdigest()
    return result


def elf_arch(path):
    data = path.read_bytes()[:20]
    if len(data) < 20 or data[:5] != b"\x7fELF\x02" or data[5] != 1:
        raise RuntimeError(f"Expected a little-endian ELF64 Linux executable: {path}")
    machine = struct.unpack_from("<H", data, 18)[0]
    if machine not in (62, 183):
        raise RuntimeError(f"Unsupported ELF machine {machine}")
    return {62: "amd64", 183: "arm64"}[machine]


def debian_dependencies(binaries, scratch, name, arch):
    """Resolve this split's ELF symbol requirements against the packaging target."""
    if not shutil.which("dpkg-shlibdeps"):
        raise RuntimeError("Debian packaging requires dpkg-dev (dpkg-shlibdeps)")
    if run("dpkg", "--print-architecture") != arch:
        raise RuntimeError("Derive Debian dependencies on the native target architecture")
    fixture = scratch / (name + "-shlibdeps")
    control = fixture / "debian/control"
    control.parent.mkdir(parents=True)
    control.write_text(
        "Source: loopz-preview\nSection: sound\nPriority: optional\n"
        "Maintainer: LoopZ developers\nStandards-Version: 4.7.0\n\n"
        f"Package: {name}\nArchitecture: any\nDescription: LoopZ dependency scan fixture\n"
    )
    result = subprocess.run(
        ["dpkg-shlibdeps", "-O", *["-e" + str(p) for p in sorted(binaries)]],
        cwd=fixture,
        capture_output=True,
        text=True,
    )
    if result.returncode:
        raise RuntimeError("Cannot derive Debian ELF dependencies: " + result.stderr.strip())
    dependencies = [
        line.removeprefix("shlibs:Depends=").strip()
        for line in result.stdout.splitlines()
        if line.startswith("shlibs:Depends=")
    ]
    if len(dependencies) != 1 or not dependencies[0]:
        raise RuntimeError("dpkg-shlibdeps did not report nonempty ELF dependencies")
    return dependencies[0]


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--source", type=Path, default=Path(__file__).resolve().parents[1])
    parser.add_argument("--build", type=Path)
    parser.add_argument("--output", type=Path)
    parser.add_argument("--version", default="0.2.3~linux-preview1")
    parser.add_argument("--format", choices=("tar", "deb", "both"), default="tar")
    parser.add_argument("--source-receipt", type=Path)
    parser.add_argument("--write-source-receipt", type=Path)
    args = parser.parse_args()
    root = args.source.resolve()
    before = snapshot(root)
    if args.write_source_receipt:
        args.write_source_receipt.write_text(json.dumps(before, indent=2) + "\n")
        return
    if not args.build or not args.output or not args.source_receipt:
        parser.error("packaging requires --build, --output and --source-receipt")
    if not re.fullmatch(r"[0-9][A-Za-z0-9.+~:-]*", args.version):
        parser.error("invalid package version")
    frozen = json.loads(args.source_receipt.read_text())
    if frozen != before:
        raise RuntimeError("Source changed since pre-build receipt; rebuild from a fresh snapshot")
    build, output = args.build.resolve(), args.output.resolve()
    cache = (build / "CMakeCache.txt").read_text()
    if "CMAKE_INSTALL_PREFIX:PATH=/usr\n" not in cache:
        raise RuntimeError("Configure this build with -DCMAKE_INSTALL_PREFIX=/usr")
    home = re.search(r"^CMAKE_HOME_DIRECTORY:INTERNAL=(.*)$", cache, re.M)
    if not home or Path(home.group(1)).resolve() != root:
        raise RuntimeError("Build directory belongs to another source checkout")
    pin = re.search(r"^ZCORE_ROOT:[^=]*=(.*)$", cache, re.M)
    if pin and Path(pin.group(1)).resolve() != root / "vendor/zcore":
        raise RuntimeError("Build uses a different zCore checkout")
    config = re.search(r"^CMAKE_BUILD_TYPE:STRING=(.*)$", cache, re.M)
    output.mkdir(parents=True, exist_ok=True)
    if any(output.iterdir()):
        raise RuntimeError("Package output must be an empty task-owned directory")
    with tempfile.TemporaryDirectory(prefix="loopz-package-") as scratch:
        scratch = Path(scratch)
        installed = scratch / "installed"
        env = dict(os.environ, DESTDIR=str(installed))
        subprocess.run(["cmake", "--install", str(build)], env=env, check=True)
        engine = [
            "usr/bin/loopz-engine",
            "usr/bin/loopzctl",
            "usr/lib/systemd/user/loopz-engine.service",
        ]
        desktop = [
            "usr/bin/loopz-desktop",
            "usr/share/loopz/interface",
            "usr/share/applications/loopz.desktop",
        ]
        parts = [("loopz-engine", engine)]
        if (installed / desktop[0]).exists():
            parts.append(("loopz-desktop", desktop))
        arch = elf_arch(installed / engine[0])
        for name, paths in parts:
            stage = scratch / name
            for relative in paths:
                source, target = installed / relative, stage / relative
                if not source.exists():
                    raise RuntimeError(f"Missing installed payload: {relative}")
                target.parent.mkdir(parents=True, exist_ok=True)
                if source.is_dir():
                    shutil.copytree(source, target, symlinks=True)
                else:
                    shutil.copy2(source, target)
            binaries = [p for p in (stage / "usr/bin").iterdir() if p.is_file()]
            if any(elf_arch(p) != arch for p in binaries):
                raise RuntimeError("Mixed executable architectures")
            dependencies = None
            if args.format in ("deb", "both"):
                dependencies = debian_dependencies(binaries, scratch, name, arch)
                if name == "loopz-desktop":
                    dependencies = f"loopz-engine (= {args.version}), {dependencies}"
            manifest = {
                str(p.relative_to(stage)): sha(p) for p in sorted(stage.rglob("*")) if p.is_file()
            }
            receipt = {
                "package": name,
                "version": args.version,
                "architecture": arch,
                "configuration": config.group(1) if config else None,
                "source": before,
                "payloadSha256": manifest,
                "debianDepends": dependencies,
                "dependencySource": (
                    "dpkg-shlibdeps target symbol metadata" if dependencies else None
                ),
                "provenanceScope": "Pre-build source snapshot verified unchanged at packaging; binary hashes identify installed payload.",
            }
            receipt_dir = stage / "usr/share/doc" / name
            receipt_dir.mkdir(parents=True, exist_ok=True)
            (receipt_dir / "provenance.json").write_text(json.dumps(receipt, indent=2) + "\n")
            stem = f"{name}_{args.version}_{arch}"
            if args.format in ("tar", "both"):
                with tarfile.open(output / f"{stem}.tar.gz", "w:gz") as archive:
                    for entry in sorted(stage.iterdir()):
                        archive.add(entry, arcname=entry.name)
            if args.format in ("deb", "both"):
                control = stage / "DEBIAN"
                control.mkdir()
                (control / "control").write_text(
                    f"Package: {name}\nVersion: {args.version}\nArchitecture: {arch}\n"
                    f"Maintainer: LoopZ developers\nDepends: {dependencies}\n"
                    "Section: sound\nPriority: optional\nDescription: LoopZ Linux development preview\n"
                )
                subprocess.run(
                    [
                        "dpkg-deb",
                        "--root-owner-group",
                        "--build",
                        str(stage),
                        str(output / f"{stem}.deb"),
                    ],
                    check=True,
                )
        if snapshot(root) != before:
            raise RuntimeError("Source changed while packaging; discard generated packages")
    artifacts = sorted(output.glob(f"*_{args.version}_{arch}.*"))
    (output / "SHA256SUMS").write_text("".join(f"{sha(p)}  {p.name}\n" for p in artifacts))


if __name__ == "__main__":
    main()
