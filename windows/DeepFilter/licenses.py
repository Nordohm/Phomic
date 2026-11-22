"""Writes THIRD-PARTY.txt: the licenses of the Rust crates built into phomic_deepfilter.lib (and so into
Phomic.exe), which the build copies to licenses/deepfilternet/. Run it after Cargo.lock changes:

    python windows/DeepFilter/licenses.py

It asks cargo which crates the library links (normal dependencies for x64 Windows; not build scripts'
or macros', which don't end up in the program) and copies each one's license files from cargo's
download folder, so run `cargo build` (or build Phomic) first.
"""

import json
import os
import pathlib
import subprocess
import sys

HERE = pathlib.Path(__file__).resolve().parent
TARGET = "x86_64-pc-windows-msvc"
NAMES = ("LICENSE", "LICENCE", "COPYING", "NOTICE", "UNLICENSE")


def cargo() -> str:
    home = pathlib.Path(os.environ.get("USERPROFILE", "~")).expanduser() / ".cargo" / "bin" / "cargo.exe"
    return str(home) if home.exists() else "cargo"


def main() -> int:
    meta = json.loads(subprocess.run(
        [cargo(), "metadata", "--format-version", "1", "--locked", "--filter-platform", TARGET,
         "--manifest-path", str(HERE / "Cargo.toml")],
        check=True, capture_output=True, text=True, encoding="utf-8").stdout)
    packages = {p["id"]: p for p in meta["packages"]}
    nodes = {n["id"]: n for n in meta["resolve"]["nodes"]}
    root = meta["resolve"]["root"]

    def is_macro(pkg) -> bool:
        return any("proc-macro" in t["kind"] for t in pkg["targets"])

    linked, todo = set(), [root]
    while todo:
        node = nodes[todo.pop()]
        for dep in node["deps"]:
            normal = any(k["kind"] is None for k in dep["dep_kinds"])
            if normal and dep["pkg"] not in linked and not is_macro(packages[dep["pkg"]]):
                linked.add(dep["pkg"])
                todo.append(dep["pkg"])

    out = [
        "Rust crates built into Phomic's noise suppression (DeepFilterNet), with their licenses.",
        "DeepFilterNet's own license is in this folder too (LICENSE, LICENSE-MIT, LICENSE-APACHE).",
        "",
    ]
    missing = []
    seen: dict[str, str] = {}
    for pkg in sorted((packages[i] for i in linked), key=lambda p: (p["name"], p["version"])):
        if pkg["source"] is None:  # DeepFilterNet itself (a path in this repository)
            continue
        folder = pathlib.Path(pkg["manifest_path"]).parent
        files = sorted(f for f in folder.iterdir() if f.is_file() and f.name.upper().startswith(NAMES))
        out += ["=" * 78, f"{pkg['name']} {pkg['version']}", f"License: {pkg.get('license') or pkg.get('license_file')}"]
        if pkg.get("repository"):
            out.append(f"Source: {pkg['repository']}")
        out.append("")
        if not files:
            missing.append(f"{pkg['name']} {pkg['version']}")
            out += ["(The crate ships no license file; its license is the one named above.)", ""]
        for f in files:
            text = f.read_text(encoding="utf-8", errors="replace").strip()
            if text in seen:  # word for word the same as one above (the Apache license, mostly)
                out += [f"--- {f.name}: the same text as {seen[text]} above ---", ""]
            else:
                seen[text] = f"{pkg['name']} {pkg['version']}'s {f.name}"
                out += [f"--- {f.name} ---", text, ""]
    (HERE / "THIRD-PARTY.txt").write_text("\n".join(out) + "\n", encoding="utf-8", newline="\n")
    print(f"{len(linked) - sum(packages[i]['source'] is None for i in linked)} crates; no license file: {', '.join(missing) or 'none'}")
    return 0


if __name__ == "__main__":
    sys.exit(main())
