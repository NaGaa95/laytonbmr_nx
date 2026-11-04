#!/usr/bin/env python3
"""Run the real input hook and HID feed code with host-side platform mocks."""
import argparse
import os
from pathlib import Path
import shutil
import subprocess
import tempfile


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--cc", default=os.environ.get("CC", "cc"))
    parser.add_argument("--source-dir", type=Path,
                        default=Path(__file__).resolve().parents[1] / "source")
    args = parser.parse_args()
    compiler = shutil.which(args.cc)
    if compiler is None:
        parser.error("C compiler not found; supply --cc /path/to/clang-or-gcc")
    source = args.source_dir.resolve()
    feed = (source / "android_native_unity.c").read_text(encoding="utf-8")
    # The HID section is self-contained at the end of this platform file.
    # Compile it verbatim; only libnx calls and types are mocked in the test.
    marker = "static PadState g_pad;"
    if feed.count(marker) != 1:
        raise RuntimeError("Cannot locate the HID section to test")
    with tempfile.TemporaryDirectory(prefix="layton-input-") as directory:
        temporary = Path(directory)
        (temporary / "hid_feed.inc").write_text(
            feed[feed.index(marker):], encoding="utf-8")
        executable = temporary / ("input_test.exe" if os.name == "nt" else "input_test")
        subprocess.run([compiler, "-std=c11", "-Wall", "-Wextra", "-Werror", "-O2",
                        "-I", str(source), "-I", str(temporary),
                        str(Path(__file__).with_name("input_test.c")),
                        "-o", str(executable)], check=True)
        subprocess.run([str(executable)], check=True)


if __name__ == "__main__":
    main()
