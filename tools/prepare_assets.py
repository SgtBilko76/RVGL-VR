#!/usr/bin/env python3
"""Copy RVGL's own assets (rvgl-base/distrib/assets) into the APK asset dir and
write assets_list.txt in the format of rvgl-android/update_assets.sh
(sorted relative paths, directories with a trailing slash).

Original game data (game_files, ost) is never bundled; push it to /sdcard/RVGL.
"""
import pathlib
import shutil
import sys

ROOT = pathlib.Path(__file__).resolve().parent.parent
SRC = ROOT / "rvgl-base" / "distrib" / "assets"
out = pathlib.Path(sys.argv[1]) if len(sys.argv) > 1 else ROOT / "build" / "generated" / "rvglassets"

if out.exists():
    shutil.rmtree(out)
shutil.copytree(SRC, out, ignore=shutil.ignore_patterns(".git*"))

entries = []
for p in out.rglob("*"):
    rel = p.relative_to(out).as_posix()
    if "assets_list" in rel:
        continue
    entries.append(rel + "/" if p.is_dir() else rel)
(out / "assets_list.txt").write_text("\n".join(sorted(entries)) + "\n")
print(f"{len(entries)} asset entries -> {out}")
