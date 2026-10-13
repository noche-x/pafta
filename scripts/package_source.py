#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-2.0-only
"""Package Pafta and the dependency sources used by a configured build."""
import argparse
import io
from pathlib import Path
import subprocess
import tarfile

DEPENDENCIES = ("raylib", "imgui", "rlimgui", "tinygltf", "nfd")


def dependency_file(item: tarfile.TarInfo):
    if ".git" in Path(item.name).parts or Path(item.name).name == ".DS_Store":
        return None
    return item


def package_source(repo: Path, build: Path, output: Path) -> None:
    cache = {}
    for line in (build / "CMakeCache.txt").read_text().splitlines():
        if line and not line.startswith(("#", "//")) and "=" in line:
            key, value = line.split("=", 1)
            cache[key.split(":", 1)[0]] = value
    sources = {}
    for name in DEPENDENCIES:
        override = cache.get("FETCHCONTENT_SOURCE_DIR_" + name.upper(), "")
        source = Path(override) if override else build / "_deps" / (name + "-src")
        if not source.is_dir():
            raise RuntimeError(f"Missing dependency source: {source}")
        sources[name] = source
    # Include new files too, but not Git history or ignored maps and builds.
    files = subprocess.check_output(
        ["git", "ls-files", "--cached", "--others", "--exclude-standard", "-z"], cwd=repo
    ).decode().split("\0")
    revision = subprocess.check_output(["git", "rev-parse", "HEAD"], cwd=repo).decode().strip()
    status = subprocess.check_output(["git", "status", "--porcelain"], cwd=repo)
    output.parent.mkdir(parents=True, exist_ok=True)
    try:
        with tarfile.open(output, "w:gz") as archive:
            for name in sorted(set(files) - {""}):
                path = repo / name
                if path.exists() or path.is_symlink():
                    archive.add(path, arcname="pafta/" + name, recursive=False)
            for name, source in sources.items():
                archive.add(source, arcname="pafta/third_party/" + name,
                            filter=dependency_file)
            metadata = f"Base revision: {revision}\nWorking tree changes: {bool(status)}\n"
            data = metadata.encode()
            info = tarfile.TarInfo("pafta/SOURCE_REVISION.txt")
            info.size = len(data)
            archive.addfile(info, io.BytesIO(data))
    except BaseException:
        output.unlink(missing_ok=True)
        raise


if __name__ == "__main__":
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("build", type=Path, help="configured native build directory")
    parser.add_argument("output", type=Path, help="output .tar.gz archive")
    args = parser.parse_args()
    package_source(Path(__file__).resolve().parents[1], args.build.resolve(), args.output.resolve())
    print(args.output)
