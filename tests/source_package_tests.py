# SPDX-License-Identifier: GPL-2.0-only
"""Check source artifacts contain build inputs and omit local Git/build state."""
import importlib.util
from pathlib import Path
import subprocess
import tarfile
import tempfile
import unittest

spec = importlib.util.spec_from_file_location("package_source", Path(__file__).parents[1] / "scripts/package_source.py")
module = importlib.util.module_from_spec(spec)
spec.loader.exec_module(module)


class SourcePackageTests(unittest.TestCase):
    def test_archive_contents_and_missing_dependency(self):
        with tempfile.TemporaryDirectory() as directory:
            repo = Path(directory)
            def git(*args):
                return subprocess.run(["git", *args], cwd=repo, check=True, capture_output=True)
            git("init")
            (repo / ".gitignore").write_text("build/\ndist/\n")
            (repo / "LICENSE").write_text("test license")
            (repo / "deleted.cpp").write_text("// tracked then removed")
            git("add", ".gitignore", "LICENSE", "deleted.cpp")
            git("-c", "user.name=Test", "-c", "user.email=test@example.invalid", "commit", "-m", "fixture")
            (repo / "deleted.cpp").unlink()
            # New source files must be included even before they are committed.
            (repo / "CMakeLists.txt").write_text("cmake_minimum_required(VERSION 3.24)")
            build = repo / "build"
            build.mkdir()
            (build / "CMakeCache.txt").write_text("")
            (build / "private-build-log").write_text("must not ship")
            for name in module.DEPENDENCIES:
                source = build / "_deps" / (name + "-src")
                source.mkdir(parents=True)
                (source / "library.cpp").write_text("// dependency source")
                (source / ".git").mkdir()
                (source / ".git" / "config").write_text("private history")
            output = repo / "dist" / "pafta-source.tar.gz"
            module.package_source(repo, build, output)
            with tarfile.open(output) as archive:
                names = archive.getnames()
                self.assertIn("pafta/LICENSE", names)
                self.assertNotIn("pafta/deleted.cpp", names)
                self.assertIn("pafta/CMakeLists.txt", names)
                self.assertIn("pafta/SOURCE_REVISION.txt", names)
                for name in module.DEPENDENCIES:
                    self.assertIn(f"pafta/third_party/{name}/library.cpp", names)
                self.assertFalse(any(".git" in Path(name).parts or "/build/" in name or "/dist/" in name for name in names))
            missing = build / "_deps" / "nfd-src"
            missing.rename(missing.with_name("missing-nfd"))
            with self.assertRaisesRegex(RuntimeError, "Missing dependency source"):
                module.package_source(repo, build, repo / "dist" / "invalid.tar.gz")
            self.assertFalse((repo / "dist" / "invalid.tar.gz").exists())


if __name__ == "__main__":
    unittest.main()
