#!/usr/bin/env python3
"""Collect the installed dependency notices and licenses for the bundled Qt release."""
import json
import pathlib
import plistlib
import re
import shutil
import subprocess
import sys
import urllib.request


def bundled_qt_version(app):
    info = app / "Contents/Frameworks/QtCore.framework/Resources/Info.plist"
    with info.open("rb") as source:
        version = plistlib.load(source)["CFBundleVersion"]
    if not re.fullmatch(r"\d+\.\d+\.\d+(?:-(?:alpha|beta|rc)\d+)?", version):
        raise RuntimeError(f"Cannot identify an exact Qt source tag from bundled version: {version}")
    return version


def collect(app):
    app = app.resolve()
    destination = app / "Contents/Resources/third-party"
    destination.mkdir(parents=True, exist_ok=True)
    root = pathlib.Path(__file__).resolve().parent.parent
    shutil.copy2(root / "THIRD_PARTY.md", destination / "README.md")
    data = json.loads(subprocess.check_output(["brew", "info", "--json=v2", "--installed"], text=True))
    cellar = pathlib.Path(subprocess.check_output(["brew", "--cellar"], text=True).strip())
    license_summary = []
    bundled = {path.name for path in (app / "Contents/Frameworks").rglob("*") if path.is_file()}
    for formula in data["formulae"]:
        installed = formula.get("installed", [])
        version = formula.get("linked_keg") or (installed[-1]["version"] if installed else None)
        if not version:
            continue
        formula_root = cellar / formula["name"] / version
        if not formula_root.is_dir():
            continue
        libraries = [path.name for path in (formula_root / "lib").glob("*.dylib")]
        if not formula["name"].startswith("qt") and not any(name in bundled for name in libraries):
            continue
        # linked_keg includes Homebrew revisions; formula versions.stable may
        # already describe a newer release than the binary we actually copied.
        license_summary.append({"name": formula["name"], "version": version,
                                "license": formula.get("license"), "homepage": formula.get("homepage")})
        target = destination / formula["name"]
        target.mkdir(exist_ok=True)
        for item in formula_root.iterdir():
            if item.is_file() and any(word in item.name.upper() for word in ("LICENSE", "COPYING", "NOTICE", "AUTHORS")):
                shutil.copy2(item, target / item.name)

    # Homebrew's Qt framework bottles omit the source LICENSES directory.
    # Read the exact release from the copied framework, rather than qtpaths or
    # the formula's latest release, and replace stale notices on every package.
    qt_version = bundled_qt_version(app)
    source_tag = "v" + qt_version
    base_url = "https://raw.githubusercontent.com/qt/qtbase/" + source_tag + "/LICENSES/"
    texts = {}
    for name in ("LGPL-3.0-only", "GPL-2.0-only", "GPL-3.0-only", "Qt-GPL-exception-1.0"):
        with urllib.request.urlopen(base_url + name + ".txt", timeout=30) as response:
            texts[name] = response.read()
        if not texts[name]:
            raise RuntimeError(f"Empty Qt license response: {name} ({source_tag})")
    qt_target = destination / "qt"
    qt_target.mkdir(exist_ok=True)
    for name, content in texts.items():
        (qt_target / (name + ".txt")).write_bytes(content)
    (qt_target / "source.json").write_text(json.dumps({"version": qt_version, "tag": source_tag,
        "url": "https://github.com/qt/qtbase/tree/" + source_tag + "/LICENSES"}, indent=2) + "\n")
    (destination / "libraries.json").write_text(json.dumps(license_summary, ensure_ascii=False, indent=2) + "\n")
    print(f"Collected notices for {len(license_summary)} installed dependency formulae; Qt {qt_version}")


if __name__ == "__main__":
    collect(pathlib.Path(sys.argv[1]))
