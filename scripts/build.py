#!/usr/bin/env python3
import os
import sys
import shutil
import subprocess
from pathlib import Path

ROOT = Path(__file__).resolve().parent.parent
SRC = ROOT / "src"
LOADER = SRC / "wyze-loader"
HIJACK = SRC / "wyze-hijack-ap"
WEBFW = SRC / "webflasher" / "firmware"


def idf(args, cwd):
    idf_path = os.environ.get("IDF_PATH")
    if idf_path:
        cmd = [sys.executable, str(Path(idf_path) / "tools" / "idf.py")] + args
    else:
        exe = shutil.which("idf.py")
        if not exe:
            sys.exit("ERROR: idf.py not found. Run this inside an ESP-IDF environment "
                     "(run export.sh / export.bat first).")
        cmd = [exe] + args
    print(">>", " ".join(str(c) for c in cmd), " (cwd=%s)" % cwd)
    subprocess.run(cmd, cwd=str(cwd), check=True)


def rmbuild(proj):
    b = proj / "build"
    if b.exists():
        print(">> rm", b)
        shutil.rmtree(b, ignore_errors=True)


def copy(src, dst):
    if not src.exists():
        sys.exit("ERROR: expected artifact missing: %s" % src)
    dst.parent.mkdir(parents=True, exist_ok=True)
    shutil.copy2(src, dst)
    print(">> copy", src, "->", dst)


def build_project(proj, target):
    rmbuild(proj)
    idf(["set-target", target], proj)
    idf(["build"], proj)


def main():
    build_project(LOADER, "esp32c3")
    copy(LOADER / "build" / "wyze_loader.bin", HIJACK / "payload.bin")

    build_project(HIJACK, "esp32s3")
    copy(HIJACK / "build" / "bootloader" / "bootloader.bin", WEBFW / "bootloader.bin")
    copy(HIJACK / "build" / "partition_table" / "partition-table.bin", WEBFW / "partition-table.bin")
    copy(HIJACK / "build" / "wyze-hijack_ap.bin", WEBFW / "wyze-hijack_ap.bin")

    print("\nBuild complete. Commit src/webflasher to publish via GitHub Pages.")


if __name__ == "__main__":
    main()
